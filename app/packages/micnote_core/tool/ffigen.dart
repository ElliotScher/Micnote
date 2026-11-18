// Regenerates lib/src/bindings.dart from core/include/micnote.h, libmicnote's
// C ABI. Run after changing that header:
//   dart run tool/ffigen.dart
// The output is committed; CI regenerates it and fails on a diff.
import 'dart:io';

import 'package:ffigen/ffigen.dart';

void main() {
  final packageRoot = Platform.script.resolve('../');
  final header = packageRoot.resolve('../../../core/include/micnote.h');

  FfiGenerator(
    input: Input(entryPoints: [header], include: (h) => h == header),
    output: Output(
      dart: DartOutput(path: packageRoot.resolve('lib/src/bindings.dart')),
      // @Native functions, resolved through the code asset hook/build.dart
      // adds under this id.
      style: const NativeExternalBindings(
        assetId: 'package:micnote_core/src/bindings.dart',
      ),
      commentType: const CommentType(CommentStyle.any, CommentLength.full),
    ),
    visitors: [
      Visitor(
        func: (node) => node.isIncluded = node.originalName.startsWith('mn_'),
      ),
    ],
  ).generate();
}
