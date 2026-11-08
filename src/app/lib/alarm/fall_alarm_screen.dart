import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import 'fall_alert.dart';

/// Prominent full-screen RED alarm shown when a fall-alert push arrives.
///
/// Two actions:
///  - **Acknowledge / Respond** — writes the caretaker ack to
///    `events/{eventId}` (status `new` → `acknowledged`, stamps
///    `acknowledgedBy` = current uid + `acknowledgedAt`) per DATA_MODEL.md and
///    the scoped `firestore.rules` caretaker-ack rule. When the push has no
///    `eventId` (e.g. a manual console test push) it no-ops gracefully and just
///    dismisses — it must never crash.
///  - **Dismiss** — closes the alarm without writing anything.
class FallAlarmScreen extends StatefulWidget {
  const FallAlarmScreen({
    super.key,
    required this.alert,
    this.firestore,
    this.auth,
  });

  final FallAlert alert;

  /// Optional injection points (tests); default to the singletons at runtime.
  final FirebaseFirestore? firestore;
  final FirebaseAuth? auth;

  @override
  State<FallAlarmScreen> createState() => _FallAlarmScreenState();
}

class _FallAlarmScreenState extends State<FallAlarmScreen> {
  bool _working = false;

  FirebaseFirestore get _firestore =>
      widget.firestore ?? FirebaseFirestore.instance;
  FirebaseAuth get _auth => widget.auth ?? FirebaseAuth.instance;

  /// Writes the caretaker acknowledgement. Conforms to the `events` update rule:
  /// only advances `status` new→acknowledged and stamps `acknowledgedBy` (self)
  /// + `acknowledgedAt`; all other event fields are left untouched (a partial
  /// `update()`), so the rule's immutability checks pass.
  Future<void> _acknowledge() async {
    final alert = widget.alert;

    // Manual/console test push with no eventId → nothing to ack. Dismiss.
    if (!alert.hasEventId) {
      _close();
      return;
    }

    final uid = _auth.currentUser?.uid;
    if (uid == null) {
      // Not signed in — can't (and rules won't) write. Just dismiss.
      _close();
      return;
    }

    setState(() => _working = true);
    try {
      await _firestore.collection('events').doc(alert.eventId!).update({
        'status': 'acknowledged',
        'acknowledgedBy': uid,
        'acknowledgedAt': FieldValue.serverTimestamp(),
      });
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Response recorded')),
      );
      _close();
    } catch (e) {
      if (!mounted) return;
      setState(() => _working = false);
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Could not record response: $e')),
      );
    }
  }

  void _close() {
    final nav = Navigator.of(context);
    if (nav.canPop()) nav.pop();
  }

  String _formatTime(DateTime? ts) {
    if (ts == null) return 'Time unknown';
    final local = ts.toLocal();
    String two(int n) => n.toString().padLeft(2, '0');
    return '${local.year}-${two(local.month)}-${two(local.day)} '
        '${two(local.hour)}:${two(local.minute)}';
  }

  @override
  Widget build(BuildContext context) {
    final alert = widget.alert;
    const onRed = Colors.white;

    return PopScope(
      // Don't let a stray back gesture silently dismiss a safety alarm.
      canPop: false,
      child: Scaffold(
        backgroundColor: const Color(0xFFB00020),
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Spacer(),
                const Icon(Icons.warning_amber_rounded,
                    size: 96, color: onRed),
                const SizedBox(height: 16),
                const Text(
                  'FALL DETECTED',
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    color: onRed,
                    fontSize: 40,
                    fontWeight: FontWeight.w900,
                    letterSpacing: 2,
                  ),
                ),
                const SizedBox(height: 24),
                Text(
                  alert.fallerName,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    color: onRed,
                    fontSize: 28,
                    fontWeight: FontWeight.bold,
                  ),
                ),
                const SizedBox(height: 12),
                if (alert.deviceId != null)
                  Text(
                    'Device: ${alert.deviceId}',
                    textAlign: TextAlign.center,
                    style: const TextStyle(color: onRed, fontSize: 16),
                  ),
                const SizedBox(height: 4),
                Text(
                  _formatTime(alert.ts),
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: onRed, fontSize: 16),
                ),
                const Spacer(),
                SizedBox(
                  height: 60,
                  child: FilledButton.icon(
                    style: FilledButton.styleFrom(
                      backgroundColor: onRed,
                      foregroundColor: const Color(0xFFB00020),
                      textStyle: const TextStyle(
                        fontSize: 20,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    onPressed: _working ? null : _acknowledge,
                    icon: _working
                        ? const SizedBox(
                            width: 22,
                            height: 22,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.check_circle),
                    label: const Text('Acknowledge / Respond'),
                  ),
                ),
                const SizedBox(height: 12),
                SizedBox(
                  height: 52,
                  child: OutlinedButton(
                    style: OutlinedButton.styleFrom(
                      foregroundColor: onRed,
                      side: const BorderSide(color: onRed, width: 1.5),
                      textStyle: const TextStyle(fontSize: 18),
                    ),
                    onPressed: _working ? null : _close,
                    child: const Text('Dismiss'),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
