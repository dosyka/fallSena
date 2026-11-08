import 'dart:async';

import 'package:flutter/foundation.dart';

import 'device_ble_service.dart';
import 'device_repository.dart';

/// Relays the wearable's LIVE status (battery %, online, lastSeen) into the
/// Firestore `devices/{deviceId}` doc so a linked caretaker can later see it
/// cloud-relayed (backlog #P3-C2a). Kept free of any UI/Flutter-widget
/// dependency, mirroring [FallReporter], so the background service can reuse it.
///
/// `online`/`lastSeen` semantics (see DATA_MODEL.md): a killed app can't write
/// `online = false`, so consumers treat `online == true` as stale once
/// `lastSeen` is older than ~2× the relay interval. To keep that heuristic
/// honest this refreshes `lastSeen` on a periodic [heartbeat] while connected,
/// and best-effort writes `online = false` on a clean disconnect so an offline
/// state shows immediately.
///
/// Every write goes through [DeviceRepository.updateStatus], which writes only
/// status fields (never `ownerId`) so the `devices` update rule passes. Write
/// failures are captured into [lastError] and never tear down the reporter or
/// the BLE link.
class DeviceStatusReporter {
  DeviceStatusReporter({
    required this.bleService,
    required this.repository,
    required this.deviceId,
    this.heartbeat = const Duration(seconds: 30),
  });

  final DeviceBleService bleService;
  final DeviceRepository repository;
  final String deviceId;

  /// How often to refresh `lastSeen` (+ latest battery, `online: true`) while
  /// connected, so the caretaker's staleness heuristic works.
  final Duration heartbeat;

  /// The last error from a failed Firestore status write, if any. Exposed so a
  /// transient failure can be surfaced without the reporter crashing.
  final ValueNotifier<Object?> lastError = ValueNotifier(null);

  /// Whether the reporter is currently relaying status.
  bool get isActive => _started;

  bool _started = false;
  bool _disposed = false;
  Timer? _heartbeatTimer;

  /// Starts relaying status. Idempotent — a second call while already started is
  /// a no-op. Immediately writes `online: true` + current battery, listens for
  /// battery changes, and starts the [heartbeat] timer.
  void start() {
    if (_started) return;
    _started = true;
    bleService.batteryPercent.addListener(_onBatteryChanged);
    _heartbeatTimer = Timer.periodic(heartbeat, (_) => _relay());
    // Immediate first relay so the device shows online right away.
    _relay();
  }

  void _onBatteryChanged() => _relay();

  /// Writes the latest battery + `online: true` and refreshes `lastSeen`.
  Future<void> _relay() async {
    try {
      await repository.updateStatus(
        deviceId: deviceId,
        batteryPct: bleService.batteryPercent.value,
        online: true,
      );
      if (_disposed) return;
      lastError.value = null;
    } catch (e) {
      // Never let a write failure tear down the reporter or the BLE link.
      if (_disposed) return;
      lastError.value = e;
    }
  }

  /// Stops relaying: cancels the battery listener + heartbeat timer and
  /// best-effort writes `online: false` (with `lastSeen`) so a clean disconnect
  /// immediately shows offline. May be restarted with [start].
  void stop() {
    if (!_started) return;
    _started = false;
    bleService.batteryPercent.removeListener(_onBatteryChanged);
    _heartbeatTimer?.cancel();
    _heartbeatTimer = null;
    _markOffline();
  }

  /// Best-effort final `online: false` write. Guarded so a failure can't crash
  /// teardown.
  Future<void> _markOffline() async {
    try {
      await repository.updateStatus(deviceId: deviceId, online: false);
    } catch (_) {
      // Teardown must never throw; a stale online flag is covered by the
      // caretaker's lastSeen staleness heuristic.
    }
  }

  /// Releases all resources. Call from the owning widget's `dispose`.
  void dispose() {
    stop();
    _disposed = true;
    lastError.dispose();
  }
}
