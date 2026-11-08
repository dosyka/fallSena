/**
 * FallSena Cloud Functions — fall-event push relay (task P3-B2).
 *
 * A Firestore-triggered function fires when the faller app writes a new
 * `events/{eventId}` doc. It resolves the faller's *accepted* caretaker links,
 * gathers every caretaker FCM token, and sends a single high-priority multicast
 * push per the "Notification data contract" in `src/app/DATA_MODEL.md`.
 *
 * Free-tier note: this is written and unit-tested only. Deploying an onCreate
 * Firestore trigger needs the Blaze plan, so this is NOT deployed here.
 */
import { onDocumentCreated } from 'firebase-functions/v2/firestore';
import * as admin from 'firebase-admin';

if (admin.apps.length === 0) {
  admin.initializeApp();
}

/** Full-screen-intent Android channel — must match the app's channel id. */
const FALL_ALERTS_CHANNEL = 'fall_alerts';

/** Send-response error codes that mean a token is dead and should be pruned. */
const DEAD_TOKEN_CODES = new Set([
  'messaging/registration-token-not-registered',
  'messaging/invalid-registration-token',
]);

/** A token string paired with the Firestore doc it came from (for pruning). */
interface TokenRef {
  token: string;
  ref: FirebaseFirestore.DocumentReference;
}

/** Normalize an event `ts` field (Firestore Timestamp | Date | undefined) to ISO-8601. */
function tsToIso(ts: unknown): string {
  if (ts && typeof (ts as { toDate?: unknown }).toDate === 'function') {
    return (ts as { toDate: () => Date }).toDate().toISOString();
  }
  if (ts instanceof Date) {
    return ts.toISOString();
  }
  return new Date().toISOString();
}

/**
 * Core relay logic, split out from the trigger wiring so it is unit-testable
 * offline. Never throws on the documented edge cases (no accepted caretakers,
 * caretaker with zero tokens, empty token list) — it simply returns early.
 */
export async function relayFallEvent(
  snapshot: FirebaseFirestore.DocumentSnapshot | undefined,
): Promise<void> {
  if (!snapshot || !snapshot.exists) {
    return;
  }

  const event = snapshot.data() ?? {};
  const eventId = snapshot.id;
  // Field names per DATA_MODEL.md → events/{eventId}.
  const fallerId: string | undefined = event.userId;
  const deviceId: string = event.deviceId ?? '';
  const type: string = event.type ?? '';

  if (!fallerId) {
    return;
  }

  const db = admin.firestore();

  // 2. Look up the faller's display name for the alert text.
  const userSnap = await db.collection('users').doc(fallerId).get();
  const fallerName: string =
    (userSnap.exists && (userSnap.data()?.displayName as string)) || 'Someone';

  // 3. Find accepted caretaker links for this faller.
  const linksSnap = await db
    .collection('links')
    .where('fallerId', '==', fallerId)
    .where('status', '==', 'accepted')
    .get();

  if (linksSnap.empty) {
    return; // no accepted caretakers → nothing to send
  }

  const caretakerIds: string[] = linksSnap.docs
    .map((doc) => doc.data().caretakerId as string)
    .filter((id): id is string => Boolean(id));

  // 4. Gather every FCM token across all linked caretakers.
  const tokenRefs: TokenRef[] = [];
  for (const caretakerId of caretakerIds) {
    const tokensSnap = await db
      .collection('users')
      .doc(caretakerId)
      .collection('fcmTokens')
      .get();
    for (const tokenDoc of tokensSnap.docs) {
      tokenRefs.push({ token: tokenDoc.id, ref: tokenDoc.ref });
    }
  }

  if (tokenRefs.length === 0) {
    return; // caretaker(s) with zero tokens → skip the send
  }

  const tokens = tokenRefs.map((t) => t.token);

  // 5. Build the high-priority multicast message per the notification contract.
  const message: admin.messaging.MulticastMessage = {
    tokens,
    notification: {
      title: 'Fall detected',
      body: `${fallerName} may have fallen — tap to respond`,
    },
    // FCM data values must all be strings.
    data: {
      eventId,
      fallerId,
      fallerName,
      deviceId,
      type,
      ts: tsToIso(event.ts),
    },
    android: {
      priority: 'high',
      notification: {
        // Delivered to a full-screen-intent channel so it rings even if the
        // app is killed (see DATA_MODEL "Platform priority flags").
        channelId: FALL_ALERTS_CHANNEL,
        priority: 'max',
        visibility: 'public',
        defaultSound: true,
      },
    },
    apns: {
      headers: {
        'apns-priority': '10',
      },
      payload: {
        aps: {
          sound: 'default',
          // iOS 15+ — pierces Focus/silent.
          'interruption-level': 'time-sensitive',
        },
      },
    },
  };

  const response = await admin.messaging().sendEachForMulticast(message);

  // 6. Prune dead tokens: delete the fcmTokens doc for any unregistered/invalid token.
  const deletions: Promise<unknown>[] = [];
  response.responses.forEach((resp, idx) => {
    if (!resp.success && resp.error && DEAD_TOKEN_CODES.has(resp.error.code)) {
      deletions.push(tokenRefs[idx].ref.delete());
    }
  });
  await Promise.all(deletions);
}

/**
 * Firestore onCreate trigger for `events/{eventId}`. Thin wrapper around
 * {@link relayFallEvent}. (Deploy requires Blaze — not deployed in P3-B2.)
 */
export const onFallEventCreated = onDocumentCreated(
  'events/{eventId}',
  async (event) => {
    await relayFallEvent(event.data);
  },
);
