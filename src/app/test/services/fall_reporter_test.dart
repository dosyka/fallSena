import 'package:fallsena/services/device_ble_service.dart';
import 'package:fallsena/services/device_repository.dart';
import 'package:fallsena/services/fall_reporter.dart';
import 'package:flutter_test/flutter_test.dart';

/// A hand-rolled fake [DeviceRepository] that records [reportFall] calls and
/// returns a canned event id — it never touches Firebase. Implemented via
/// `implements` so the real (Firebase-backed) constructor is never invoked.
class _FakeDeviceRepository implements DeviceRepository {
  final List<String> reportedDeviceIds = [];
  int get reportCount => reportedDeviceIds.length;

  @override
  Future<String> reportFall({required String deviceId}) async {
    reportedDeviceIds.add(deviceId);
    return 'event-${reportedDeviceIds.length}';
  }

  // Unused by FallReporter — never called in these tests.
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
}

void main() {
  late DeviceBleService ble;
  late _FakeDeviceRepository repo;
  late FallReporter reporter;

  setUp(() {
    // Constructing DeviceBleService only creates ValueNotifiers; no BLE stack
    // is touched until connect()/startScan(), which we never call.
    ble = DeviceBleService();
    repo = _FakeDeviceRepository();
    reporter = FallReporter(
      bleService: ble,
      repository: repo,
      deviceId: 'dev1',
    );
    reporter.start();
  });

  tearDown(() {
    reporter.dispose();
    ble.dispose();
  });

  // Drives a fall-event value and lets the (awaited) fake report settle so any
  // ValueNotifier updates in FallReporter complete before the next assertion.
  Future<void> emit(int? value) async {
    ble.fallEvent.value = value;
    await Future<void>.delayed(Duration.zero);
  }

  group('FallReporter rising-edge dedupe', () {
    test('0 -> 1 reports exactly once', () async {
      await emit(0);
      await emit(1);
      expect(repo.reportCount, 1);
      expect(repo.reportedDeviceIds, ['dev1']);
    });

    test('1 -> 1 (latched) suppresses the duplicate report', () async {
      await emit(0);
      await emit(1);
      await emit(1);
      expect(repo.reportCount, 1);
    });

    test('0 -> 2 -> 1 reports exactly once (2 re-arms, 1 fires)', () async {
      await emit(0);
      await emit(2);
      await emit(1);
      expect(repo.reportCount, 1);
    });

    test('0 -> 2 -> 0 reports zero (fall cancelled during grace)', () async {
      await emit(0);
      await emit(2);
      await emit(0);
      expect(repo.reportCount, 0);
    });

    test('returning to 0 then a new 0 -> 1 re-fires', () async {
      await emit(0);
      await emit(1);
      expect(repo.reportCount, 1);
      await emit(0);
      await emit(1);
      expect(repo.reportCount, 2);
    });
  });
}
