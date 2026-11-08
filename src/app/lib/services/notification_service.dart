import 'dart:convert';

import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/material.dart';
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

import '../alarm/fall_alarm_screen.dart';
import '../alarm/fall_alert.dart';

/// Top-level FCM background message handler. MUST be a top-level (or static)
/// function annotated with `@pragma('vm:entry-point')` because it runs in a
/// separate background isolate.
///
/// Kept intentionally minimal and safe: the incoming push already carries a
/// `notification` block, so Android renders it on the high-importance
/// `fall_alerts` full-screen-intent channel without any work here. Tapping it
/// routes through [FirebaseMessaging.onMessageOpenedApp] once the app opens, so
/// there is nothing to do (and no Firebase calls to make) in this isolate.
@pragma('vm:entry-point')
Future<void> fallSenaFirebaseMessagingBackgroundHandler(
  RemoteMessage message,
) async {
  // No-op by design. Do not touch Firebase/UI from the background isolate.
}

/// Receives fall-alert pushes and turns them into a prominent full-screen alarm.
///
/// Responsibilities (task #P3-C1):
///  - Create the Android `fall_alerts` notification channel (id MUST match the
///    Cloud Function in `functions/src/index.ts`) with max importance +
///    sound/vibration, so a push rings like an alarm.
///  - Wire the three FCM states: foreground ([FirebaseMessaging.onMessage]),
///    background tap ([FirebaseMessaging.onMessageOpenedApp]) and terminated
///    launch ([FirebaseMessaging.getInitialMessage]).
///  - Route to [FallAlarmScreen] via a global [navigatorKey].
class NotificationService {
  NotificationService({
    FlutterLocalNotificationsPlugin? localNotifications,
    FirebaseMessaging? messaging,
  })  : _local = localNotifications ?? FlutterLocalNotificationsPlugin(),
        _messaging = messaging ?? FirebaseMessaging.instance;

  final FlutterLocalNotificationsPlugin _local;
  final FirebaseMessaging _messaging;

  /// Global navigator used to push the alarm screen from FCM callbacks (which
  /// have no [BuildContext]). Attached to the app's [MaterialApp].
  final GlobalKey<NavigatorState> navigatorKey = GlobalKey<NavigatorState>();

  /// Guards against stacking multiple alarm screens if a foreground push and a
  /// notification tap both try to route.
  bool _alarmVisible = false;

  /// The `fall_alerts` channel. Its id is the exact contract with the Cloud
  /// Function's Android `channelId`. Max importance + sound + vibration is what
  /// turns the push into a loud, break-through alarm.
  static const AndroidNotificationChannel _fallAlertsChannel =
      AndroidNotificationChannel(
    'fall_alerts',
    'Fall alerts',
    description:
        'High-priority alarms when a linked person may have fallen.',
    importance: Importance.max,
    playSound: true,
    enableVibration: true,
  );

  /// Initializes local notifications, creates the channel, and subscribes to
  /// the FCM message streams. Call once at startup after Firebase init.
  Future<void> init() async {
    const androidInit =
        AndroidInitializationSettings('@mipmap/ic_launcher');
    const darwinInit = DarwinInitializationSettings();
    const initSettings = InitializationSettings(
      android: androidInit,
      iOS: darwinInit,
    );
    await _local.initialize(
      settings: initSettings,
      onDidReceiveNotificationResponse: _onLocalNotificationTap,
    );

    // Create the high-importance full-screen-intent channel up front so the
    // very first push already rings correctly.
    await _local
        .resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>()
        ?.createNotificationChannel(_fallAlertsChannel);

    // Foreground: Android does not auto-display FCM notifications, so we show a
    // local notification (for the alarm sound/heads-up) and route to the alarm.
    FirebaseMessaging.onMessage.listen(_onForegroundMessage);

    // Background tap: user tapped the system notification while app was alive
    // in the background.
    FirebaseMessaging.onMessageOpenedApp.listen(_onMessageOpenedApp);

    // Terminated launch: app was started by tapping the notification.
    final initialMessage = await _messaging.getInitialMessage();
    if (initialMessage != null) {
      _routeToAlarm(FallAlert.fromData(initialMessage.data));
    }
  }

  void _onForegroundMessage(RemoteMessage message) {
    final alert = FallAlert.fromData(message.data);
    _showLocalAlarmNotification(message, alert);
    _routeToAlarm(alert);
  }

  void _onMessageOpenedApp(RemoteMessage message) {
    _routeToAlarm(FallAlert.fromData(message.data));
  }

  /// Tapping the foreground local notification re-opens the alarm using the
  /// payload we stored on it.
  void _onLocalNotificationTap(NotificationResponse response) {
    final payload = response.payload;
    if (payload == null || payload.isEmpty) return;
    try {
      final data = (jsonDecode(payload) as Map).cast<String, dynamic>();
      _routeToAlarm(FallAlert.fromData(data));
    } catch (_) {
      // Malformed payload — ignore rather than crash.
    }
  }

  /// Displays a `fall_alerts` local notification for a FOREGROUND message.
  /// `fullScreenIntent: true` asks the OS to present it as a full-screen alarm.
  void _showLocalAlarmNotification(RemoteMessage message, FallAlert alert) {
    final notification = message.notification;
    final androidDetails = AndroidNotificationDetails(
      _fallAlertsChannel.id,
      _fallAlertsChannel.name,
      channelDescription: _fallAlertsChannel.description,
      importance: Importance.max,
      priority: Priority.max,
      category: AndroidNotificationCategory.alarm,
      fullScreenIntent: true,
      playSound: true,
      enableVibration: true,
      visibility: NotificationVisibility.public,
    );
    const darwinDetails = DarwinNotificationDetails(
      presentAlert: true,
      presentSound: true,
      presentBanner: true,
      interruptionLevel: InterruptionLevel.timeSensitive,
    );
    _local.show(
      id: alert.eventId?.hashCode ??
          DateTime.now().millisecondsSinceEpoch.remainder(1 << 31),
      title: notification?.title ?? 'Fall detected',
      body: notification?.body ??
          '${alert.fallerName} may have fallen — tap to respond',
      notificationDetails:
          NotificationDetails(android: androidDetails, iOS: darwinDetails),
      payload: jsonEncode(alert.toData()),
    );
  }

  /// Pushes [FallAlarmScreen]. Idempotent while an alarm is already visible so
  /// a foreground push + its notification tap don't stack two screens. If the
  /// navigator isn't ready yet (e.g. terminated launch), defer to the next
  /// frame.
  void _routeToAlarm(FallAlert alert) {
    final nav = navigatorKey.currentState;
    if (nav == null) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _routeToAlarm(alert));
      return;
    }
    if (_alarmVisible) return;
    _alarmVisible = true;
    nav
        .push(
          MaterialPageRoute<void>(
            builder: (_) => FallAlarmScreen(alert: alert),
            fullscreenDialog: true,
          ),
        )
        .whenComplete(() => _alarmVisible = false);
  }
}
