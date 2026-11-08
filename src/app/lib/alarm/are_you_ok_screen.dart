import 'package:flutter/material.dart';

/// Full-screen "Are you OK?" prompt shown to the FALLER while the wearable is in
/// its grace/cancel window (Fall-Event value `2`, backlog #P3-USERCHECK-APP-1).
///
/// A suspected fall was detected on the device. Unless the wearer confirms they
/// are OK before the grace window elapses, the wearable escalates to a confirmed
/// fall (Fall-Event `1`) and the caretaker is alerted.
///
/// The only way to cancel is a deliberate HOLD-to-confirm: the user must press
/// AND HOLD for [_holdDuration] (a progress ring fills while holding; releasing
/// early resets it). A single tap does NOT dismiss it — this is a deliberate
/// anti-misinput guard so a stray touch after a real fall can't silence the
/// alert. The system back button is blocked too ([PopScope] `canPop: false`).
///
/// On hold-complete it calls [onConfirmOk] (which cancels the countdown on the
/// device). Presenting/dismissing this screen is driven by the live Fall-Event
/// value elsewhere — that value-driven dismiss is the source of truth — so the
/// optimistic pop here is just a courtesy.
class AreYouOkScreen extends StatefulWidget {
  const AreYouOkScreen({super.key, required this.onConfirmOk});

  /// Called once when the hold-to-confirm completes. Typically wired to the
  /// connection manager's `cancelFallCountdown()`.
  final Future<void> Function() onConfirmOk;

  @override
  State<AreYouOkScreen> createState() => _AreYouOkScreenState();
}

class _AreYouOkScreenState extends State<AreYouOkScreen>
    with SingleTickerProviderStateMixin {
  /// How long the user must hold to confirm "I'm OK".
  static const Duration _holdDuration = Duration(seconds: 3);

  late final AnimationController _controller;
  bool _confirmed = false;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(vsync: this, duration: _holdDuration)
      ..addStatusListener(_onStatus)
      ..addListener(() => setState(() {}));
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onStatus(AnimationStatus status) {
    if (status == AnimationStatus.completed) {
      _onHoldComplete();
    }
  }

  void _startHold() {
    if (_confirmed) return;
    // Continue filling from wherever the ring currently sits.
    _controller.forward();
  }

  void _cancelHold() {
    if (_confirmed) return;
    // Released before completing — reset the ring so the user must hold again.
    _controller.reset();
  }

  Future<void> _onHoldComplete() async {
    if (_confirmed) return;
    _confirmed = true;
    setState(() {});
    await widget.onConfirmOk();
    if (!mounted) return;
    // Optimistic pop; the value-driven dismiss is the real source of truth and
    // is idempotent, so double-dismiss is harmless.
    final nav = Navigator.of(context);
    if (nav.canPop()) nav.pop();
  }

  @override
  Widget build(BuildContext context) {
    const onAmber = Colors.white;

    return PopScope(
      // Block the system back button — the only way out is the deliberate hold.
      canPop: false,
      child: Scaffold(
        backgroundColor: const Color(0xFFE8830C),
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Spacer(),
                const Icon(Icons.help_outline, size: 96, color: onAmber),
                const SizedBox(height: 16),
                const Text(
                  'Are you OK?',
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    color: onAmber,
                    fontSize: 40,
                    fontWeight: FontWeight.w900,
                    letterSpacing: 1,
                  ),
                ),
                const SizedBox(height: 24),
                const Text(
                  'A possible fall was detected. Your caretaker will be '
                  'alerted unless you confirm you are OK.',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: onAmber, fontSize: 18),
                ),
                const Spacer(),
                _HoldToCancelButton(
                  progress: _controller.value,
                  confirmed: _confirmed,
                  onHoldStart: _startHold,
                  onHoldEnd: _cancelHold,
                ),
                const SizedBox(height: 16),
                const Text(
                  'Press and hold for 3 seconds',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: onAmber, fontSize: 16),
                ),
                const SizedBox(height: 24),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// The hold-to-confirm control: a large filling ring around a label. Press and
/// hold to fill; release before full to reset.
class _HoldToCancelButton extends StatelessWidget {
  const _HoldToCancelButton({
    required this.progress,
    required this.confirmed,
    required this.onHoldStart,
    required this.onHoldEnd,
  });

  final double progress;
  final bool confirmed;
  final VoidCallback onHoldStart;
  final VoidCallback onHoldEnd;

  @override
  Widget build(BuildContext context) {
    const onAmber = Colors.white;
    return Center(
      child: GestureDetector(
        // A plain tap must NOT dismiss — only a sustained hold does. We wire the
        // raw press-down/press-up/cancel events so releasing early resets.
        onTapDown: (_) => onHoldStart(),
        onTapUp: (_) => onHoldEnd(),
        onTapCancel: onHoldEnd,
        child: SizedBox(
          width: 180,
          height: 180,
          child: Stack(
            alignment: Alignment.center,
            children: [
              SizedBox(
                width: 180,
                height: 180,
                child: CircularProgressIndicator(
                  value: progress,
                  strokeWidth: 10,
                  backgroundColor: Colors.white24,
                  valueColor: const AlwaysStoppedAnimation<Color>(onAmber),
                ),
              ),
              Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(
                    confirmed ? Icons.check_circle : Icons.touch_app,
                    size: 40,
                    color: onAmber,
                  ),
                  const SizedBox(height: 8),
                  Text(
                    confirmed ? "OK sent" : "I'm OK",
                    style: const TextStyle(
                      color: onAmber,
                      fontSize: 22,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}
