/**
 * Offline unit tests for the fall-event push relay (P3-B2).
 *
 * `firebase-admin` is fully mocked (firestore queries + messaging) so no
 * credentials, live project, or emulator are touched. `firebase-functions-test`
 * runs in offline mode purely to wrap/invoke the real v2 trigger export.
 */

// --- Mock firebase-admin BEFORE importing the function under test. ---
const messagingSend = jest.fn();
let firestoreImpl: () => unknown = () => {
  throw new Error('firestore mock not configured');
};

jest.mock('firebase-admin', () => ({
  // Non-empty so the module-load `initializeApp()` guard is skipped.
  apps: [{}],
  initializeApp: jest.fn(),
  firestore: () => firestoreImpl(),
  messaging: () => ({ sendEachForMulticast: messagingSend }),
}));

// Offline functions-test (no project args = offline mode).
import functionsTestInit from 'firebase-functions-test';
const testEnv = functionsTestInit();

import { relayFallEvent, onFallEventCreated } from './index';

// ---------------------------------------------------------------------------
// Firestore mock builder.
// ---------------------------------------------------------------------------
interface FakeDbOpts {
  /** faller displayName; undefined → user doc does not exist. */
  displayName?: string;
  /** accepted-link docs to return from the links query. */
  links: Array<{ caretakerId: string }>;
  /** caretakerId → token strings held in users/{id}/fcmTokens. */
  tokens: Record<string, string[]>;
}

function makeDb(opts: FakeDbOpts) {
  const deleted: string[] = [];

  const usersCollection = {
    doc: (uid: string) => ({
      get: async () => ({
        exists: opts.displayName !== undefined,
        data: () => ({ displayName: opts.displayName }),
      }),
      collection: (_sub: string) => {
        const toks = opts.tokens[uid] ?? [];
        return {
          get: async () => ({
            docs: toks.map((t) => ({
              id: t,
              ref: {
                delete: async () => {
                  deleted.push(t);
                },
              },
            })),
          }),
        };
      },
    }),
  };

  const linksQuery: any = {
    where: () => linksQuery,
    get: async () => ({
      empty: opts.links.length === 0,
      docs: opts.links.map((l) => ({ data: () => l })),
    }),
  };

  const db = {
    collection: (name: string) => {
      if (name === 'users') return usersCollection;
      if (name === 'links') return linksQuery;
      throw new Error(`unexpected collection: ${name}`);
    },
  };

  return { db, deleted };
}

/** Build a plain DocumentSnapshot-shaped object for the event. */
function makeEventSnapshot(data: Record<string, unknown>, id = 'evt_1') {
  return {
    id,
    exists: true,
    data: () => data,
  } as unknown as FirebaseFirestore.DocumentSnapshot;
}

const BASE_EVENT = {
  eventId: 'evt_1',
  userId: 'faller_1',
  deviceId: 'dev_9',
  type: 'fall_confirmed',
  ts: new Date('2026-07-03T10:00:00.000Z'),
  status: 'new',
};

beforeEach(() => {
  messagingSend.mockReset();
  firestoreImpl = () => {
    throw new Error('firestore mock not configured');
  };
});

afterAll(() => {
  testEnv.cleanup();
});

