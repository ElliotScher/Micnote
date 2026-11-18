import 'package:flutter_test/flutter_test.dart';
import 'package:micnote/main.dart';

void main() {
  // flutter test runs micnote_core's build hook, so this calls the real
  // libmicnote built for the host.
  testWidgets('home page shows the libmicnote version', (tester) async {
    await tester.pumpWidget(const MicnoteApp());

    expect(
      find.textContaining(RegExp(r'^libmicnote \d+\.\d+\.\d+$')),
      findsOneWidget,
    );
  });
}
