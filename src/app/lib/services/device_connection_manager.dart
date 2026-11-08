import 'package:firebase_core/firebase_core.dart';
import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../alarm/are_you_ok_screen.dart';
import 'ble_foreground_service.dart';
import 'device_ble_service.dart';
import 'device_repository.dart';
import 'device_status_reporter.dart';
import 'fall_reporter.dart';
import 'grace_notification_service.dart';

/// App-scoped, long-lived owner of the wearable connection + fall reporting.
///
/// This exists to fix the lifecycle bug where [DeviceScreen] owned the whole
/// BLE connection: because the screen created the [DeviceBleService],
/// [FallReporter], [DeviceStatusReporter] and [BleForegroundService] in
/// `initState` and tore them all down in `dispose`, navigating away from (or in
/// some cases backgrounding off) the dashboard killed the BLE link and stopped
/// fall reporting, so caretakers stopped getting alerts.
///
/// Ownership is hoisted here so the connection + reporting survive screen
/// navigation and app backgrounding. Teardown happens in EXACTLY ONE place —
/// [disconnect] — which is only reached on an explicit user disconnect.
/// [DeviceScreen] is now a pure view over this manager's observables.
///
/// This matches the codebase's plain-service + [ValueNotifier] style (no
/// Provider/Riverpod): use the process-wide [instance] in production; a local
/// instance can be constructed with injected collaborators for tests.
///
/// TODO(app-kill survival): reporting still does NOT survive a full app KILL
/// (process death / swiped from recents). flutter_blue_plus drives BLE from the
/// main isolate only, so we cannot move the connection into the
/// foreground-service isolate. The foreground service ([BleForegroundService])
/// only keeps the process (and therefore this manager) alive while backgrounded;
/// once the process is killed, the connection is gone until the user reconnects.
class DeviceConnectionManager with WidgetsBindingObserver {
  DeviceConnectionManager({
    DeviceBleService? service,
    DeviceRepository? repository,
    BleForegroundService? foregroundService,
    GraceNotificationService? graceNotifications,
  })  : service = service ?? DeviceBleService(),
        repository = repository ?? DeviceRepository(),
        _foregroundService = foregroundService ?? BleForegroundService(),
        _graceNotifications =
            graceNotifications ?? GraceNotificationService();

  /// The process-wide connection owner used in production. Long-lived: created
  /// once and never disposed for the life of the app.
  static final DeviceConnectionManager instance = DeviceConnectionManager();

  /// The single BLE central. Owned here, never disposed by the screen.
  final DeviceBleService service;

  /// The single Firestore device repository.
  final DeviceRepository repository;

  final BleForegroundService _foregroundService;

  /// Force-opens the app with a full-screen-intent notification while the app
  /// is backgrounded/locked during a grace window (#P3-USERCHECK-APP-2).
  ///
  /// Constructor-injectable for tests. The production [instance] is built with
  /// a default (uninitialized) service; [main] creates + `init()`s the real one
  /// and hands it in via [graceNotifications] so `init()` runs exactly once.
  GraceNotificationService _graceNotifications;

  /// Replaces the grace-notification service. Used by [main] to inject the
  /// already-initialized service into the singleton [instance].
  set graceNotifications(GraceNotificationService service) =>
      _graceNotifications = service;

  FallReporter? _reporter;
  DeviceStatusReporter? _statusReporter;
  bool _initialized = false;

  // --- Foreground "Are you OK?" grace observer (#P3-USERCHECK-APP-1) ---

  /// Global navigator used to present the "Are you OK?" screen from this
  /// no-BuildContext manager. Set via [attachGraceObserver].
  GlobalKey<NavigatorState>? _navigatorKey;

  /// Whether the fall-event grace observer has been attached (idempotent).
  bool _graceObserverAttached = false;

  /// The currently-presented "Are you OK?" route, if any. Doubles as the
  /// duplicate-present guard: non-null means the screen is already up.
  Route<void>? _areYouOkRoute;

