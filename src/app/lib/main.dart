import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_messaging/firebase_messaging.dart';
import 'package:flutter/material.dart';

import 'auth/auth_gate.dart';
import 'firebase_options.dart';
import 'services/device_connection_manager.dart';
import 'services/grace_notification_service.dart';
import 'services/notification_service.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await Firebase.initializeApp(
    options: DefaultFirebaseOptions.currentPlatform,
  );

  // Register the top-level FCM background handler before runApp (required by
  // firebase_messaging so pushes that arrive while the app is backgrounded /
  // terminated are handled).
  FirebaseMessaging.onBackgroundMessage(
    fallSenaFirebaseMessagingBackgroundHandler,
  );

  // Set up the fall_alerts channel + foreground/tap/terminated routing.
  final notificationService = NotificationService();
  await notificationService.init();

  // Set up the faller's grace_check full-screen-intent channel, then hand the
  // initialized service to the connection manager so it can force-open the app
  // when a grace window starts while backgrounded/locked (#P3-USERCHECK-APP-2).
  final graceNotificationService = GraceNotificationService();
  await graceNotificationService.init();
  DeviceConnectionManager.instance.graceNotifications = graceNotificationService;

  // Present the faller's "Are you OK?" grace screen whenever the wearable enters
  // its grace window (Fall-Event `2`). Reuses the same global navigator key so
  // it can be shown over whatever is on screen (#P3-USERCHECK-APP-1).
  DeviceConnectionManager.instance
      .attachGraceObserver(notificationService.navigatorKey);

  runApp(FallSenaApp(navigatorKey: notificationService.navigatorKey));
}

class FallSenaApp extends StatelessWidget {
  const FallSenaApp({super.key, this.navigatorKey});

  /// Global navigator key used by [NotificationService] to open the fall alarm
  /// from FCM callbacks (which have no [BuildContext]).
  final GlobalKey<NavigatorState>? navigatorKey;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'FallSena',
      navigatorKey: navigatorKey,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.teal),
        useMaterial3: true,
      ),
      home: const AuthGate(),
    );
  }
}
