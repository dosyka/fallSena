import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// Tri-state connection status for the wearable. flutter_blue_plus 2.x only
/// models `connected`/`disconnected`, so we track the transient `connecting`
/// state ourselves for the UI.
enum DeviceConnectionState { disconnected, connecting, connected }

/// BLE central for the FallSena wearable (backlog #P3-U1).
///
/// Wraps `flutter_blue_plus` and exposes the live fall-event + battery values
/// the wearable publishes over its GATT server. Named [DeviceBleService] (NOT
/// `BleService`) to avoid confusion with the firmware's `ble_service.h`.
///
/// The BLE contract is authoritative and comes from
/// `src/firmware/main/ble_service.h`. The device advertises connectably as
/// "FallSena" and exposes:
///  - Custom FallSena service `fa110000-…`
///    - Fall-Event  char `fa110001-…` (read+notify, 1 byte: 0=none,
///      1=confirmed fall, 2=suspected/in-grace — the wearable's 10 s cancel
///      window).
///    - Device-Status char `fa110002-…` (read+notify, 2 bytes LE) — unused here.
///    - Command char `fa110003-…` (write, 1 byte): writing `0x01` cancels the
///      active on-device grace countdown (same as the on-device button cancel).
///  - Standard Battery Service `0x180F`
///    - Battery Level char `0x2A19` (read+notify, 1 byte 0..100 %).
///
/// Scope is BLE connect + live values only. This service does NOT write
/// Firestore events and does NOT run in the background (task #P3-U2).
class DeviceBleService {
  // --- BLE contract UUIDs (canonical big-endian / 128-bit form) ---

  /// Advertised device name to filter scans on.
  static const String deviceName = 'FallSena';

  /// Custom FallSena GATT service.
  static const String fallSenaServiceUuid =
      'fa110000-5e5a-4fa1-1e00-000000000000';

  /// Fall-Event characteristic: read+notify, 1 byte
  /// (0=none, 1=confirmed fall, 2=suspected/in-grace).
  static const String fallEventCharUuid =
      'fa110001-5e5a-4fa1-1e00-000000000000';

  /// Command characteristic: write, 1 byte. Writing `0x01` cancels the active
  /// on-device grace countdown (same as the on-device button cancel).
  static const String commandCharUuid =
      'fa110003-5e5a-4fa1-1e00-000000000000';

  /// Standard Battery Service.
  static const String batteryServiceUuid =
      '0000180f-0000-1000-8000-00805f9b34fb';

  /// Standard Battery Level characteristic: read+notify, 1 byte (0..100 %).
  static const String batteryLevelCharUuid =
      '00002a19-0000-1000-8000-00805f9b34fb';

  // --- Public reactive state the UI watches ---

  /// Current connection state. Starts disconnected.
  final ValueNotifier<DeviceConnectionState> connectionState =
      ValueNotifier(DeviceConnectionState.disconnected);

  /// Latest Fall-Event value: 0 = no active fall, 1 = confirmed fall,
  /// 2 = suspected/in-grace (the wearable's 10 s cancel window).
  /// `null` until first read/notification arrives.
  final ValueNotifier<int?> fallEvent = ValueNotifier(null);

  /// Latest battery percent (0..100). `null` until first read/notification.
  final ValueNotifier<int?> batteryPercent = ValueNotifier(null);

  /// The device we are currently connected to (or connecting to), if any.
  BluetoothDevice? get device => _device;
  BluetoothDevice? _device;

  // --- Internals ---

  final List<StreamSubscription<dynamic>> _subscriptions = [];
  StreamSubscription<BluetoothConnectionState>? _connectionSub;

  /// The writable Command characteristic, once located during binding. Used by
  /// [writeCancelCommand] to cancel the on-device grace countdown. `null` when
  /// disconnected or if the connected device does not expose it.
  BluetoothCharacteristic? _commandChar;

  /// Whether Bluetooth is on and usable right now.
  ///
  /// We rely on the OS scan prompt + the manifest permissions for the runtime
  /// permission flow (no `permission_handler` dependency); this only reports
  /// whether the adapter itself is available and on.
  Future<bool> ensurePermissions() async {
    if (!await FlutterBluePlus.isSupported) return false;
    if (FlutterBluePlus.adapterStateNow == BluetoothAdapterState.on) {
      return true;
    }
    // Wait briefly for the first real adapter-state event (it can start as
    // `unknown` before the platform reports in).
    try {
      final state = await FlutterBluePlus.adapterState
          .firstWhere((s) => s != BluetoothAdapterState.unknown)
          .timeout(const Duration(seconds: 4));
      return state == BluetoothAdapterState.on;
    } catch (_) {
      return FlutterBluePlus.adapterStateNow == BluetoothAdapterState.on;
    }
  }

  /// Live stream of scan results (already filtered to the FallSena device).
  Stream<List<ScanResult>> get scanResults => FlutterBluePlus.scanResults;

  /// Whether a scan is currently running.
  Stream<bool> get isScanning => FlutterBluePlus.isScanning;

  /// Starts scanning, filtered to the FallSena device (by advertised name and
  /// custom service UUID). The OS BLE permission prompt is triggered here.
  Future<void> startScan() async {
    if (FlutterBluePlus.isScanningNow) return;
    await FlutterBluePlus.startScan(
      withNames: const [deviceName],
      withServices: [Guid(fallSenaServiceUuid)],
      timeout: const Duration(seconds: 15),
    );
  }