  /// Last-observed app lifecycle state, tracked so grace logic can decide
  /// whether to force-open the app (backgrounded/locked) vs rely on the in-app
  /// route (foreground). Seeded from the binding's known state.
  AppLifecycleState? _lifecycleState =
      WidgetsBinding.instance.lifecycleState;

  bool get _isResumed => _lifecycleState == AppLifecycleState.resumed;

  // --- Observable state the screen renders ---

  /// Live connection state (reuses the underlying [DeviceBleService] notifier).
  ValueNotifier<DeviceConnectionState> get connectionState =>
      service.connectionState;

  /// Live battery percent (reuses the underlying [DeviceBleService] notifier).
  ValueNotifier<int?> get batteryPercent => service.batteryPercent;

  /// Live fall-event value (reuses the underlying [DeviceBleService] notifier).
  ValueNotifier<int?> get fallEvent => service.fallEvent;

  /// Id of the most recently reported fall event. Stable across reporter
  /// recreation (forwarded from the current [FallReporter]); `null` until the
  /// first successful report.
  final ValueNotifier<String?> lastReportedEventId = ValueNotifier(null);

  /// Whether fall reporting is currently active (a live [FallReporter] +
  /// [DeviceStatusReporter] are running).
  final ValueNotifier<bool> reportingActive = ValueNotifier(false);

  /// User-facing message for Bluetooth-off / permission / connect / reporting
  /// errors (including the #P3-ERRMSG `ensureDevice` branching). `null` = none.
  final ValueNotifier<String?> message = ValueNotifier(null);

  /// Live stream of scan results (already filtered to the FallSena device).
  Stream<List<ScanResult>> get scanResults => service.scanResults;

  /// Whether a scan is currently running.
  Stream<bool> get isScanning => service.isScanning;

  /// Prepares the Android foreground service (notification channel + runtime
  /// permission). Idempotent — safe to call more than once. No-op off Android.
  Future<void> init() async {
    if (_initialized) return;
    _initialized = true;
    // Observe app lifecycle purely for [BGDELIVER] correlation — logs when the
    // app is foregrounded/backgrounded so on-device traces can be lined up
    // against the fall-delivery legs. Does NOT change any behavior.
    WidgetsBinding.instance.addObserver(this);
    await _foregroundService.init();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    debugPrint(
        '[BGDELIVER] ${DateTime.now().toIso8601String()} lifecycle -> $state');
    _lifecycleState = state;
    // Close the gap where the grace window was entered while foregrounded (so
    // only the in-app route was shown) and the user THEN backgrounds/locks the
    // app: force-open it via the full-screen intent so the "Are you OK?" prompt
    // still reaches them (#P3-USERCHECK-APP-2).
    if (state != AppLifecycleState.resumed &&
        fallEvent.value == 2 &&
        !_graceNotifications.isShowing) {
      _graceNotifications.show();
    }
  }

  /// Starts a filtered BLE scan. Sets [message] on permission/adapter failure.
  Future<void> startScan() async {
    message.value = null;
    final ready = await service.ensurePermissions();
    if (!ready) {
      message.value =
          'Bluetooth is off or unavailable. Turn it on and try again.';
      return;
    }
    try {
      await service.startScan();
    } catch (_) {
      message.value = 'Could not start scanning. Check Bluetooth permissions.';
    }
  }

  /// Stops an in-progress scan.
  Future<void> stopScan() => service.stopScan();

  /// Connects to [device] and starts fall reporting.
  ///
  /// Idempotent: if already connected with a live reporter, this is a no-op so
  /// it cannot create duplicate reporters. Preserves the original ordering —
  /// BLE connect, then `ensureDevice` (awaited) BEFORE starting the
  /// [FallReporter]/[DeviceStatusReporter], then the foreground service.
  Future<void> connect(BluetoothDevice device) async {
    if (connectionState.value == DeviceConnectionState.connected &&
        _reporter != null) {
      return;
    }
    message.value = null;
    try {
      await service.connect(device);
    } catch (_) {
      message.value = 'Could not connect to the device.';
      return;
    }
    await _startReporting(device);
  }

