import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';

import 'messaging_service.dart';

/// The two account roles in FallSena. See DATA_MODEL.md: `role` is a closed
/// enum `"faller" | "caretaker"`.
enum UserRole {
  faller,
  caretaker;

  String get wireValue => name;
}

/// Wraps Firebase Auth (email/password) and the `users/{uid}` Firestore doc
/// lifecycle described in DATA_MODEL.md.
class AuthService {
  AuthService({
    FirebaseAuth? auth,
    FirebaseFirestore? firestore,
    MessagingService? messagingService,
  })  : _auth = auth ?? FirebaseAuth.instance,
        _firestore = firestore ?? FirebaseFirestore.instance,
        _messagingService = messagingService ?? MessagingService();

  final FirebaseAuth _auth;
  final FirebaseFirestore _firestore;
  final MessagingService _messagingService;

  /// This device's FCM token / notification registration lifecycle.
  MessagingService get messagingService => _messagingService;

  /// Emits the current user (or null) on every auth state change.
  Stream<User?> authStateChanges() => _auth.authStateChanges();

  User? get currentUser => _auth.currentUser;

  /// Creates an Auth account, then writes the conforming `users/{uid}` doc.
  Future<UserCredential> signUp({
    required String email,
    required String password,
    required String displayName,
    required UserRole role,
  }) async {
    final credential = await _auth.createUserWithEmailAndPassword(
      email: email,
      password: password,
    );

    final user = credential.user;
    if (user != null) {
      await user.updateDisplayName(displayName);
      await _createUserDoc(
        uid: user.uid,
        email: email,
        displayName: displayName,
        role: role,
      );
      // Fire off a verification email, but never let a send failure fail the
      // whole sign-up: the user must still land signed-in.
      try {
        await user.sendEmailVerification();
      } catch (_) {
        // Non-fatal; the user can resend from the home screen banner.
      }
    }
    return credential;
  }

  /// Sends a password-reset email to [email] (Forgot password flow).
  Future<void> sendPasswordResetEmail({required String email}) {
    return _auth.sendPasswordResetEmail(email: email);
  }

  /// Sends an email-verification link to the currently signed-in user.
  Future<void> sendEmailVerification() {
    return _auth.currentUser?.sendEmailVerification() ?? Future.value();
  }

  /// Reloads the current user so [isEmailVerified] reflects server state.
  Future<void> reloadUser() {
    return _auth.currentUser?.reload() ?? Future.value();
  }

  /// Whether the current user's email has been verified.
  bool get isEmailVerified => _auth.currentUser?.emailVerified ?? false;

  Future<UserCredential> signIn({
    required String email,
    required String password,
  }) {
    return _auth.signInWithEmailAndPassword(
      email: email,
      password: password,
    );
  }

  /// Deletes this device's FCM token doc (so it stops receiving this user's
  /// pushes) before clearing the Auth session.
  Future<void> signOut() async {
    await _messagingService.unregisterToken();
    await _auth.signOut();
  }

  /// Writes `users/{uid}` per DATA_MODEL.md: uid, role, displayName, email,
  /// createdAt/updatedAt as server timestamps.
  Future<void> _createUserDoc({
    required String uid,
    required String email,
    required String displayName,
    required UserRole role,
  }) {
    return _firestore.collection('users').doc(uid).set({
      'uid': uid,
      'role': role.wireValue,
      'displayName': displayName,
      'email': email,
      'createdAt': FieldValue.serverTimestamp(),
      'updatedAt': FieldValue.serverTimestamp(),
    });
  }
}