  /// Stops an in-progress scan.
  Future<void> stopScan() async {
    if (FlutterBluePlus.isScanningNow) {
      await FlutterBluePlus.stopScan();
    }
  }

  /// Connects to [device], discovers services, locates the Fall-Event + Battery
  /// Level characteristics, enables notifications on both and does an initial
  /// read of each.
  Future<void> connect(BluetoothDevice device) async {
    await stopScan();
    await _teardownConnection();

    _device = device;
    connectionState.value = DeviceConnectionState.connecting;

    // Track the radio connection state so an unexpected disconnect updates the
    // UI and cleans up without crashing.
    _connectionSub = device.connectionState.listen((state) {
      if (state == BluetoothConnectionState.connected) {
        connectionState.value = DeviceConnectionState.connected;
      } else {
        connectionState.value = DeviceConnectionState.disconnected;
        _cancelValueSubscriptions();
      }
    });

    try {
      // License.nonprofit: this is a personal/nonprofit project per the
      // flutter_blue_plus 2.x license gate.
      await device.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 20),
      );
    } catch (e) {
      await _teardownConnection();
      connectionState.value = DeviceConnectionState.disconnected;
      rethrow;
    }

    try {
      final services = await device.discoverServices();
      await _bindCharacteristics(services);
    } catch (e) {
      // Discovery/binding failed — leave the device disconnected cleanly.
      await disconnect();
      rethrow;
    }
  }

  /// Finds the two characteristics we care about, subscribes to notifications
  /// and performs an initial read.
  Future<void> _bindCharacteristics(List<BluetoothService> services) async {
    for (final service in services) {
      for (final characteristic in service.characteristics) {
        final uuid = characteristic.uuid.str128.toLowerCase();
        if (uuid == fallEventCharUuid) {
          await _subscribe(characteristic, _onFallEvent);
        } else if (uuid == batteryLevelCharUuid) {
          await _subscribe(characteristic, _onBatteryLevel);
        } else if (uuid == commandCharUuid) {
          // Write-only command channel — no notifications, just keep a handle.
          _commandChar = characteristic;
        }
      }
    }
  }

  /// Writes the cancel-grace command (`0x01`) to the device's Command
  /// characteristic, cancelling the active on-device grace countdown (same as
  /// the on-device button cancel). Prefers write-without-response when the
  /// characteristic supports it. Throws a [StateError] if the characteristic is
  /// not available (not connected, or the device doesn't expose it).
  Future<void> writeCancelCommand() async {
    final char = _commandChar;
    if (char == null) {
      throw StateError(
          'Command characteristic unavailable — cannot cancel grace countdown.');
    }
    final withoutResponse = char.properties.writeWithoutResponse;
    await char.write([0x01], withoutResponse: withoutResponse);
  }

  /// Enables notifications on [characteristic], wires [onValue] to the value
  /// stream and performs an initial read so the UI has a value immediately.
  Future<void> _subscribe(
    BluetoothCharacteristic characteristic,
    void Function(List<int>) onValue,
  ) async {
    _subscriptions.add(characteristic.onValueReceived.listen(onValue));
    await characteristic.setNotifyValue(true);
    try {
      final value = await characteristic.read();
      onValue(value);
    } catch (_) {
      // An initial read can fail on some stacks; notifications still cover it.
    }
  }

  void _onFallEvent(List<int> value) {
    if (value.isEmpty) return;
    final previous = fallEvent.value;
    final next = value[0];
    // Log EVERY change of the Fall-Event characteristic at the source so an
    // on-device trace shows whether BLE notifications still arrive while the
    // app is backgrounded (#P3-BGDELIVER).
    debugPrint(
        '[BGDELIVER] ${DateTime.now().toIso8601String()} BLE fallEvent notify: $previous -> $next');
    fallEvent.value = next;
  }

  void _onBatteryLevel(List<int> value) {
    if (value.isEmpty) return;
    batteryPercent.value = value[0];
  }

  /// Disconnects from the current device and clears live values.
  Future<void> disconnect() async {
    final device = _device;
    await _teardownConnection();
    if (device != null) {
      try {
        await device.disconnect();
      } catch (_) {
        // Already gone — nothing to do.
      }
    }
    connectionState.value = DeviceConnectionState.disconnected;
  }

  /// Cancels value subscriptions and the connection-state subscription, and
  /// resets the reactive state. Does not touch the radio.
  Future<void> _teardownConnection() async {
    _cancelValueSubscriptions();
    await _connectionSub?.cancel();
    _connectionSub = null;
    _device = null;
    fallEvent.value = null;
    batteryPercent.value = null;
  }

  void _cancelValueSubscriptions() {
    for (final sub in _subscriptions) {
      sub.cancel();
    }
    _subscriptions.clear();
    // The command characteristic is a per-connection handle — drop it whenever
    // the connection's value subscriptions are torn down (teardown or an
    // unexpected radio disconnect) so a stale handle can't be written to.
    _commandChar = null;
  }

  /// Releases all resources. Call from the owning widget's `dispose`.
  void dispose() {
    _cancelValueSubscriptions();
    _connectionSub?.cancel();
    connectionState.dispose();
    fallEvent.dispose();
    batteryPercent.dispose();
  }
}