describe('relayFallEvent', () => {
  it('sends a correctly-shaped high-priority multicast to one accepted caretaker with N tokens', async () => {
    const { db } = makeDb({
      displayName: 'Dad',
      links: [{ caretakerId: 'care_1' }],
      tokens: { care_1: ['tokA', 'tokB', 'tokC'] },
    });
    firestoreImpl = () => db;
    messagingSend.mockResolvedValue({
      successCount: 3,
      failureCount: 0,
      responses: [{ success: true }, { success: true }, { success: true }],
    });

    await relayFallEvent(makeEventSnapshot(BASE_EVENT));

    expect(messagingSend).toHaveBeenCalledTimes(1);
    const msg = messagingSend.mock.calls[0][0];

    // Tokens fanned out to every caretaker token.
    expect(msg.tokens).toEqual(['tokA', 'tokB', 'tokC']);

    // Data payload per DATA_MODEL notification contract (all strings).
    expect(msg.data).toEqual({
      eventId: 'evt_1',
      fallerId: 'faller_1',
      fallerName: 'Dad',
      deviceId: 'dev_9',
      type: 'fall_confirmed',
      ts: '2026-07-03T10:00:00.000Z',
    });
    for (const v of Object.values(msg.data)) {
      expect(typeof v).toBe('string');
    }

    // Visible notification text.
    expect(msg.notification.title).toBe('Fall detected');
    expect(msg.notification.body).toContain('Dad');

    // High-priority flags (Android + APNs).
    expect(msg.android.priority).toBe('high');
    expect(msg.android.notification.channelId).toBe('fall_alerts');
    expect(msg.android.notification.priority).toBe('max');
    expect(msg.apns.headers['apns-priority']).toBe('10');
    expect(msg.apns.payload.aps['interruption-level']).toBe('time-sensitive');
    expect(msg.apns.payload.aps.sound).toBe('default');
  });

  it('does not send when there are no accepted caretakers (only pending links exist)', async () => {
    // The function queries status == "accepted", so a pending-only faller yields
    // an empty result set here.
    const { db } = makeDb({
      displayName: 'Mom',
      links: [],
      tokens: {},
    });
    firestoreImpl = () => db;

    await relayFallEvent(makeEventSnapshot({ ...BASE_EVENT, userId: 'faller_2' }));

    expect(messagingSend).not.toHaveBeenCalled();
  });

  it('does not send when the accepted caretaker holds zero tokens', async () => {
    const { db } = makeDb({
      displayName: 'Gran',
      links: [{ caretakerId: 'care_empty' }],
      tokens: { care_empty: [] },
    });
    firestoreImpl = () => db;

    await relayFallEvent(makeEventSnapshot(BASE_EVENT));

    expect(messagingSend).not.toHaveBeenCalled();
  });

  it('prunes a token whose send returns registration-token-not-registered', async () => {
    const { db, deleted } = makeDb({
      displayName: 'Dad',
      links: [{ caretakerId: 'care_1' }],
      tokens: { care_1: ['goodTok', 'deadTok'] },
    });
    firestoreImpl = () => db;
    messagingSend.mockResolvedValue({
      successCount: 1,
      failureCount: 1,
      responses: [
        { success: true },
        {
          success: false,
          error: { code: 'messaging/registration-token-not-registered' },
        },
      ],
    });

    await relayFallEvent(makeEventSnapshot(BASE_EVENT));

    expect(messagingSend).toHaveBeenCalledTimes(1);
    // Only the dead token doc is deleted; the good one is kept.
    expect(deleted).toEqual(['deadTok']);
  });

  it('also prunes invalid-registration-token errors and keeps other-error tokens', async () => {
    const { db, deleted } = makeDb({
      displayName: 'Dad',
      links: [{ caretakerId: 'care_1' }],
      tokens: { care_1: ['invalidTok', 'transientTok'] },
    });
    firestoreImpl = () => db;
    messagingSend.mockResolvedValue({
      successCount: 0,
      failureCount: 2,
      responses: [
        {
          success: false,
          error: { code: 'messaging/invalid-registration-token' },
        },
        // A non-token error (e.g. server unavailable) must NOT prune.
        { success: false, error: { code: 'messaging/internal-error' } },
      ],
    });

    await relayFallEvent(makeEventSnapshot(BASE_EVENT));

    expect(deleted).toEqual(['invalidTok']);
  });

  it('returns early without throwing on a missing/nonexistent snapshot', async () => {
    await expect(relayFallEvent(undefined)).resolves.toBeUndefined();
    expect(messagingSend).not.toHaveBeenCalled();
  });
});

describe('onFallEventCreated trigger (v2 handler invoked via .run)', () => {
  it('invokes the relay for a created event doc', async () => {
    const { db } = makeDb({
      displayName: 'Dad',
      links: [{ caretakerId: 'care_1' }],
      tokens: { care_1: ['tokA'] },
    });
    firestoreImpl = () => db;
    messagingSend.mockResolvedValue({
      successCount: 1,
      failureCount: 0,
      responses: [{ success: true }],
    });

    // Invoke the real v2 trigger handler via `.run()`. (We can't use
    // `testEnv.wrap()` here because its Firestore encoder needs the real
    // firebase-admin, which this suite mocks; `.run` calls the handler directly.)
    await (onFallEventCreated as any).run({
      data: makeEventSnapshot(BASE_EVENT),
      params: { eventId: 'evt_1' },
    });

    expect(messagingSend).toHaveBeenCalledTimes(1);
    expect(messagingSend.mock.calls[0][0].tokens).toEqual(['tokA']);
  });
});
