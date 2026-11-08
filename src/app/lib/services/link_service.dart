import 'package:cloud_firestore/cloud_firestore.dart';
import 'package:firebase_auth/firebase_auth.dart';

import 'auth_service.dart';

/// Immutable view of a `links/{linkId}` document (the caretaker <-> faller
/// pairing edge). See DATA_MODEL.md.
class LinkModel {
  const LinkModel({
    required this.linkId,
    required this.fallerId,
    required this.caretakerId,
    required this.status,
    required this.createdBy,
    this.acceptedAt,
  });

  final String linkId;
  final String fallerId;
  final String caretakerId;
  final String status;
  final String createdBy;
  final Timestamp? acceptedAt;

  factory LinkModel.fromDoc(DocumentSnapshot<Map<String, dynamic>> doc) {
    final data = doc.data() ?? const <String, dynamic>{};
    return LinkModel(
      linkId: doc.id,
      fallerId: (data['fallerId'] ?? '') as String,
      caretakerId: (data['caretakerId'] ?? '') as String,
      status: (data['status'] ?? '') as String,
      createdBy: (data['createdBy'] ?? '') as String,
      acceptedAt: data['acceptedAt'] as Timestamp?,
    );
  }

  /// Whichever of [fallerId]/[caretakerId] is not [myUid].
  String otherPartyUid(String myUid) =>
      fallerId == myUid ? caretakerId : fallerId;
}

/// Manages the `links/{linkId}` pairing collection described in DATA_MODEL.md.
///
/// Pairing is UID-code based: a user shares their own UID, the other party
/// types it in. There is no cross-user lookup (the rules make `users/{uid}`
/// readable only by self).
class LinkService {
  LinkService({
    FirebaseFirestore? firestore,
    FirebaseAuth? auth,
  })  : _firestore = firestore ?? FirebaseFirestore.instance,
        _auth = auth ?? FirebaseAuth.instance;

  final FirebaseFirestore _firestore;
  final FirebaseAuth _auth;

  CollectionReference<Map<String, dynamic>> get _links =>
      _firestore.collection('links');

  /// The signed-in user's UID (their own "pairing code").
  String get myUid => _auth.currentUser?.uid ?? '';

  /// The `links/{linkId}` field that holds *my* UID for [myRole]:
  /// faller -> `fallerId`, caretaker -> `caretakerId`.
  String _ownField(UserRole myRole) =>
      myRole == UserRole.faller ? 'fallerId' : 'caretakerId';

  /// Creates a pending pairing request to [otherUid]. My side is derived from
  /// [myRole]; the doc ID is the deterministic "{fallerId}_{caretakerId}".
  Future<void> sendPairingRequest({
    required String otherUid,
    required UserRole myRole,
  }) async {
    final me = myUid;
    final other = otherUid.trim();
    if (other.isEmpty) {
      throw ArgumentError('Enter a pairing code.');
    }
    if (other == me) {
      throw ArgumentError("That's your own pairing code.");
    }

    final fallerId = myRole == UserRole.faller ? me : other;
    final caretakerId = myRole == UserRole.faller ? other : me;
    final linkId = '${fallerId}_$caretakerId';

    await _links.doc(linkId).set({
      'fallerId': fallerId,
      'caretakerId': caretakerId,
      'status': 'pending',
      'createdBy': me,
      'createdAt': FieldValue.serverTimestamp(),
      'acceptedAt': null,
    });
  }

  /// Accepts a pending request (the OTHER party flips pending -> accepted).
  /// Only `status`/`acceptedAt` are written so the immutable fields stay
  /// untouched, satisfying the `allow update` rule.
  Future<void> acceptRequest(String linkId) {
    return _links.doc(linkId).update({
      'status': 'accepted',
      'acceptedAt': FieldValue.serverTimestamp(),
    });
  }

  /// Unpairs by deleting the link doc (either party may do this).
  Future<void> unpair(String linkId) {
    return _links.doc(linkId).delete();
  }

  /// Pending requests addressed to me that I did NOT create (incoming).
  Stream<List<LinkModel>> incomingRequests(UserRole myRole) {
    final me = myUid;
    return _links
        .where(_ownField(myRole), isEqualTo: me)
        .where('status', isEqualTo: 'pending')
        .snapshots()
        .map((snap) => snap.docs
            .map(LinkModel.fromDoc)
            .where((link) => link.createdBy != me)
            .toList());
  }

  /// Pending requests I created that are awaiting the other party (outgoing).
  Stream<List<LinkModel>> outgoingRequests(UserRole myRole) {
    final me = myUid;
    return _links
        .where(_ownField(myRole), isEqualTo: me)
        .where('status', isEqualTo: 'pending')
        .snapshots()
        .map((snap) => snap.docs
            .map(LinkModel.fromDoc)
            .where((link) => link.createdBy == me)
            .toList());
  }

  /// Accepted connections involving me.
  Stream<List<LinkModel>> acceptedLinks(UserRole myRole) {
    final me = myUid;
    return _links
        .where(_ownField(myRole), isEqualTo: me)
        .where('status', isEqualTo: 'accepted')
        .snapshots()
        .map((snap) => snap.docs.map(LinkModel.fromDoc).toList());
  }
}
