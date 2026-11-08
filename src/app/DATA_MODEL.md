# FallSena — Firestore Data Model

This document defines the Firestore (Native mode) schema for the FallSena
Phase 3 mobile app. One Flutter codebase ships two roles — **faller** (user
who wears the device) and **caretaker** (who receives fall alerts). The model
is designed to drive the notification pipeline:

```
[Device] --BLE--> [faller app] --write events/{id}--> [Firestore]
   --> [Cloud Function onCreate] --> [FCM/APNs high-priority push] --> [caretaker app]
```

Latency budget end-to-end: ≤10 s (see PM decision 2026-07-02).

The security rules that enforce this model live in `firestore.rules`. Every
collection below notes the rule block that guards it.

---

## Conventions

- All timestamps are Firestore `timestamp` (server time via
  `FieldValue.serverTimestamp()`), never client clocks, so ordering and the
  latency audit are trustworthy.
- Document IDs: `users/{uid}` uses the Firebase Auth UID. `links/{linkId}` uses
  a **deterministic composite ID** `"{fallerId}_{caretakerId}"` so the security
  rules can `get()` a specific link doc (rules cannot run arbitrary queries).
  `devices` and `events` use auto-generated IDs.
- `role` is a closed enum: `"faller"` | `"caretaker"`. A single account has one
  role for the notification logic; the app selects it at onboarding.

---

## Collection: `users/{uid}`

One doc per authenticated account. `{uid}` == Firebase Auth UID.

| Field         | Type      | Notes |
|---------------|-----------|-------|
| `uid`         | string    | Mirror of the doc ID (convenience for queries/denormalization). |
| `role`        | string    | `"faller"` or `"caretaker"`. |
| `displayName` | string    | Shown in the caretaker's alert ("Dad fell"). |
| `email`       | string    | From Auth (Email/Password). |
| `createdAt`   | timestamp | Server time at account creation. |
| `updatedAt`   | timestamp | Server time at last profile change. |

**Relationships:** a `faller` owns 0..N `devices` (via `devices.ownerId`) and is
the subject of 0..N `events`. A `caretaker` links to fallers via `links`.

**Rules block:** `match /users/{userId}` — a user may read/write **only their own**
doc (`request.auth.uid == userId`). Cross-user display-name lookups for pushes
are done server-side by the Cloud Function (Admin SDK bypasses rules).

### Subcollection: `users/{uid}/fcmTokens/{token}`

FCM registration tokens. **Tokens rotate** — the app must handle refresh, so we
store them as one doc per token (keyed by the token string) rather than a single
field. This lets an account be signed in on multiple devices and lets the Cloud
Function fan out to every current token.

| Field        | Type      | Notes |
|--------------|-----------|-------|
| `token`      | string    | Mirror of doc ID (the FCM token). |
| `platform`   | string    | `"android"` | `"ios"` — for platform-specific push config. |
| `createdAt`  | timestamp | When first registered. |
| `lastSeenAt` | timestamp | Refreshed on each app start / `onTokenRefresh`. |

**Token refresh contract (app side):**
1. On sign-in and on every `FirebaseMessaging.onTokenRefresh`, upsert
   `users/{uid}/fcmTokens/{token}` and update `lastSeenAt`.
2. On sign-out (or when a send fails with `messaging/registration-token-not-registered`),
   delete the stale token doc. The Cloud Function should prune tokens that
   return `unregistered` on send.

**Rules block:** `match /users/{userId}/fcmTokens/{tokenId}` — read/write only by
the owning user.

---

## Collection: `devices/{deviceId}`

One doc per physical FallSena wearable, owned by a faller. Fields mirror what the
firmware exposes over BLE (see `src/firmware/.../ble_service.h`): battery percent
0..100, a charging flag (firmware currently always reports `0` — no charge-detect
GPIO yet), and a fall-event state. The faller app relays these into Firestore so
the caretaker can see status **cloud-relayed** (never via direct BLE).

| Field          | Type      | Notes |
|----------------|-----------|-------|
| `deviceId`     | string    | Mirror of doc ID. |
| `ownerId`      | string    | UID of the owning **faller** (`users/{uid}`). |
| `name`         | string    | User-facing label ("Grandpa's pendant"). Defaults to the BLE name "FallSena". |
| `bleId`        | string    | Platform BLE identifier / advertised name used to reconnect. |
| `batteryPct`   | number    | 0..100, last value relayed from the Battery Level char (0x2A19). |
| `charging`     | boolean   | From Device-Status char byte[1]. Firmware pins this `false` for now. |
| `online`       | boolean   | True while the faller app holds an active BLE link to the device. |
| `lastSeen`     | timestamp | Server time of the last status relay; drives an "offline" heuristic when stale. |
| `createdAt`    | timestamp | When the device was paired to the account. |

**Online / lastSeen semantics:** `online` is set `true` on BLE connect and
`false` on disconnect by the faller app. Because a killed app can't write
`false`, consumers should also treat `online == true` as stale if
`lastSeen` is older than a threshold (e.g. > 2× the status relay interval).

**Relationships:** `ownerId` → `users/{uid}` (role `faller`). Read-visible to any
caretaker with an **accepted** link to `ownerId`.

**Rules block:** `match /devices/{deviceId}` — read by owner OR a linked-accepted
caretaker; create/update/delete only by the owner (`ownerId == request.auth.uid`).

---