  /// Ensures the Firestore `devices/{deviceId}` doc exists (required before any
  /// event write — the events create rule reads the device's ownerId), THEN
  /// starts a [FallReporter] to relay confirmed falls and a
  /// [DeviceStatusReporter] to relay live status, and promotes the process to a
  /// foreground service so reporting survives backgrounding.
  Future<void> _startReporting(BluetoothDevice device) async {
    final deviceId = device.remoteId.str;
    final bleId = device.remoteId.str;
    final name = device.advName.isNotEmpty
        ? device.advName
        : DeviceBleService.deviceName;

    _teardownReporter();
    try {
      await repository.ensureDevice(
        deviceId: deviceId,
        name: name,
        bleId: bleId,
      );
    } catch (e) {
      debugPrint('ensureDevice failed: $e');
      message.value = _ensureDeviceErrorMessage(e);
      return;
    }

    final reporter = FallReporter(
      bleService: service,
      repository: repository,
      deviceId: deviceId,
    )..start();
    reporter.lastReportedEventId.addListener(_onReported);
    _reporter = reporter;

    // Relay the wearable's live status (battery/online/lastSeen) into the
    // device doc so a linked caretaker can see it cloud-relayed (#P3-C2a).
    final statusReporter = DeviceStatusReporter(
      bleService: service,
      repository: repository,
      deviceId: deviceId,
    )..start();
    _statusReporter = statusReporter;

    reportingActive.value = true;

    // Reporting is now live — promote the process to an Android foreground
    // service so the BLE link + FallReporter (both on the main isolate) survive
    // the app being backgrounded (#P3-U2b). No-op off Android.
    //
    // A foreground-service start failure is NON-FATAL: reporting stays logically
    // active (works while foregrounded), but we surface a clear warning + log
    // because monitoring may stop once the app is backgrounded. The failure is
    // captured as a bool by [BleForegroundService.start] and never thrown.
    final fgsStarted = await _foregroundService.start(
      title: 'FallSena is monitoring for falls',
      body: 'Connected to your wearable. Caretakers are alerted on a fall.',
    );
    if (!fgsStarted) {
      const warning =
          'Monitoring may stop when the app is in the background '
          '(background service failed to start).';
      message.value = warning;
      debugPrint(
          '[BGDELIVER] ${DateTime.now().toIso8601String()} $warning');
    }
  }

  /// The #P3-ERRMSG permission-denied / unavailable / other branching, surfaced
  /// as a user-facing message when `ensureDevice` fails.
  String _ensureDeviceErrorMessage(Object e) {
    if (e is FirebaseException) {
      switch (e.code) {
        case 'permission-denied':
          return 'This wearable is registered to a different account, or '
              "you're not signed in. Sign in with the owning account, or "
              'remove the device from Firebase, then reconnect.';
        case 'unavailable':
          return 'Connected, but could not reach the server to enable fall '
              'reporting. Check your internet connection and reconnect.';
        default:
          return 'Connected, but could not enable fall reporting '
              '(error: ${e.code}). Please reconnect.';
      }
    }
    return 'Connected, but could not enable fall reporting ($e). Please '
        'reconnect.';
  }

  void _onReported() {
    lastReportedEventId.value = _reporter?.lastReportedEventId.value;
  }

  /// Cancels the wearable's active grace countdown by writing the cancel command
  /// over BLE (used by the "Are you OK?" hold-to-confirm). Never throws: a write
  /// failure is surfaced via [message] instead of crashing the caller.
  Future<void> cancelFallCountdown() async {
    try {
      await service.writeCancelCommand();
    } catch (e) {
      debugPrint('cancelFallCountdown failed: $e');
      message.value =
          "Couldn't tell the device you're OK. If it keeps buzzing, use the "
          'button on the wearable to cancel.';
    }
  }

  // --- Foreground "Are you OK?" grace observer ---

