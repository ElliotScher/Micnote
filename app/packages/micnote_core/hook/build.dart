// Builds libmicnote (core/) with CMake for the target Flutter or `dart test`
// asks for, and bundles it as the code asset behind lib/src/bindings.dart.
//
// The hook maps the target (OS, architecture, Android API, Apple SDK and
// deployment version) to CMake arguments itself, rather than reading
// CMakePresets.json: presets are fixed per platform, while the hook runs once
// per target architecture and must build into the directory hooks give it.
//
// Needs cmake on PATH, and ninja everywhere but Windows, where Visual Studio's
// developer prompt supplies both. Hooks only see an allowlist of environment
// variables (PATH, HOME, ANDROID_*, NIX_*, ...), so compilers come from the
// hook input, not CC/CXX.
import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;
    final code = input.config.code;
    if (code.linkModePreference == LinkModePreference.static) {
      throw UnsupportedError('libmicnote is only built as a dynamic library');
    }

    final core = input.packageRoot.resolve('../../../core/');
    final buildDir = input.outputDirectory.resolve('cmake/');
    final configure = [
      '-S',
      core.toFilePath(),
      '-B',
      buildDir.toFilePath(),
      '-G',
      'Ninja',
      '-DCMAKE_BUILD_TYPE=Release',
      '-DMICNOTE_SHARED=ON',
      '-DMICNOTE_VERSIONED_SONAME=OFF',
      ..._targetArgs(code),
    ];
    final compile = ['--build', buildDir.toFilePath(), '--target', 'micnote'];

    if (code.targetOS == OS.windows) {
      // cl.exe, link.exe and Visual Studio's bundled cmake and ninja are only
      // on PATH inside the developer prompt for the target architecture.
      final prompt = code.cCompiler?.windows.developerCommandPrompt;
      if (prompt == null) {
        throw StateError('No Visual Studio developer prompt in the hook input');
      }
      final cmd = [
        'call',
        _quote(prompt.script.toFilePath()),
        ...prompt.arguments,
        '&&',
        'cmake',
        ...configure.map(_quote),
        '&&',
        'cmake',
        ...compile.map(_quote),
      ].join(' ');
      await _run('cmd', ['/d', '/s', '/c', '"$cmd"']);
    } else {
      await _run('cmake', configure);
      await _run('cmake', compile);
    }

    // core/ sets CMAKE_LIBRARY_OUTPUT_DIRECTORY (and the runtime directory,
    // where Windows puts DLLs) to <build>/bin.
    final library = buildDir.resolve(
      'bin/${code.targetOS.dylibFileName('micnote')}',
    );
    if (!File.fromUri(library).existsSync()) {
      throw StateError('CMake built no ${library.toFilePath()}');
    }
    output.assets.code.add(
      CodeAsset(
        package: input.packageName,
        name: 'src/bindings.dart',
        linkMode: DynamicLoadingBundled(),
        file: library,
      ),
    );

    // Rerun the hook when anything the library is built from changes.
    output.dependencies.addAll([
      for (final dir in [core, input.packageRoot.resolve('../../../proto/')])
        for (final f in Directory.fromUri(dir).listSync(recursive: true))
          if (f is File) f.uri,
    ]);
  });
}

List<String> _targetArgs(CodeConfig code) {
  final arch = code.targetArchitecture;
  switch (code.targetOS) {
    case OS.linux:
      if (arch != Architecture.current) {
        throw UnsupportedError('Linux cross-builds ($arch) are not supported');
      }
      return _compilerArgs(code.cCompiler);

    case OS.android:
      // The NDK's clang lives at <ndk>/toolchains/llvm/prebuilt/<host>/bin/.
      final compiler = code.cCompiler?.compiler;
      if (compiler == null) {
        throw StateError('No NDK compiler in the hook input');
      }
      final ndk = compiler.resolve('../../../../../');
      return [
        '-DCMAKE_TOOLCHAIN_FILE=${ndk.resolve('build/cmake/android.toolchain.cmake').toFilePath()}',
        '-DANDROID_ABI=${_androidAbi(arch)}',
        '-DANDROID_PLATFORM=android-${code.android.targetNdkApi}',
        // Static libc++, so the app needn't also bundle libc++_shared.so.
        '-DANDROID_STL=c++_static',
      ];

    case OS.macOS:
      final v = code.macOS.deploymentVersion;
      return [
        '-DCMAKE_OSX_ARCHITECTURES=${_appleArch(arch)}',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=${v.major}.${v.minor}',
      ];

    case OS.iOS:
      final v = code.iOS.deploymentVersion;
      return [
        '-DCMAKE_SYSTEM_NAME=iOS',
        '-DCMAKE_OSX_SYSROOT=${code.iOS.targetSdk.type}',
        '-DCMAKE_OSX_ARCHITECTURES=${_appleArch(arch)}',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=${v.major}.${v.minor}',
        // Compiler checks can't link a runnable iOS executable unsigned.
        '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
      ];

    case OS.windows:
      // The developer prompt picks cl.exe for the target architecture. Static
      // CRT, so the DLL needs no Visual C++ redistributable of its own.
      return [
        '-DCMAKE_C_COMPILER=cl',
        '-DCMAKE_CXX_COMPILER=cl',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
      ];

    default:
      throw UnsupportedError('No libmicnote build for ${code.targetOS}');
  }
}

/// The C compiler Flutter picked (clang on Linux), plus its C++ sibling.
List<String> _compilerArgs(CCompilerConfig? cc) {
  if (cc == null) return [];
  final c = cc.compiler.toFilePath();
  final cxx = c.endsWith('clang')
      ? '$c++'
      : c.replaceFirst(RegExp(r'gcc$'), 'g++');
  return ['-DCMAKE_C_COMPILER=$c', '-DCMAKE_CXX_COMPILER=$cxx'];
}

String _androidAbi(Architecture arch) => switch (arch) {
  Architecture.arm64 => 'arm64-v8a',
  Architecture.arm => 'armeabi-v7a',
  Architecture.x64 => 'x86_64',
  Architecture.ia32 => 'x86',
  _ => throw UnsupportedError('No Android ABI for $arch'),
};

String _appleArch(Architecture arch) => switch (arch) {
  Architecture.arm64 => 'arm64',
  Architecture.x64 => 'x86_64',
  _ => throw UnsupportedError('No Apple architecture for $arch'),
};

String _quote(String s) => s.contains(RegExp(r'[\s"&^]')) ? '"$s"' : s;

Future<void> _run(String exe, List<String> args) async {
  final result = await Process.run(exe, args, runInShell: Platform.isWindows);
  stdout.write(result.stdout);
  stderr.write(result.stderr);
  if (result.exitCode != 0) {
    throw ProcessException(
      exe,
      args,
      'exited with ${result.exitCode}',
      result.exitCode,
    );
  }
}
