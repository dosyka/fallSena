import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/auth_service.dart';
import '../services/link_service.dart';

/// Caretaker <-> faller pairing UI. Pairing is UID-code based: the user shares
/// their own UID as a "pairing code" and types the other party's code to send
/// a request. See DATA_MODEL.md (`links/{linkId}`) and firestore.rules.
class PairingScreen extends StatefulWidget {
  const PairingScreen({
    super.key,
    required this.user,
    this.linkService,
    this.firestore,
  });

  final User user;
  final LinkService? linkService;
  final FirebaseFirestore? firestore;

  @override
  State<PairingScreen> createState() => _PairingScreenState();
}

class _PairingScreenState extends State<PairingScreen> {
  late final LinkService _linkService;
  late final FirebaseFirestore _firestore;
  final TextEditingController _codeController = TextEditingController();

  UserRole? _role;
  bool _loadingRole = true;
  bool _sending = false;

  @override
  void initState() {
    super.initState();
    _linkService = widget.linkService ?? LinkService();
    _firestore = widget.firestore ?? FirebaseFirestore.instance;
    _loadRole();
  }

  @override
  void dispose() {
    _codeController.dispose();
    super.dispose();
  }

  /// Reads the signed-in user's own `users/{uid}` doc (allowed by the rules)
  /// to learn their role, which decides the faller/caretaker side of links.
  Future<void> _loadRole() async {
    try {
      final doc =
          await _firestore.collection('users').doc(widget.user.uid).get();
      final roleValue = doc.data()?['role'] as String?;
      final role = UserRole.values.firstWhere(
        (r) => r.wireValue == roleValue,
        orElse: () => UserRole.faller,
      );
      if (mounted) {
        setState(() {
          _role = role;
          _loadingRole = false;
        });
      }
    } catch (_) {
      if (mounted) {
        setState(() => _loadingRole = false);
      }
    }
  }

  void _showSnack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _copyCode() async {
    await Clipboard.setData(ClipboardData(text: widget.user.uid));
    _showSnack('Pairing code copied');
  }

  Future<void> _sendRequest() async {
    final role = _role;
    if (role == null) return;
    setState(() => _sending = true);
    try {
      await _linkService.sendPairingRequest(
        otherUid: _codeController.text,
        myRole: role,
      );
      _codeController.clear();
      _showSnack('Request sent');
    } on ArgumentError catch (e) {
      _showSnack(e.message?.toString() ?? 'Invalid pairing code');
    } catch (_) {
      _showSnack('Could not send request');
    } finally {
      if (mounted) setState(() => _sending = false);
    }
  }

  Future<void> _accept(LinkModel link) async {
    try {
      await _linkService.acceptRequest(link.linkId);
      _showSnack('Connection accepted');
    } catch (_) {
      _showSnack('Could not accept request');
    }
  }

  Future<void> _unpair(LinkModel link) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Unpair?'),
        content: const Text(
          'This removes the connection for both parties. Continue?',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Unpair'),
          ),
        ],
      ),
    );
    if (confirmed != true) return;
    try {
      await _linkService.unpair(link.linkId);
      _showSnack('Unpaired');
    } catch (_) {
      _showSnack('Could not unpair');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Connections')),
      body: _loadingRole
          ? const Center(child: CircularProgressIndicator())
          : _role == null
              ? const Center(child: Text('Could not load your profile.'))
              : _buildBody(context, _role!),
    );
  }

  Widget _buildBody(BuildContext context, UserRole role) {
    final me = widget.user.uid;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        _sectionTitle(context, 'Your pairing code'),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
                decoration: BoxDecoration(
                  border: Border.all(color: Theme.of(context).dividerColor),
                  borderRadius: BorderRadius.circular(4),
                ),
                child: SelectableText(
                  me,
                  style: const TextStyle(fontFamily: 'monospace'),
                ),
              ),
            ),
            IconButton(
              tooltip: 'Copy',
              icon: const Icon(Icons.copy),
              onPressed: _copyCode,
            ),
          ],
        ),
        const SizedBox(height: 24),
        _sectionTitle(context, 'Add a connection'),
        const SizedBox(height: 8),
        TextField(
          controller: _codeController,
          decoration: const InputDecoration(
            labelText: "Other party's pairing code",
            border: OutlineInputBorder(),
          ),
        ),
        const SizedBox(height: 8),
        SizedBox(
          width: double.infinity,
          child: FilledButton(
            onPressed: _sending ? null : _sendRequest,
            child: const Text('Send request'),
          ),
        ),
        const SizedBox(height: 24),
        _sectionTitle(context, 'Incoming requests'),
        _StreamSection(
          stream: _linkService.incomingRequests(role),
          emptyText: 'No incoming requests.',
          itemBuilder: (link) => ListTile(
            title: Text(
              link.otherPartyUid(me),
              style: const TextStyle(fontFamily: 'monospace'),
            ),
            trailing: FilledButton(
              onPressed: () => _accept(link),
              child: const Text('Accept'),
            ),
          ),
        ),
        const SizedBox(height: 24),
        _sectionTitle(context, 'Sent requests'),
        _StreamSection(
          stream: _linkService.outgoingRequests(role),
          emptyText: 'No sent requests.',
          itemBuilder: (link) => ListTile(
            title: Text(
              link.otherPartyUid(me),
              style: const TextStyle(fontFamily: 'monospace'),
            ),
            trailing: const Text('Awaiting acceptance'),
          ),
        ),
        const SizedBox(height: 24),
        _sectionTitle(context, 'Connections'),
        _StreamSection(
          stream: _linkService.acceptedLinks(role),
          emptyText: 'No connections yet.',
          itemBuilder: (link) => ListTile(
            title: Text(
              link.otherPartyUid(me),
              style: const TextStyle(fontFamily: 'monospace'),
            ),
            trailing: TextButton(
              onPressed: () => _unpair(link),
              child: const Text('Unpair'),
            ),
          ),
        ),
      ],
    );
  }

  Widget _sectionTitle(BuildContext context, String text) {
    return Text(text, style: Theme.of(context).textTheme.titleMedium);
  }
}

/// Renders a [StreamBuilder] of links as an inline (non-scrolling) list,
/// with loading, error and empty states.
class _StreamSection extends StatelessWidget {
  const _StreamSection({
    required this.stream,
    required this.emptyText,
    required this.itemBuilder,
  });

  final Stream<List<LinkModel>> stream;
  final String emptyText;
  final Widget Function(LinkModel link) itemBuilder;

  @override
  Widget build(BuildContext context) {
    return StreamBuilder<List<LinkModel>>(
      stream: stream,
      builder: (context, snapshot) {
        if (snapshot.hasError) {
          return const Padding(
            padding: EdgeInsets.symmetric(vertical: 12),
            child: Text('Could not load.'),
          );
        }
        if (!snapshot.hasData) {
          return const Padding(
            padding: EdgeInsets.symmetric(vertical: 12),
            child: LinearProgressIndicator(),
          );
        }
        final links = snapshot.data!;
        if (links.isEmpty) {
          return Padding(
            padding: const EdgeInsets.symmetric(vertical: 12),
            child: Text(emptyText),
          );
        }
        return Column(
          children: links.map(itemBuilder).toList(),
        );
      },
    );
  }
}
