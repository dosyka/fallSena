import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';

import '../services/auth_service.dart';
import '../services/device_repository.dart';
import '../services/link_service.dart';

/// Caretaker-side live monitoring UI (backlog #P3-C2b). Shows the cloud-relayed
/// status of every device owned by the fallers this caretaker has an *accepted*
/// link with: battery %, online/offline and a relative "last seen".
///
/// Reads only what firestore.rules permits: for each accepted [LinkModel] it
/// queries `devices where ownerId == link.fallerId` (a query the `devices` read
/// rule allows a linked-accepted caretaker to run). Status is never read over
/// BLE — the faller app relays it (#P3-C2a).
class MonitoredScreen extends StatefulWidget {
  const MonitoredScreen({
    super.key,
    required this.user,
    this.linkService,
    this.deviceRepository,
    this.firestore,
  });

  final User user;
  final LinkService? linkService;
  final DeviceRepository? deviceRepository;
  final FirebaseFirestore? firestore;

  @override
  State<MonitoredScreen> createState() => _MonitoredScreenState();
}

class _MonitoredScreenState extends State<MonitoredScreen> {
  late final LinkService _linkService;
  late final DeviceRepository _deviceRepository;

  @override
  void initState() {
    super.initState();
    _linkService = widget.linkService ??
        LinkService(firestore: widget.firestore);
    _deviceRepository = widget.deviceRepository ??
        DeviceRepository(firestore: widget.firestore);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Monitored')),
      body: StreamBuilder<List<LinkModel>>(
        stream: _linkService.acceptedLinks(UserRole.caretaker),
        builder: (context, snapshot) {
          if (snapshot.hasError) {
            return const Center(child: Text('Could not load.'));
          }
          if (!snapshot.hasData) {
            return const Center(child: CircularProgressIndicator());
          }
          final links = snapshot.data!;
          if (links.isEmpty) {
            return const Center(
              child: Padding(
                padding: EdgeInsets.all(24),
                child: Text(
                  'No paired people yet — pair from Connections.',
                  textAlign: TextAlign.center,
                ),
              ),
            );
          }
          return ListView(
            padding: const EdgeInsets.all(16),
            children: [
              for (final link in links)
                _FallerSection(
                  link: link,
                  deviceRepository: _deviceRepository,
                ),
            ],
          );
        },
      ),
    );
  }
}

/// One accepted faller: a header (their UID) and the live status of each device
/// they own, or a "No device yet." placeholder.
class _FallerSection extends StatelessWidget {
  const _FallerSection({
    required this.link,
    required this.deviceRepository,
  });

  final LinkModel link;
  final DeviceRepository deviceRepository;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(top: 8, bottom: 4),
          child: Text(
            link.fallerId,
            style: Theme.of(context)
                .textTheme
                .titleSmall
                ?.copyWith(fontFamily: 'monospace'),
          ),
        ),
        StreamBuilder<List<DeviceStatus>>(
          stream: deviceRepository.watchDevicesForOwner(link.fallerId),
          builder: (context, snapshot) {
            if (snapshot.hasError) {
              return const Padding(
                padding: EdgeInsets.symmetric(vertical: 8),
                child: Text('Could not load.'),
              );
            }
            if (!snapshot.hasData) {
              return const Padding(
                padding: EdgeInsets.symmetric(vertical: 8),
                child: LinearProgressIndicator(),
              );
            }
            final devices = snapshot.data!;
            if (devices.isEmpty) {
              return const ListTile(title: Text('No device yet.'));
            }
            return Column(
              children: [
                for (final device in devices) _DeviceTile(device: device),
              ],
            );
          },
        ),
        const Divider(),
      ],
    );
  }
}

class _DeviceTile extends StatelessWidget {
  const _DeviceTile({required this.device});

  final DeviceStatus device;

  @override
  Widget build(BuildContext context) {
    final online = device.isCurrentlyOnline;
    final battery =
        device.batteryPct != null ? '${device.batteryPct}%' : '—';
    return ListTile(
      leading: Icon(
        Icons.circle,
        size: 14,
        color: online ? Colors.green : Colors.grey,
      ),
      title: Text(device.name),
      subtitle: Text(
        '$battery · ${online ? 'Online' : 'Offline'} · '
        'last seen ${_relativeLastSeen(device.lastSeen)}',
      ),
    );
  }

  /// Human-readable age of [lastSeen], e.g. "2 min ago", relative to now.
  static String _relativeLastSeen(Timestamp? lastSeen) {
    if (lastSeen == null) return 'never';
    final age = DateTime.now().difference(lastSeen.toDate());
    if (age.isNegative || age.inSeconds < 60) return 'just now';
    if (age.inMinutes < 60) {
      final m = age.inMinutes;
      return '$m min ago';
    }
    if (age.inHours < 24) {
      final h = age.inHours;
      return '$h hr ago';
    }
    final d = age.inDays;
    return '$d day${d == 1 ? '' : 's'} ago';
  }
}
