# Micnote app

The Flutter app for Linux, macOS, Windows, iOS and Android. Native code lives in
[`packages/micnote_core`](packages/micnote_core): its build hook compiles
`core/` with CMake for each target, and Dart reaches it through `dart:ffi`.

Commands and setup: [`docs/dev-setup.md`](../docs/dev-setup.md#flutter).
Design: [`docs/stages/00b-scaffolding-ci.md`](../docs/stages/00b-scaffolding-ci.md) section 4.4.
