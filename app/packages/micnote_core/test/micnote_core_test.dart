import 'package:micnote_core/micnote_core.dart';
import 'package:test/test.dart';

void main() {
  // Runs hook/build.dart for the host, then calls into the built library.
  test('coreVersion() returns libmicnote\'s semantic version', () {
    expect(coreVersion(), matches(RegExp(r'^\d+\.\d+\.\d+$')));
  });
}