## Collection: `links/{linkId}`  (caretaker ↔ faller pairing)

Represents the pairing relationship. **Linking requires both parties**: one party
creates a `pending` link, the other must flip it to `accepted`.

- **Doc ID is deterministic:** `linkId = "{fallerId}_{caretakerId}"`. This both
  prevents duplicate links and lets the rules `get()` the exact link doc when
  authorizing device/event reads.

| Field         | Type      | Notes |
|---------------|-----------|-------|
| `fallerId`    | string    | UID of the faller (subject of alerts). |
| `caretakerId` | string    | UID of the caretaker (receives alerts). |
| `status`      | string    | `"pending"` | `"accepted"`. |
| `createdBy`   | string    | UID that initiated the request (must be `fallerId` or `caretakerId`). |
| `createdAt`   | timestamp | When the request was created. |
| `acceptedAt`  | timestamp | Set when the other party accepts (null while pending). |

**Pairing flow:**
1. Either party creates the link with `status: "pending"`, `createdBy: <self>`.
   `linkId` must equal `"{fallerId}_{caretakerId}"` and `createdBy` must be one of
   the two parties.
2. The **other** party (not `createdBy`) updates `status` → `"accepted"` and sets
   `acceptedAt`. `fallerId`/`caretakerId`/`createdBy` are immutable.
3. Either party may delete the link to unpair.

**Relationships:** joins `users` (faller) ↔ `users` (caretaker). An `accepted`
link is the authorization edge the rules use to grant a caretaker read access to
the faller's `devices` and `events`, and the Cloud Function uses it to find which
caretaker tokens to push to.

**Rules block:** `match /links/{linkId}` — read/create/update/delete only by the
two parties named in the doc, with the accept-transition and immutability
constraints above.

---

## Collection: `events/{eventId}`  (fall events)

One doc per confirmed fall. Written by the faller app the instant the BLE
Fall-Event characteristic notifies `1 = confirmed`. This write is the trigger for
the whole notification pipeline (Cloud Function `onCreate`). Keep it minimal and
fast.

| Field            | Type      | Notes |
|------------------|-----------|-------|
| `eventId`        | string    | Mirror of doc ID. |
| `userId`         | string    | UID of the **faller** (subject). Must equal the writer. |
| `deviceId`       | string    | The `devices/{deviceId}` that fired. Owner of it must be `userId`. |
| `type`           | string    | Event type enum, currently `"fall_confirmed"`. Reserved for future types (e.g. `"fall_test"`, `"manual_sos"`). |
| `ts`             | timestamp | Server time the event was recorded (the latency clock start). |
| `status`         | string    | `"new"` | `"acknowledged"`. Starts `"new"`. |
| `acknowledgedBy` | string    | UID of the caretaker who acknowledged (null while `new`). |
| `acknowledgedAt` | timestamp | Server time of acknowledgement (null while `new`). |

**Acknowledgement:** when a caretaker responds ("I'm on it"), the app flips
`status` → `"acknowledged"` and stamps `acknowledgedBy`/`acknowledgedAt`. Because
several caretakers can watch one faller, recording *who* acknowledged matters.

**Relationships:** `userId` → `users/{uid}` (faller); `deviceId` →
`devices/{deviceId}`. Read-visible to any caretaker with an accepted link to
`userId`.

**Rules block:** `match /events/{eventId}` — create only by the faller for their
own device (`userId == request.auth.uid` AND that device's `ownerId` == the
writer); read by the faller OR a linked-accepted caretaker; the **only** write a
caretaker may make is the narrowly-scoped acknowledge transition (see rules
comments). Events are never deleted from the app.

---

## Notification data contract

This is the exact interface between the three pipeline stages. Getting these
fields right is what lets the Cloud Function build a push without extra reads.

### 1. What the `events/{id}` doc carries (written by the faller app)

Minimum required at create time:

```
userId    : string     // faller UID — who fell
deviceId  : string     // which device fired
type      : string     // "fall_confirmed"
ts        : timestamp  // serverTimestamp()
status    : "new"
```

`acknowledgedBy` / `acknowledgedAt` are absent/null at create.

### 2. What the Cloud Function does (`events` onCreate, Admin SDK)

1. Read the new event → `userId`.
2. `get(users/{userId})` → `displayName` (for the alert text).
3. Query `links` where `fallerId == userId` AND `status == "accepted"` →
   collect `caretakerId`s.
4. For each caretaker, read `users/{caretakerId}/fcmTokens/*` → gather tokens.
5. Send one high-priority multicast push per platform.

### 3. What the push payload needs

**Notification part (visible):**
- title: e.g. `"Fall detected"`
- body: e.g. `"{displayName} may have fallen — tap to respond"`

**Data part (for the app to open the right screen / ack):**
```
eventId   : string
fallerId  : string   (= event.userId)
fallerName: string   (denormalized displayName)
deviceId  : string
type      : string
ts         : string   (ISO-8601; FCM data values must be strings)
```

**Platform priority flags (safety-critical, must break through):**
- **Android:** `priority: "high"`, delivered to a **full-screen-intent**
  notification channel so it rings even if the app is killed.
- **iOS (APNs):** `interruption-level: "time-sensitive"` (iOS 15+) so it pierces
  Focus/silent. True full-screen "ringing" needs the Critical Alerts entitlement
  (Apple approval) — deferred, not an MVP blocker. APNs is wired up in the later
  iOS bring-up pass.
