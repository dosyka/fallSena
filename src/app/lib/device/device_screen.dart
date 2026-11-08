import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

import '../services/device_ble_service.dart';
import '../services/device_connection_manager.dart';
import '../services/device_repository.dart';

/// Faller-app device screen (backlog #P3-U1): scan for the FallSena wearable,
/// connect over BLE and show its live battery % and fall-event state.
///
/// On connect it also ensures the `devices/{deviceId}` doc and starts a
/// [FallReporter] that relays confirmed falls into Firestore `events`
/// (backlog #P3-U2a).
///
/// The connection + fall reporting are OWNED by an app-scoped
/// [DeviceConnectionManager], NOT by this screen. This screen is a pure view
/// over that manager: it renders the manager's observables and its buttons call
/// `manager.connect/disconnect/startScan`. Critically, [dispose] does NOT
/// disconnect BLE, stop the foreground service or dispose the shared
/// service/reporters — so leaving this screen (navigation/backgrounding) no
/// longer kills fall reporting. Teardown happens ONLY on an explicit user
/// "Disconnect" (`manager.disconnect()`).
///
/// Default Material styling on purpose — the themed restyle is a separate
/// deferred task.
class DeviceScreen extends StatefulWidget {
  const DeviceScreen({super.key, this.service, this.repository});

  /// Injectable for tests; defaults to the process-wide manager's
  /// [DeviceBleService]. When either this or [repository] is provided, the
  /// screen builds a local manager over the injected collaborators instead of
  /// using [DeviceConnectionManager.instance].
  final DeviceBleService? service;

  /// Injectable for tests; defaults to the process-wide manager's
  /// [DeviceRepository]. See [service].
  final DeviceRepository? repository;

  @override
  State<DeviceScreen> createState() => _DeviceScreenState();
}

class _DeviceScreenState extends State<DeviceScreen> {
  late final DeviceConnectionManager _manager;

  @override
  void initState() {
    super.initState();
    // Production: use the app-scoped singleton so the connection + reporting
    // outlive this screen. Tests: build a local manager over injected
    // collaborators so the injected fakes are used.
    _manager = (widget.service != null || widget.repository != null)
        ? DeviceConnectionManager(
            service: widget.service,
            repository: widget.repository,
          )
        : DeviceConnectionManager.instance;
    // Idempotent — prepares the Android foreground service. No-op off Android.
    _manager.init();
    // Surface each successful fall report as a snackbar while this screen is up.
    _manager.lastReportedEventId.addListener(_onReported);
  }

  @override
  void dispose() {
    // IMPORTANT: only detach this screen's own listener. Do NOT disconnect BLE,
    // stop the foreground service or dispose the shared service/reporters — the
    // connection is app-scoped and must survive navigation/backgrounding.
    // Teardown lives solely in DeviceConnectionManager.disconnect().
    _manager.lastReportedEventId.removeListener(_onReported);
    super.dispose();
  }

  Future<void> _startScan() => _manager.startScan();

  Future<void> _connect(BluetoothDevice device) => _manager.connect(device);

  void _onReported() {
    final eventId = _manager.lastReportedEventId.value;
    if (eventId == null || !mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Fall reported (event $eventId)')),
    );
  }

