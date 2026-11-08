import 'dart:async';
import 'dart:io' show Platform;

import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:firebase_messaging/firebase_messaging.dart';

/// Manages this device's FCM registration token and the
/// `users/{uid}/fcmTokens/{token}` subcollection described in DATA_MODEL.md.
///
/// Tokens rotate, so we store one doc per token (keyed by the token string).
/// Registration is idempotent — safe to call on every launch while signed in —
/// and no-ops when signed out or when a token can't be obtained (e.g. a device
/// without Play Services, where `getToken()` returns null).
class MessagingService {
  MessagingService({
    FirebaseMessaging? messaging,
    FirebaseFirestore? firestore,
    FirebaseAuth? auth,
  })  : _messaging = messaging ?? FirebaseMessaging.instance,
        _firestore = firestore ?? FirebaseFirestore.instance,
        _auth = auth ?? FirebaseAuth.instance;

  final FirebaseMessaging _messaging;
  final FirebaseFirestore _firestore;
  final FirebaseAuth _auth;

  StreamSubscription<String>? _refreshSub;
  String? _currentToken;

  /// `"android"` | `"ios"` per DATA_MODEL.md's `platform` field.
  String get _platform => Platform.isIOS ? 'ios' : 'android';

  /// Requests notification permission, obtains the current FCM token, upserts
  /// its token doc, and starts listening for token rotation.
  ///
  /// Guarded to run only while signed in. Gracefully skips if no token is
  /// available. Safe to call repeatedly.
  Future<void> registerToken() async {
    final user = _auth.currentUser;
    if (user == null) return;

    // Required on Android 13+ / iOS to receive notifications.
    await _messaging.requestPermission();

    final token = await _messaging.getToken();
    if (token == null) return; // e.g. no Play Services — skip, don't crash.

    _currentToken = token;
    await _writeToken(user.uid, token);

    // Handle rotation: write the new token and prune the old one.
    _refreshSub ??= _messaging.onTokenRefresh.listen((newToken) async {
      final currentUser = _auth.currentUser;
      if (currentUser == null) return;
      final oldToken = _currentToken;
      _currentToken = newToken;
      await _writeToken(currentUser.uid, newToken);
      if (oldToken != null && oldToken != newToken) {
        await _tokenDoc(currentUser.uid, oldToken).delete();
      }
    });
  }

  /// Deletes this device's current token doc so a signed-out device stops
  /// receiving this user's pushes, and stops listening for rotation. Call
  /// before signing out (while `currentUser` is still valid).
  Future<void> unregisterToken() async {
    final user = _auth.currentUser;
    final token = _currentToken ?? await _messaging.getToken();
    if (user != null && token != null) {
      await _tokenDoc(user.uid, token).delete();
    }
    await _refreshSub?.cancel();
    _refreshSub = null;
    _currentToken = null;
  }

  DocumentReference<Map<String, dynamic>> _tokenDoc(String uid, String token) {
    return _firestore
        .collection('users')
        .doc(uid)
        .collection('fcmTokens')
        .doc(token);
  }

  /// Upserts `users/{uid}/fcmTokens/{token}` per DATA_MODEL.md: token (mirror
  /// of doc ID), platform, createdAt (only on first registration), lastSeenAt
  /// (server time, refreshed each call).
  Future<void> _writeToken(String uid, String token) async {
    final doc = _tokenDoc(uid, token);
    final snapshot = await doc.get();
    final data = <String, dynamic>{
      'token': token,
      'platform': _platform,
      'lastSeenAt': FieldValue.serverTimestamp(),
    };
    if (!snapshot.exists) {
      data['createdAt'] = FieldValue.serverTimestamp();
    }
    await doc.set(data, SetOptions(merge: true));
  }
}
