import 'package:fallsena/services/link_service.dart';
import 'package:flutter_test/flutter_test.dart';

/// Pure-logic tests for [LinkModel.otherPartyUid]. [LinkModel] is an immutable
/// data class, so these run without Firebase init.
void main() {
  LinkModel link() => const LinkModel(
        linkId: 'faller1_caretaker1',
        fallerId: 'faller1',
        caretakerId: 'caretaker1',
        status: 'accepted',
        createdBy: 'faller1',
      );

  group('LinkModel.otherPartyUid', () {
    test('when I am the faller, returns the caretaker', () {
      expect(link().otherPartyUid('faller1'), 'caretaker1');
    });

    test('when I am the caretaker, returns the faller', () {
      expect(link().otherPartyUid('caretaker1'), 'faller1');
    });
  });
}