  Future<void> _disconnect() => _manager.disconnect();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Device')),
      body: ValueListenableBuilder<String?>(
        valueListenable: _manager.message,
        builder: (context, message, _) {
          return ValueListenableBuilder<DeviceConnectionState>(
            valueListenable: _manager.connectionState,
            builder: (context, state, _) {
              return ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  if (message != null) ...[
                    _MessageBanner(message: message),
                    const SizedBox(height: 16),
                  ],
                  if (state == DeviceConnectionState.connected)
                    _buildConnected(context)
                  else
                    _buildScanAndConnect(context, state),
                ],
              );
            },
          );
        },
      ),
    );
  }

  Widget _buildScanAndConnect(
    BuildContext context,
    DeviceConnectionState state,
  ) {
    final connecting = state == DeviceConnectionState.connecting;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          connecting ? 'Connecting…' : 'Disconnected',
          style: Theme.of(context).textTheme.titleMedium,
        ),
        const SizedBox(height: 16),
        StreamBuilder<bool>(
          stream: _manager.isScanning,
          initialData: false,
          builder: (context, snapshot) {
            final scanning = snapshot.data ?? false;
            return FilledButton.icon(
              onPressed: connecting
                  ? null
                  : (scanning ? _manager.stopScan : _startScan),
              icon: Icon(scanning ? Icons.stop : Icons.bluetooth_searching),
              label: Text(scanning ? 'Stop scan' : 'Scan'),
            );
          },
        ),
        const SizedBox(height: 24),
        Text(
          'Devices',
          style: Theme.of(context).textTheme.titleMedium,
        ),
        const SizedBox(height: 8),
        StreamBuilder<List<ScanResult>>(
          stream: _manager.scanResults,
          initialData: const [],
          builder: (context, snapshot) {
            final results = snapshot.data ?? const [];
            if (results.isEmpty) {
              return const Padding(
                padding: EdgeInsets.symmetric(vertical: 12),
                child: Text('No FallSena devices found yet.'),
              );
            }
            return Column(
              children: [
                for (final result in results)
                  ListTile(
                    leading: const Icon(Icons.watch),
                    title: Text(
                      result.device.advName.isNotEmpty
                          ? result.device.advName
                          : DeviceBleService.deviceName,
                    ),
                    subtitle: Text(result.device.remoteId.str),
                    trailing: const Icon(Icons.chevron_right),
                    onTap: connecting ? null : () => _connect(result.device),
                  ),
              ],
            );
          },
        ),
      ],
    );
  }

  Widget _buildConnected(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            const Icon(Icons.bluetooth_connected, color: Colors.blue),
            const SizedBox(width: 8),
            Text(
              'Connected',
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ],
        ),
        const SizedBox(height: 24),
        _batteryCard(context),
        const SizedBox(height: 16),
        _fallEventCard(context),
        const SizedBox(height: 16),
        _fallReportingCard(context),
        const SizedBox(height: 24),
        OutlinedButton.icon(
          onPressed: _disconnect,
          icon: const Icon(Icons.bluetooth_disabled),
          label: const Text('Disconnect'),
        ),
      ],
    );
  }

  Widget _batteryCard(BuildContext context) {
    return ValueListenableBuilder<int?>(
      valueListenable: _manager.batteryPercent,
      builder: (context, percent, _) {
        return Card(
          child: ListTile(
            leading: const Icon(Icons.battery_full),
            title: const Text('Battery'),
            trailing: Text(
              percent != null ? '$percent%' : '—',
              style: Theme.of(context).textTheme.titleLarge,
            ),
          ),
        );
      },
    );
  }

  /// Shows that confirmed falls are being relayed to the caretaker, and the id
  /// of the last event reported (backlog #P3-U2a).
  Widget _fallReportingCard(BuildContext context) {
    return ValueListenableBuilder<bool>(
      valueListenable: _manager.reportingActive,
      builder: (context, active, _) {
        if (!active) {
          return const Card(
            child: ListTile(
              leading: Icon(Icons.cloud_off),
              title: Text('Fall reporting'),
              subtitle: Text('Not active'),
            ),
          );
        }
        return ValueListenableBuilder<String?>(
          valueListenable: _manager.lastReportedEventId,
          builder: (context, eventId, _) {
            return Card(
              child: ListTile(
                leading: const Icon(Icons.cloud_done, color: Colors.green),
                title: const Text('Fall reporting active'),
                subtitle: Text(
                  eventId == null
                      ? 'Caretakers will be alerted on a confirmed fall.'
                      : 'Last reported event: $eventId',
                ),
              ),
            );
          },
        );
      },
    );
  }

  Widget _fallEventCard(BuildContext context) {
    return ValueListenableBuilder<int?>(
      valueListenable: _manager.fallEvent,
      builder: (context, value, _) {
        final isFall = value == 1;
        // Value `2` = suspected fall / in the wearable's grace window. Show a
        // distinct amber "checking" state so it isn't mislabeled "No active
        // fall" while the countdown is running.
        final isGrace = value == 2;
        final Color? cardColor = isFall
            ? Colors.red.shade100
            : (isGrace ? Colors.amber.shade100 : null);
        final IconData icon = isFall
            ? Icons.warning_amber_rounded
            : (isGrace ? Icons.hourglass_top : Icons.check_circle);
        final Color iconColor =
            isFall ? Colors.red : (isGrace ? Colors.amber.shade800 : Colors.green);
        final String subtitle = value == null
            ? 'Waiting for data…'
            : isFall
                ? 'FALL CONFIRMED'
                : isGrace
                    ? 'Checking… fall suspected'
                    : 'No active fall';
        final Color? subtitleColor = isFall
            ? Colors.red.shade900
            : (isGrace ? Colors.amber.shade900 : null);
        return Card(
          color: cardColor,
          child: ListTile(
            leading: Icon(icon, color: iconColor),
            title: const Text('Fall event'),
            subtitle: Text(
              subtitle,
              style: TextStyle(
                fontWeight:
                    (isFall || isGrace) ? FontWeight.bold : FontWeight.normal,
                color: subtitleColor,
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Simple inline banner for Bluetooth-off / permission / connect errors.
class _MessageBanner extends StatelessWidget {
  const _MessageBanner({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: Theme.of(context).colorScheme.errorContainer,
        borderRadius: BorderRadius.circular(4),
      ),
      child: Row(
        children: [
          Icon(
            Icons.info_outline,
            color: Theme.of(context).colorScheme.onErrorContainer,
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              message,
              style: TextStyle(
                color: Theme.of(context).colorScheme.onErrorContainer,
              ),
            ),
          ),
        ],
      ),
    );
  }
}
