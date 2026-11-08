import 'package:flutter/material.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

/// Owns a dedicated full-screen-intent notification used to FORCE-OPEN the app
/// when the wearable enters its grace/cancel window (Fall-Event `2`) while the
/// app is backgrounded or the device is locked (#P3-USERCHECK-APP-2).
///
/// The foreground half (#P3-USERCHECK-APP-1) already pushes the in-app
/// [AreYouOkScreen] from [DeviceConnectionManager] the moment Fall-Event hits
/// `2`. But pushing a Flutter route does NOT bring a backgrounded/locked app to
/// the foreground or wake the screen. This service posts a high-importance,
/// `fullScreenIntent: true` local notification: the OS then launches
/// `MainActivity`, and the existing grace observer presents the "Are you OK?"
/// screen over the lock screen.
///
/// It intentionally owns its OWN [FlutterLocalNotificationsPlugin] rather than
/// reaching into [NotificationService]'s private plugin — the caretaker
/// `fall_alerts` channel/flow is a separate concern and must not be touched.
/// This mirrors that caretaker full-screen-intent pattern (Importance.max,
/// `category: alarm`, `fullScreenIntent: true`).
class GraceNotificationService {
  GraceNotificationService({
    FlutterLocalNotificationsPlugin? localNotifications,
  }) : _local = localNotifications ?? FlutterLocalNotificationsPlugin();

  final FlutterLocalNotificationsPlugin _local;

  /// Fixed notification id so [show] is de-duplicated and [cancel] can target
  /// exactly this notification without disturbing the caretaker alarm.
  static const int _notificationId = 90201;

  /// The dedicated grace-check channel. Separate from the caretaker
  /// `fall_alerts` channel so the two concerns never collide. Max importance +
  /// sound + vibration is what turns this into a break-through, wake-the-screen
  /// alarm.
  static const AndroidNotificationChannel _graceChannel =
      AndroidNotificationChannel(
    'grace_check',
    'Fall check',
    description:
        'Wakes the screen to ask if you are OK after a possible fall.',
    importance: Importance.max,
    playSound: true,
    enableVibration: true,
  );

  /// Whether [show] has posted the notification without a subsequent [cancel].
  /// Used by the manager to avoid re-posting on every lifecycle transition.
  bool get isShowing => _showing;
  bool _showing = false;

  /// Initializes local notifications, creates the grace channel, and requests
  /// the Android 14+ full-screen-intent runtime permission. Call once at
  /// startup. No-op UI off Android.
  Future<void> init() async {
    const androidInit =
        AndroidInitializationSettings('@mipmap/ic_launcher');
    const darwinInit = DarwinInitializationSettings();
    const initSettings = InitializationSettings(
      android: androidInit,
      iOS: darwinInit,
    );
    await _local.initialize(settings: initSettings);

    final android = _local.resolvePlatformSpecificImplementation<
        AndroidFlutterLocalNotificationsPlugin>();
    if (android == null) return;

    await android.createNotificationChannel(_graceChannel);

    // Android 14+ (API 34) revokes USE_FULL_SCREEN_INTENT for apps the Play
    // Store doesn't classify as calling/alarm apps, so request it at runtime.
    // Present in flutter_local_notifications 22.0.1; guarded in try/catch so a
    // future version that drops/renames it degrades gracefully rather than
    // crashing startup. Never adds a dependency or bumps versions.
    try {
      await android.requestFullScreenIntentPermission();
    } catch (e) {
      debugPrint('[GRACE] requestFullScreenIntentPermission skipped: $e');
    }
  }

  /// Posts the full-screen-intent grace notification. The OS uses the
  /// full-screen intent to launch `MainActivity`; the manager's grace observer
  /// then presents [AreYouOkScreen]. Idempotent-friendly: reuses a fixed id so
  /// re-posting replaces rather than stacks.
  Future<void> show() async {
    final androidDetails = AndroidNotificationDetails(
      _graceChannel.id,
      _graceChannel.name,
      channelDescription: _graceChannel.description,
      importance: Importance.max,
      priority: Priority.max,
      category: AndroidNotificationCategory.alarm,
      fullScreenIntent: true,
      playSound: true,
      enableVibration: true,
      visibility: NotificationVisibility.public,
      ongoing: true,
    );
    const darwinDetails = DarwinNotificationDetails(
      presentAlert: true,
      presentSound: true,
      presentBanner: true,
      interruptionLevel: InterruptionLevel.timeSensitive,
    );
    _showing = true;
    await _local.show(
      id: _notificationId,
      title: 'Are you OK?',
      body: 'A possible fall was detected. Open the app to confirm you are OK.',
      notificationDetails:
          NotificationDetails(android: androidDetails, iOS: darwinDetails),
    );
  }

  /// Cancels the grace notification (grace window ended: cancelled, confirmed
  /// or disconnected). Safe to call when nothing is showing.
  Future<void> cancel() async {
    _showing = false;
    await _local.cancel(id: _notificationId);
  }
}
