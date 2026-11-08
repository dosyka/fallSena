import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/foundation.dart';

/// How long after `lastSeen` a device whose raw `online` is still `true` is
/// treated as stale (i.e. shown OFFLINE). Chosen as ~2× the ~30s status relay
/// heartbeat so a single missed heartbeat doesn't flip the indicator, matching
/// the DATA_MODEL.md "Online / lastSeen semantics" rule.
const Duration kDeviceStalenessThreshold = Duration(seconds: 90);

/// Read-only view of a `devices/{deviceId}` doc's live status, for the
/// caretaker monitoring screen (backlog #P3-C2b). Fields mirror DATA_MODEL.md
/// `devices/{deviceId}`; the factory tolerates missing fields since a device
/// doc may exist before any status relay has run.
class DeviceStatus {
  const DeviceStatus({
    required this.deviceId,
    required this.name,
    required this.batteryPct,
    required this.charging,
    required this.online,
    required this.lastSeen,
  });

  final String deviceId;
  final String name;
  final int? batteryPct;
  final bool charging;

  /// The raw `online` flag written by the faller app on BLE connect/disconnect.
  final bool online;
  final Timestamp? lastSeen;

  /// Whether the device should be shown as online right now. Raw `online` is
  /// trusted only while `lastSeen` is fresh: a killed faller app can't write
  /// `online == false`, so a stale `lastSeen` (older than
  /// [kDeviceStalenessThreshold]) is treated as OFFLINE.
  bool get isCurrentlyOnline {
    if (!online) return false;
    final seen = lastSeen;
    if (seen == null) return false;
    final age = DateTime.now().difference(seen.toDate());
    return age <= kDeviceStalenessThreshold;
  }

  factory DeviceStatus.fromDoc(
    DocumentSnapshot<Map<String, dynamic>> doc,
  ) {
    final data = doc.data() ?? const <String, dynamic>{};
    return DeviceStatus(
      deviceId: (data['deviceId'] ?? doc.id) as String,
      name: (data['name'] ?? 'FallSena') as String,
      batteryPct: (data['batteryPct'] as num?)?.toInt(),
      charging: (data['charging'] ?? false) as bool,
      online: (data['online'] ?? false) as bool,
      lastSeen: data['lastSeen'] as Timestamp?,
    );
  }
}

/// Firestore write path for the faller side of the notification pipeline
/// (backlog #P3-U2a). Owns the `devices/{deviceId}` and `events/{eventId}`
/// collections described in DATA_MODEL.md.
///
/// The `events` create rule calls `deviceOwner(deviceId)`, which `get()`s
/// `devices/{deviceId}` — so a device doc owned by the faller MUST exist before
/// any event write, or the create is rejected. Callers must [ensureDevice]
/// before [reportFall].
class DeviceRepository {
  DeviceRepository({
    FirebaseFirestore? firestore,
    FirebaseAuth? auth,
  })  : _firestore = firestore ?? FirebaseFirestore.instance,
        _auth = auth ?? FirebaseAuth.instance;

  final FirebaseFirestore _firestore;
  final FirebaseAuth _auth;

  /// The signed-in faller's UID.
  String get uid => _auth.currentUser?.uid ?? '';

  DocumentReference<Map<String, dynamic>> _deviceRef(String deviceId) =>
      _firestore.collection('devices').doc(deviceId);

  /// Streams the live [DeviceStatus] of every device owned by [ownerId], for
  /// the caretaker monitoring screen (backlog #P3-C2b).
  ///
  /// A single equality filter (`ownerId == ownerId`) — no composite index
  /// needed. This is exactly the query the `devices` read rule permits for a
  /// caretaker with an *accepted* link to `ownerId`
  /// (`isSelf(ownerId) || isLinkedCaretaker(ownerId)`); see firestore.rules.
  Stream<List<DeviceStatus>> watchDevicesForOwner(String ownerId) {
    return _firestore
        .collection('devices')
        .where('ownerId', isEqualTo: ownerId)
        .snapshots()
        .map((snap) => snap.docs.map(DeviceStatus.fromDoc).toList());
  }

  /// Ensures a `devices/{deviceId}` doc exists for this faller. Idempotent per
  /// wearable: reads first and only creates when the doc is absent, so an
  /// existing `createdAt` is never clobbered.
  ///
  /// The created doc satisfies `allow create: isSelf(ownerId)` — see
  /// DATA_MODEL.md `devices/{deviceId}`.
  Future<void> ensureDevice({
    required String deviceId,
    required String name,
    required String bleId,
  }) async {
    final ref = _deviceRef(deviceId);
    final snapshot = await ref.get();
    if (snapshot.exists) return;

    await ref.set({
      'deviceId': deviceId,
      'ownerId': uid,
      'name': name,
      'bleId': bleId,
      'createdAt': FieldValue.serverTimestamp(),
    });
  }

  /// Relays the wearable's live status into the `devices/{deviceId}` doc so a
  /// linked caretaker can see it cloud-relayed (backlog #P3-C2a). Writes ONLY
  /// the status fields defined in DATA_MODEL.md — `batteryPct` (0..100),
  /// `charging`, `online` and `lastSeen` — and NEVER `ownerId`, so the update
  /// rule's ownership-immutability check
  /// (`request.resource.data.ownerId == resource.data.ownerId`) passes.
  ///
  /// `lastSeen` is always refreshed with the server timestamp; it drives the
  /// caretaker's staleness heuristic (see DATA_MODEL.md "Online / lastSeen
  /// semantics"), so a heartbeat should call this periodically while connected.
  ///
  /// Uses `.update()` (not `.set`) so it fails loudly if the doc does not exist
  /// yet — [ensureDevice] always runs first, so it will. A write failure
  /// rethrows to the caller, matching the class style; callers must treat a
  /// status write as non-fatal.
  Future<void> updateStatus({
    required String deviceId,
    int? batteryPct,
    bool? online,
    bool charging = false,
  }) async {
    final data = <String, dynamic>{
      'charging': charging,
      'lastSeen': FieldValue.serverTimestamp(),
    };
    if (batteryPct != null) data['batteryPct'] = batteryPct;
    if (online != null) data['online'] = online;
    await _deviceRef(deviceId).update(data);
  }

  /// Writes a confirmed-fall `events/{eventId}` doc — the `onCreate` trigger for
  /// the notification relay. Carries the minimum the create rule and
  /// DATA_MODEL.md require; `acknowledgedBy`/`acknowledgedAt` are absent at
  /// create.
  ///
  /// Returns the new event's document id.
  Future<String> reportFall({required String deviceId}) async {
    debugPrint(
        '[BGDELIVER] ${DateTime.now().toIso8601String()} reportFall entry (deviceId=$deviceId, uid=$uid)');
    try {
      final ref = await _firestore.collection('events').add({
        'userId': uid,
        'deviceId': deviceId,
        'type': 'fall_confirmed',
        'status': 'new',
        'ts': FieldValue.serverTimestamp(),
      });
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} reportFall wrote eventId=${ref.id}');
      return ref.id;
    } catch (e) {
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} reportFall error: $e');
      rethrow;
    }
  }
}
