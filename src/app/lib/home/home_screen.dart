import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import '../device/device_screen.dart';
import '../monitor/monitored_screen.dart';
import '../pairing/pairing_screen.dart';
import '../services/auth_service.dart';

/// Placeholder signed-in landing screen. Real faller/caretaker experiences
/// land here in later tasks; for now it confirms auth works and offers
/// sign-out.
class HomeScreen extends StatefulWidget {
  const HomeScreen({
    super.key,
    required this.authService,
    required this.user,
  });

  final AuthService authService;
  final User user;

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  UserRole? _role;

  /// Purely informational email-verification banner state. Sign-in is never
  /// gated on verification; this only nudges the user.
  bool _emailVerified = false;
  bool _bannerDismissed = false;
  bool _verificationBusy = false;

  @override
  void initState() {
    super.initState();
    // Register this device's FCM token now that we're signed in. Idempotent
    // and self-guarding; failures here must not block the UI.
    widget.authService.messagingService.registerToken();
    _emailVerified = widget.authService.isEmailVerified;
    _loadRole();
  }

  /// Resends the verification email and confirms via SnackBar.
  Future<void> _resendVerification() async {
    setState(() => _verificationBusy = true);
    try {
      await widget.authService.sendEmailVerification();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Verification email sent')),
        );
      }
    } on FirebaseAuthException catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(e.message ?? e.code)),
        );
      }
    } catch (_) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Could not send verification email')),
        );
      }
    } finally {
      if (mounted) setState(() => _verificationBusy = false);
    }
  }

  /// Reloads the user and hides the banner if the email is now verified.
  Future<void> _refreshVerification() async {
    setState(() => _verificationBusy = true);
    try {
      await widget.authService.reloadUser();
      final verified = widget.authService.isEmailVerified;
      if (mounted) {
        setState(() => _emailVerified = verified);
        if (!verified) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(content: Text('Email not verified yet')),
          );
        }
      }
    } catch (_) {
      // Non-fatal; leave the banner as-is.
    } finally {
      if (mounted) setState(() => _verificationBusy = false);
    }
  }

  /// Reads the signed-in user's own `users/{uid}` doc to learn their role,
  /// which decides the primary AppBar action (faller -> Device, caretaker ->
  /// Monitored). Mirrors PairingScreen._loadRole; a load failure leaves [_role]
  /// null so we simply omit the role-specific action.
  Future<void> _loadRole() async {
    try {
      final doc = await FirebaseFirestore.instance
          .collection('users')
          .doc(widget.user.uid)
          .get();
      final roleValue = doc.data()?['role'] as String?;
      final role = UserRole.values.firstWhere(
        (r) => r.wireValue == roleValue,
        orElse: () => UserRole.faller,
      );
      if (mounted) setState(() => _role = role);
    } catch (_) {
      // Leave _role null: show Connections + Sign out only.
    }
  }

  @override
  Widget build(BuildContext context) {
    final name = widget.user.displayName?.trim();
    return Scaffold(
      appBar: AppBar(
        title: const Text('FallSena'),
        actions: [
          if (_role == UserRole.faller)
            IconButton(
              tooltip: 'Device',
              icon: const Icon(Icons.bluetooth),
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute<void>(
                  builder: (_) => const DeviceScreen(),
                ),
              ),
            ),
          if (_role == UserRole.caretaker)
            IconButton(
              tooltip: 'Monitored',
              icon: const Icon(Icons.monitor_heart),
              onPressed: () => Navigator.of(context).push(
                MaterialPageRoute<void>(
                  builder: (_) => MonitoredScreen(user: widget.user),
                ),
              ),
            ),
          IconButton(
            tooltip: 'Connections',
            icon: const Icon(Icons.people),
            onPressed: () => Navigator.of(context).push(
              MaterialPageRoute<void>(
                builder: (_) => PairingScreen(user: widget.user),
              ),
            ),
          ),
          IconButton(
            tooltip: 'Sign out',
            icon: const Icon(Icons.logout),
            onPressed: () => widget.authService.signOut(),
          ),
        ],
      ),
      body: Column(
        children: [
          if (!_emailVerified && !_bannerDismissed) _buildVerificationBanner(),
          Expanded(
            child: Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  const Icon(Icons.check_circle, size: 48),
                  const SizedBox(height: 16),
                  Text(
                    (name != null && name.isNotEmpty)
                        ? 'Signed in as $name'
                        : 'Signed in',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    widget.user.email ?? '',
                    style: Theme.of(context).textTheme.bodyMedium,
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// Non-blocking, dismissible nudge to verify email. Dismiss only hides it for
  /// the session; nothing here gates app access.
  Widget _buildVerificationBanner() {
    return MaterialBanner(
      content: const Text('Please verify your email address.'),
      leading: const Icon(Icons.mark_email_unread_outlined),
      actions: [
        TextButton(
          onPressed: _verificationBusy ? null : _resendVerification,
          child: const Text('Resend'),
        ),
        TextButton(
          onPressed: _verificationBusy ? null : _refreshVerification,
          child: const Text("I've verified"),
        ),
        TextButton(
          onPressed: _verificationBusy
              ? null
              : () => setState(() => _bannerDismissed = true),
          child: const Text('Dismiss'),
        ),
      ],
    );
  }
}
