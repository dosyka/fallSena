import 'package:fallsena/device/device_screen.dart';
import 'package:fallsena/services/device_ble_service.dart';
import 'package:fallsena/services/device_repository.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_test/flutter_test.dart';

/// Regression test for #P3-BGLIFE.
///
/// Leaving the faller [DeviceScreen] (navigation / backgrounding, i.e. its
/// `dispose()`) must NOT tear down the app-scoped BLE connection or fall
/// reporting. Teardown lives in exactly one place —
/// `DeviceConnectionManager.disconnect()` — reached only on an explicit user
/// "Disconnect". This bug (screen dispose killing the connection) was
/// previously real and painful, so this locks in the guarantee.
///
/// When [DeviceScreen] is given an injected `service`/`repository` it builds a
/// LOCAL manager over those fakes (its test seam), so we can observe whether
/// disposing the screen calls through to `service.disconnect()`.

/// Fake [DeviceBleService] that never touches the real BLE stack. It inherits
/// the reactive [ValueNotifier]s the manager reads, records how many times
/// [disconnect] is called, and serves inert scan/isScanning streams instead of
/// the `FlutterBluePlus` platform-backed ones.
class _FakeDeviceBleService extends DeviceBleService {
  int disconnectCount = 0;

  @override
  Future<void> disconnect() async {
    disconnectCount++;
  }

  // Override the two plugin-backed streams so the screen's StreamBuilders never
  // reach into FlutterBluePlus (which would hit an absent platform channel).
  @override
  Stream<List<ScanResult>> get scanResults => const Stream.empty();

  @override
  Stream<bool> get isScanning => Stream<bool>.value(false);
}

/// Fake [DeviceRepository] via `implements` so the real Firebase-touching
/// constructor never runs. It is not exercised in this test (nothing calls
/// `connect`), so every member is a stub.
class _FakeDeviceRepository implements DeviceRepository {
  @override
  String get uid => throw UnimplementedError();

  @override
  Stream<List<DeviceStatus>> watchDevicesForOwner(String ownerId) =>
      throw UnimplementedError();

  @override
  Future<void> ensureDevice({
    required String deviceId,
    required String name,
    required String bleId,
  }) =>
      throw UnimplementedError();

  @override
  Future<void> updateStatus({
    required String deviceId,
    int? batteryPct,
    bool? online,
    bool charging = false,
  }) =>
      throw UnimplementedError();

  @override
  Future<String> reportFall({required String deviceId}) =>
      throw UnimplementedError();
}

void main() {
  final binding = TestWidgetsFlutterBinding.ensureInitialized();

  // Belt-and-suspenders for the platform-channel gotcha: DeviceScreen.initState
  // calls manager.init(), which calls BleForegroundService.init(). That is a
  // no-op off Android (the test host), but if the plugin's method channel is
  // ever reached we stub it here with STANDARD flutter_test — no mocking
  // package — so the harness returns null instead of throwing MissingPlugin.
  const foregroundTaskChannel = MethodChannel('flutter_foreground_task/methods');

  setUp(() {
    binding.defaultBinaryMessenger.setMockMethodCallHandler(
      foregroundTaskChannel,
      (call) async => null,
    );
  });

  tearDown(() {
    binding.defaultBinaryMessenger.setMockMethodCallHandler(
      foregroundTaskChannel,
      null,
    );
  });

  testWidgets(
    'disposing DeviceScreen (navigating away) does NOT disconnect BLE',
    (tester) async {
      final fakeBle = _FakeDeviceBleService();
      final fakeRepo = _FakeDeviceRepository();

      // Mount the screen with injected fakes -> it builds a LOCAL manager over
      // them (its test seam), so service.disconnect() reflects real teardown.
      await tester.pumpWidget(
        MaterialApp(
          home: DeviceScreen(service: fakeBle, repository: fakeRepo),
        ),
      );
      await tester.pumpAndSettle();

      expect(find.text('Device'), findsOneWidget);
      expect(fakeBle.disconnectCount, 0);

      // Replace the whole app with a different widget so DeviceScreen leaves the
      // tree and its dispose() runs — this simulates navigating away /
      // backgrounding off the screen.
      await tester.pumpWidget(
        const MaterialApp(home: Scaffold(body: Text('elsewhere'))),
      );
      await tester.pumpAndSettle();

      // The screen is gone...
      expect(find.text('Device'), findsNothing);
      expect(find.text('elsewhere'), findsOneWidget);

      // ...and the core regression assertion: leaving the screen did NOT tear
      // down the connection. Only an explicit disconnect() would, and nothing
      // called it.
      expect(
        fakeBle.disconnectCount,
        0,
        reason: 'DeviceScreen.dispose() must not disconnect the app-scoped BLE '
            'connection; only DeviceConnectionManager.disconnect() may.',
      );
    },
  );
}
