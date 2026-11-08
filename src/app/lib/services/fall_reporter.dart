import 'package:flutter/foundation.dart';

import 'device_ble_service.dart';
import 'device_repository.dart';

/// Bridges the BLE Fall-Event characteristic to the Firestore write path
/// (backlog #P3-U2a). Kept free of any UI/Flutter-widget dependency so the
/// future background service (#P3-U2b) can reuse it verbatim.
///
/// The Fall-Event characteristic latches at `1` until the device is cleared, so
/// this only writes ONE event per fall: it fires on the rising edge into `1`
/// (previous value not `1`, new value `1`) and re-arms only after the value
/// returns to a non-`1` state.
class FallReporter {
  FallReporter({
    required this.bleService,
    required this.repository,
    required this.deviceId,
  });

  final DeviceBleService bleService;
  final DeviceRepository repository;
  final String deviceId;

  /// The id of the most recently written event, for the UI to surface that a
  /// report was sent. `null` until the first successful report.
  final ValueNotifier<String?> lastReportedEventId = ValueNotifier(null);

  /// The last error from a failed Firestore write, if any. Exposed so the UI can
  /// surface a failure without the listener crashing.
  final ValueNotifier<Object?> lastError = ValueNotifier(null);

  /// Whether the reporter is currently listening for fall events.
  bool get isActive => _started;

  bool _started = false;
  bool _armed = true;
  bool _disposed = false;

  /// Starts listening to [DeviceBleService.fallEvent]. Idempotent — a second
  /// call while already started is a no-op.
  void start() {
    if (_started) return;
    _started = true;
    _armed = true;
    bleService.fallEvent.addListener(_onFallEvent);
    // A ValueNotifier listener does NOT fire for the value already present, so
    // if the Fall-Event characteristic is already latched at `1` when reporting
    // starts (e.g. reconnecting to a device with an unacknowledged fall), the
    // fall would be missed. Evaluate the current value once through the SAME
    // rising-edge/dedupe path — it reports at most once and disarms, so the
    // listener can't also double-report the same latched value.
    _onFallEvent();
  }

  void _onFallEvent() {
    final value = bleService.fallEvent.value;
    if (value == 1) {
      // Rising edge into a latched fall: report once, then disarm until the
      // characteristic returns to a non-1 value.
      if (_armed) {
        debugPrint(
            '[BGDELIVER] ${DateTime.now().toIso8601String()} FallReporter rising-edge detected (armed) -> reporting');
        _armed = false;
        _report();
      } else {
        debugPrint(
            '[BGDELIVER] ${DateTime.now().toIso8601String()} FallReporter report suppressed(disarmed)');
      }
    } else {
      // Any non-`1` value re-arms for the next fall. This covers `0`/`null`
      // (cleared) AND `2` (suspected/in-grace — the wearable's cancel window).
      // Value `2` deliberately does NOT create a Firestore event: only the
      // rising edge into `1` reports. So `0→2→1` reports exactly once (the `2`
      // just keeps us armed, the `1` fires) and `0→2→0` reports zero (the fall
      // was cancelled during grace and `1` is never reached).
      _armed = true;
    }
  }

  Future<void> _report() async {
    debugPrint(
        '[BGDELIVER] ${DateTime.now().toIso8601String()} FallReporter write-attempt (deviceId=$deviceId)');
    try {
      final eventId = await repository.reportFall(deviceId: deviceId);
      if (_disposed) return;
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} FallReporter write-success(eventId=$eventId)');
      lastReportedEventId.value = eventId;
      lastError.value = null;
    } catch (e) {
      // Never let a write failure tear down the listener.
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} FallReporter write-failure(error): $e');
      if (_disposed) return;
      lastError.value = e;
    }
  }

  /// Stops listening. May be restarted with [start].
  void stop() {
    if (!_started) return;
    _started = false;
    bleService.fallEvent.removeListener(_onFallEvent);
  }

  /// Releases all resources. Call from the owning widget's `dispose`.
  void dispose() {
    stop();
    _disposed = true;
    lastReportedEventId.dispose();
    lastError.dispose();
  }
}