  /// Attaches the fall-event observer that presents/dismisses the "Are you OK?"
  /// screen as the wearable enters/leaves its grace window (Fall-Event `2`).
  ///
  /// Wired here (not in [DeviceScreen], which isn't always mounted) because this
  /// manager is app-scoped. Reuses the app's global [navigatorKey] so the screen
  /// can be presented over whatever is on screen. Idempotent.
  void attachGraceObserver(GlobalKey<NavigatorState> navigatorKey) {
    if (_graceObserverAttached) return;
    _graceObserverAttached = true;
    _navigatorKey = navigatorKey;
    fallEvent.addListener(_onGraceFallEvent);
    // Evaluate the current value once in case we're already in a grace window.
    _onGraceFallEvent();
  }

  /// Value-driven present/dismiss: `2` = in grace → present; anything else
  /// (`0` cancelled, `1` confirmed, `null` disconnected) → dismiss. This is the
  /// source of truth, so it's safe for the screen to also optimistically pop.
  void _onGraceFallEvent() {
    if (fallEvent.value == 2) {
      // Foreground half (#P3-USERCHECK-APP-1): show the in-app route.
      _presentAreYouOk();
      // Background half (#P3-USERCHECK-APP-2): only force-open with the
      // full-screen intent when the app is NOT foregrounded, so a resumed app
      // doesn't fire a redundant alarm notification on top of the route.
      if (!_isResumed) _graceNotifications.show();
    } else {
      // Grace ended (0 cancelled / 1 confirmed / null disconnected): dismiss
      // the route AND clear the force-open notification.
      _dismissAreYouOk();
      _graceNotifications.cancel();
    }
  }

  void _presentAreYouOk() {
    final nav = _navigatorKey?.currentState;
    if (nav == null) {
      // Navigator not ready yet — retry next frame, re-checking the value so a
      // grace window that ended in the meantime doesn't present a stale screen.
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (fallEvent.value == 2) _presentAreYouOk();
      });
      return;
    }
    // Guard against stacking duplicates (mirrors NotificationService).
    if (_areYouOkRoute != null) return;
    final route = MaterialPageRoute<void>(
      builder: (_) => AreYouOkScreen(onConfirmOk: cancelFallCountdown),
      fullscreenDialog: true,
    );
    _areYouOkRoute = route;
    nav.push(route).whenComplete(() {
      if (identical(_areYouOkRoute, route)) _areYouOkRoute = null;
    });
  }

  void _dismissAreYouOk() {
    final route = _areYouOkRoute;
    if (route == null) return;
    _areYouOkRoute = null;
    // removeRoute (rather than pop) so it dismisses even if another route was
    // pushed on top in the meantime, and completes the push future above.
    _navigatorKey?.currentState?.removeRoute(route);
  }

  /// The ONLY place that tears down reporting + the connection. Disposes the
  /// reporters, stops the foreground service and disconnects BLE. Reached only
  /// on an explicit user disconnect — NOT on screen navigation/backgrounding.
  Future<void> disconnect() async {
    _teardownReporter();
    await service.disconnect();
  }

  /// Releases the app-lifecycle observer registered in [init]. The production
  /// [instance] is app-lived and never disposed; this exists so locally
  /// constructed instances (e.g. in tests) don't leak a global observer.
  void dispose() {
    if (_graceObserverAttached) {
      fallEvent.removeListener(_onGraceFallEvent);
      _graceObserverAttached = false;
    }
    if (!_initialized) return;
    WidgetsBinding.instance.removeObserver(this);
  }

  void _teardownReporter() {
    _reporter?.lastReportedEventId.removeListener(_onReported);
    _reporter?.dispose();
    _reporter = null;
    _statusReporter?.dispose();
    _statusReporter = null;
    reportingActive.value = false;
    // Reporting is no longer active — drop the foreground service so the
    // process is no longer held alive in the background. Fire-and-forget: this
    // is called from sync contexts and is idempotent.
    _foregroundService.stop();
  }
}
