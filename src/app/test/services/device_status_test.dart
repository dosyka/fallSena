import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:fallsena/services/device_repository.dart';
import 'package:flutter_test/flutter_test.dart';

/// Pure-logic tests for [DeviceStatus.isCurrentlyOnline]. [DeviceStatus] and
/// [Timestamp] are plain data classes, so these run without Firebase init.
void main() {
  DeviceStatus statusWith({required bool online, Timestamp? lastSeen}) {
    return DeviceStatus(
      deviceId: 'dev1',
      name: 'FallSena',
      batteryPct: 80,
      charging: false,
      online: online,
      lastSeen: lastSeen,
    );
  }

  group('DeviceStatus.isCurrentlyOnline', () {
    test('online with a fresh lastSeen is online', () {
      final status = statusWith(
        online: true,
        lastSeen: Timestamp.fromDate(DateTime.now()),
      );
      expect(status.isCurrentlyOnline, isTrue);
    });

    test('online but stale lastSeen (older than threshold) is offline', () {
      final stale = DateTime.now().subtract(
        kDeviceStalenessThreshold + const Duration(seconds: 30),
      );
      final status = statusWith(
        online: true,
        lastSeen: Timestamp.fromDate(stale),
      );
      expect(status.isCurrentlyOnline, isFalse);
    });

    test('online with a null lastSeen is offline', () {
      final status = statusWith(online: true, lastSeen: null);
      expect(status.isCurrentlyOnline, isFalse);
    });

    test('online == false is always offline (even with a fresh lastSeen)', () {
      final status = statusWith(
        online: false,
        lastSeen: Timestamp.fromDate(DateTime.now()),
      );
      expect(status.isCurrentlyOnline, isFalse);
    });
  });
}
