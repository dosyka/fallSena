import 'dart:io' show Platform;

import 'package:flutter/foundation.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';

/// Thin wrapper around `flutter_foreground_task` (backlog #P3-U2b).
///
/// Its ONLY job is to promote the app's process to an Android foreground
/// service (a persistent, low-importance ongoing notification) so Android keeps
/// the process — and therefore the main isolate, the `flutter_blue_plus` BLE
/// link and the `FallReporter` — alive while the app is backgrounded.
///
/// IMPORTANT: no BLE or reporting work runs in the plugin's background isolate.
/// `flutter_blue_plus` connections can only be driven from the main isolate, so
/// all of that logic stays exactly where it is ([DeviceBleService] /
/// [FallReporter]). The [_BleForegroundTaskHandler] below is intentionally a
/// no-op — we want process priority + the required notification, not work in
/// the service isolate.
///
/// All methods are no-ops on non-Android platforms (iOS is a separate pass).
class BleForegroundService {
  static const String _channelId = 'fallsena_monitoring';
  static const int _serviceId = 5301;

  /// Whether the plugin has been initialised. Guards double-init and lets
  /// [start]/[stop] be safely called even if [init] was skipped.
  bool _initialized = false;

  /// Configures the notification channel/options and requests the runtime
  /// permissions the plugin needs (notifications on Android 13+). Safe to call
  /// more than once; safe (no-op) off Android.
  Future<void> init() async {
    if (!Platform.isAndroid) return;
    if (_initialized) return;

    // Android 13+: the ongoing foreground-service notification needs the
    // POST_NOTIFICATIONS runtime permission to be visible.
    final permission =
        await FlutterForegroundTask.checkNotificationPermission();
    if (permission != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }

    FlutterForegroundTask.init(
      androidNotificationOptions: AndroidNotificationOptions(
        channelId: _channelId,
        channelName: 'Fall monitoring',
        channelDescription:
            'Keeps FallSena connected to the wearable to report falls.',
        // Low importance + no sound/vibration: this is a silent, ongoing status
        // notification, not an alert.
        channelImportance: NotificationChannelImportance.LOW,
        priority: NotificationPriority.LOW,
        onlyAlertOnce: true,
      ),
      iosNotificationOptions: const IOSNotificationOptions(
        showNotification: false,
        playSound: false,
      ),
      foregroundTaskOptions: ForegroundTaskOptions(
        // No repeating work in the service isolate — the notification alone
        // provides the process priority we need.
        eventAction: ForegroundTaskEventAction.nothing(),
        allowWakeLock: true,
      ),
    );

    _initialized = true;
  }

  /// Starts the foreground service with an ongoing notification. Idempotent —
  /// returns `true` (already running) if the service is already up. No-op off
  /// Android (returns `true`).
  ///
  /// Returns `true` when the service is started (or already running), `false`
  /// when the start attempt failed. A failure is logged and NEVER thrown, so a
  /// foreground-service failure cannot tear down the connect flow.
  Future<bool> start({required String title, required String body}) async {
    if (!Platform.isAndroid) return true;
    if (!_initialized) await init();
    if (await FlutterForegroundTask.isRunningService) {
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} FGS start requested: already running');
      return true;
    }

    debugPrint(
        '[BGDELIVER] ${DateTime.now().toIso8601String()} FGS start requested');
    try {
      await FlutterForegroundTask.startService(
        serviceId: _serviceId,
        // BLE link = a connected device (Android 14+ FGS type requirement).
        serviceTypes: const [ForegroundServiceTypes.connectedDevice],
        notificationTitle: title,
        notificationText: body,
        callback: startBleForegroundTaskCallback,
      );
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} FGS started');
      return true;
    } catch (e) {
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} FGS failed(reason): $e');
      return false;
    }
  }

  /// Stops the foreground service. Idempotent — a no-op if not running. No-op
  /// off Android.
  Future<void> stop() async {
    if (!Platform.isAndroid) return;
    if (!await FlutterForegroundTask.isRunningService) return;
    await FlutterForegroundTask.stopService();
  }
}

/// Top-level entry point for the foreground-service isolate. Must be a
/// top-level/static function annotated with `@pragma('vm:entry-point')` so it
/// survives tree-shaking and can be invoked by the plugin.
@pragma('vm:entry-point')
void startBleForegroundTaskCallback() {
  FlutterForegroundTask.setTaskHandler(_BleForegroundTaskHandler());
}

/// Deliberately empty TaskHandler. All BLE + reporting logic lives on the main
/// isolate (see class doc on [BleForegroundService]); this exists only because
/// the plugin requires a handler for the service isolate.
class _BleForegroundTaskHandler extends TaskHandler {
  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {}

  @override
  void onRepeatEvent(DateTime timestamp) {}

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {}
}
