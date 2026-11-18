# Stage 0b: scaffolding and CI

## 1. Goal
Create the skeleton of every component so each one builds, tests and lints in CI from day one: `proto/`, `firmware/`, `core/`, `app/` and `hardware/`. Write the threat model before any security-relevant code exists.

**Exit criteria (from PLAN.md):** CI is green across firmware build, core tests on 3 OSes, `flutter analyze`/`test`, and KiCad ERC/DRC.

## 2. Depends on / unblocks
- **Depends on:** 0a (toolchains, `versions.env`, doctor).
- **Unblocks:** 1, 2, 4 and 6a, which all add code into these skeletons.

## 3. Deliverables
```
proto/
  micnote.proto            # package micnote.v1, initial messages (see 01 for full list)
  micnote.options          # nanopb max sizes / callbacks
  gen/                     # committed nanopb output: micnote.pb.h / micnote.pb.c
  CMakeLists.txt           # micnote_proto INTERFACE/STATIC lib used by core
  regen.py                 # regenerate gen/ with pinned nanopb generator
  test_regen.py            # unittest suite for regen.py, 100% line + branch coverage
firmware/
  west.yml                 # self + sdk-nrf @ NCS_VERSION, path-prefix: external, name-allowlist
  app/
    CMakeLists.txt, prj.conf
    release.conf           # overlay for shipping builds: no UART, no asserts
    src/main.c             # logs "Micnote <version>" and blinks led0
    sysbuild.conf          # MCUboot enabled, Thingy:53 net-core bootloader off
    VERSION                # Zephyr app version file (read from the app dir)
  tests/
    smoke/                 # ztest on native_sim: the kernel clock runs
    proto/                 # proto/gen round-trips under Zephyr's nanopb
core/
  CMakeLists.txt, CMakePresets.json (at repo root, see below)
  cmake/deps.cmake, cmake/warnings.cmake, cmake/sanitizers.cmake
  include/micnote.h        # mn_version() only, for now
  src/version.cpp
  cli/main.cpp             # `micnote-cli --version`
  tests/CMakeLists.txt, tests/version_test.cpp
  fuzz/CMakeLists.txt      # empty harness target, populated in stage 1
app/
  (flutter create --org dev.micnote --project-name micnote --platforms=linux,macos,windows,ios,android app)
  lib/main.dart            # shows core version via FFI
  test/widget_test.dart    # runs against the real libmicnote
  packages/micnote_core/   # everything native, as a Dart package
    hook/build.dart        # build hook: CMake-builds core/ for each target
    tool/ffigen.dart       # regenerates the bindings
    lib/src/bindings.dart  # ffigen output (committed)
    lib/micnote_core.dart  # Dart API over the bindings
    test/
hardware/
  puck/
    puck.kicad_pro, puck.kicad_sch   # root schematic + 8 empty sheets (4.5)
    puck.kicad_pcb, puck.kicad_dru   # 4-layer 1.0 mm board, placeholder outline; JLCPCB rules
    sym-lib-table, fp-lib-table      # load lib/ as the "micnote" library
  lib/                     # micnote.kicad_sym, micnote.pretty/, 3dmodels/
docs/
  threat-model.md
  protocol.md              # stub, filled in stage 1
  action-schema.md         # stub, filled in stage 3
.clang-format, .clang-tidy, .editorconfig
CMakePresets.json
.github/workflows/{firmware,core,app,hardware,proto}.yml
```

## 4. Design detail

### 4.1 Proto and nanopb
- The nanopb generator is pinned in `versions.env` (`NANOPB_VERSION`). `regen.py` (standard-library Python, one script for every OS) creates a throwaway venv in `build/.nanopb-venv`, installs `nanopb[grpcio-tools]==$NANOPB_VERSION` with `grpcio-tools==$GRPCIO_TOOLS_VERSION` (its bundled `protoc` means no system `protoc` is needed; NixOS, where that wheel can't load, falls back to the flake's `protoc` with identical output), runs `nanopb_generator -I . -D <scratch dir> micnote.proto` from `proto/`, swaps the result into `gen/` only on success, and commits the output as nanopb writes it: the generator alone is deterministic, and running `clang-format` over it would make CI's diff depend on the clang-format version. `proto/gen/.clang-format` disables formatting there, and `.gitattributes` keeps it LF on Windows checkouts.
- **Pinned versions:** the generator is nanopb 0.4.9.1 from PyPI. The host runtime is fetched at the same commit as Zephyr's nanopb module in NCS v3.4.1 (`5499fd4`, nanopb 1.0.0-dev), so puck and host run identical runtime code; both sides use `PB_PROTO_HEADER_VERSION` 40, which the generated header checks.
- **The same generated `.pb.c/.pb.h` is used by both sides:**
  - Firmware: compiles `proto/gen/micnote.pb.c` directly and uses Zephyr's nanopb module for the runtime (`CONFIG_NANOPB=y`). In 0b only `firmware/tests/proto` does this; the app adds it in stage 1, when it first sends a message.
  - Core: the nanopb runtime comes through FetchContent (`pb_encode.c`, `pb_decode.c`, `pb_common.c`), and `proto/CMakeLists.txt` builds `micnote_proto`.
- **CI (`proto.yml`):** `regen.py --check` (regenerate, then `git diff --exit-code proto/gen`) on Ubuntu and Windows, plus `regen.py`'s unit tests (standard-library `unittest`, one test class per step) under coverage.py with a 100% line and branch gate (pinned as `COVERAGE_VERSION`), and `ruff check` and `ruff format --check` (pinned as `RUFF_VERSION`). Both are configured in the root `pyproject.toml`, which holds tool config only; nothing is packaged.
- **`micnote.options`:** fixed max sizes for every repeated or bytes field (e.g. `Chunk.payload max_size:4096`), so the firmware never needs heap callbacks.

### 4.2 Firmware skeleton
- The **sysbuild** build with MCUboot is enabled from the start (`SB_CONFIG_BOOTLOADER_MCUBOOT=y` in `sysbuild.conf`), so the flash partition layout is fixed early. Use the dev-only default signing key for now; stage 7 replaces it with a real key kept outside the repo.
  - The Thingy:53's board defaults also enable a network-core bootloader (b0n), which needs an `s0_partition`. NCS 3.4 deprecated Partition Manager, which used to generate it, and the Thingy's net-core devicetree doesn't define one, so `sysbuild.conf` sets `SB_CONFIG_SECURE_BOOT_NETCORE=n`. Revisit when the partition layout is designed in devicetree (stage 1 storage, stage 6b OTA).
  - The Thingy's MCUboot defaults to `OVERWRITE_ONLY` and USB serial recovery; the DK's MCUboot has neither.
- **`prj.conf`** (the everyday debug build): `CONFIG_LOG=y`, logs to RTT (`USE_SEGGER_RTT`, `LOG_BACKEND_RTT`) and to UART (`LOG_BACKEND_UART`), `CONFIG_GPIO=y`, `CONFIG_ASSERT=y`, and the NCS boot banner named "Micnote". Each option is listed because the app needs it, even where a board default already sets it.
  - **`release.conf`**, layered on top with `-DEXTRA_CONF_FILE=release.conf`, turns off asserts and the whole UART chain (`SERIAL`, `CONSOLE`, `LOG_BACKEND_UART`): an enabled UART keeps a high-frequency clock running. RTT logging stays. Flash drops from about 51 KB to 33 KB on the DK.
  - `CONFIG_LED` and `CONFIG_SHELL` from the original baseline are left out until code uses them; the RTT shell needs its own backend setup and shares RTT channel 0 with logging.
- **Board overlays:** none yet. Both boards' devicetrees already provide `led0`, so the overlays would be empty; they arrive with the first hardware the app adds (stage 1).
- **Version:** `firmware/app/VERSION` (Zephyr reads it from the app directory) generates `<zephyr/app_version.h>` and feeds imgtool's `--version` for the MCUboot image header.
- **`west.yml`** imports sdk-nrf with a `name-allowlist` of the 13 modules these builds use (from 54), cutting `external/` to 1.6 GB on a shallow fetch. A newly needed module shows up as a Kconfig or CMake error naming it; add it to the list.
- **Build and test commands:** see [`docs/dev-setup.md`](../dev-setup.md#firmware).
- **CI (`firmware.yml`):**
  - Runs in Nordic's `ghcr.io/nrfconnect/sdk-nrf-toolchain` image, the CI route Nordic's docs describe, pinned by tag (`NCS_VERSION`) and digest (`NCS_TOOLCHAIN_IMAGE_DIGEST`) in `versions.env`.
  - Caches `external/` keyed on `firmware/west.yml`; `west update --narrow -o=--depth=1` fetches the rest.
  - Builds the DK, the Thingy:53 and the DK with `release.conf`, and uploads `zephyr.signed.hex`, MCUboot's `zephyr.hex` and `dfu_application.zip` as artifacts.
  - Runs `west twister -T firmware/tests -p native_sim`, after installing the 32-bit host gcc and `make`, which the image lacks.
  - On NixOS, native_sim needs the dev shell's `hardeningDisable` for fortify (see `docs/dev-setup.md`, Troubleshooting).

### 4.3 Core skeleton
- **`CMakePresets.json`** at the repo root:
  | Preset | Generator | Notes |
  |---|---|---|
  | `dev` | Ninja Multi-Config | warnings as errors, `CMAKE_EXPORT_COMPILE_COMMANDS=ON` |
  | `asan` | Ninja | `-fsanitize=address,undefined`; compiler from `CC`/`CXX` (CI uses clang 20) |
  | `release` | Ninja | CMake's Release flags, LTO |
  | `fuzz` | Ninja | clang (`CC`/`CXX`), `-fsanitize=fuzzer-no-link,address,undefined`; fuzz executables link `-fsanitize=fuzzer` |
  | `android-arm64` | Ninja | `$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake`, `ANDROID_PLATFORM=26` |
  | `ios` | Xcode | `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_DEPLOYMENT_TARGET=16.0` |
  | `windows-msvc` | Ninja | `/W4 /permissive- /MT` |
- **`cmake/deps.cmake`:** a helper `micnote_fetch(name GIT_REPOSITORY … GIT_TAG <full sha> …)`. It requires full commit hashes, never branches or tags, so dependency versions can't move silently. In 0b it only fetches GoogleTest and nanopb; the rest arrive in stages 1–3.
- **Warnings:** `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (MSVC equivalents).
- **Tidy:** `.clang-tidy` enables `bugprone-*,cert-*,cppcoreguidelines-*,modernize-*,performance-*,readability-*` minus the noisy checks (documented inline).
- **Library type:** `libmicnote` builds as a **shared** library on every platform (`MICNOTE_SHARED`, default on). The app's build hook bundles it as a dynamic library, wrapped in a framework on iOS and macOS, so the static-XCFramework plan for iOS is gone. The hook also sets `MICNOTE_VERSIONED_SONAME=OFF`: Android only packages plain `lib<name>.so` files.
- **CI (`core.yml`):** matrix of ubuntu (gcc-14 and clang), macos (Apple clang) and windows (MSVC). Runs configure → build → `ctest --output-on-failure`, and also an `asan` job on ubuntu (pushes to `main` and manual runs only, to keep PR runs fast) and a `clang-tidy` job.
- **Coverage gate:** a `coverage` preset (gcc `--coverage`) and a `coverage` CI job running gcovr (pinned as `GCOVR_VERSION`). `gcovr.cfg` requires 100% line, function and branch coverage of `core/src`, `core/cli` and `core/include`. It leaves out branches that exist only for exception unwinding and branches gcc proves unreachable. Code no test can reach is fenced with `GCOVR_EXCL_START/STOP` and a comment saying why.

### 4.4 App skeleton
- **Build hooks, not FFI plugin glue.** In Flutter 3.47 the `plugin_ffi` template (per-platform CMake, `externalNativeBuild`, podspec) is deprecated in favour of `package_ffi`, which uses build hooks ("native assets", on by default). Flutter runs a package's `hook/build.dart` once per target OS and architecture and bundles the libraries it reports, on all six platforms. That replaces the planned `app/native/` glue and its podspec/jniLibs/DLL-placement work.
- **Layout:** `app/` is the Flutter app. Everything native lives in `app/packages/micnote_core`, a pure-Dart package the app depends on by path: the hook, the ffigen bindings and a small Dart API (`coreVersion()`). The bindings can be tested with `dart test`, which also runs the hook.
- **The hook builds `core/` with CMake** (our own mapping, not a third-party builder): it maps each target to CMake arguments and builds the `micnote` target into the hook's output directory.
  - Linux: the C compiler Flutter passes (clang on PATH) and its `clang++`.
  - Android: the NDK toolchain file found from the NDK clang Flutter passes, `ANDROID_ABI`, `ANDROID_PLATFORM` = the app's `minSdk`, static libc++.
  - macOS/iOS: `CMAKE_OSX_ARCHITECTURES`, the deployment target, and `CMAKE_SYSTEM_NAME=iOS` + `CMAKE_OSX_SYSROOT` for iOS.
  - Windows: runs inside the Visual Studio developer prompt Flutter passes (cl.exe, plus VS's cmake and ninja), static CRT.
  - Hooks only see an allowlist of environment variables (PATH, HOME, ANDROID_*, NIX_*, ...): `CC`/`CXX` don't reach them, so compilers come from the hook input. cmake and ninja must be on PATH (except on Windows).
  - It doesn't read `CMakePresets.json`: presets are fixed per platform, while the hook runs per architecture into a directory it's given.
- **Bindings:** `tool/ffigen.dart` (ffigen's Dart API; its YAML config is deprecated) reads `core/include/micnote.h` and writes `@Native` functions to `lib/src/bindings.dart`, bound to the hook's code asset. Committed; CI regenerates and diffs.
- **Android pins:** `app/android/app/build.gradle.kts` reads `compileSdk`, `ndkVersion` and `buildToolsVersion` from `tools/versions.env`, so Gradle never picks its own NDK.
- **First screen:** shows `libmicnote <version>`, from `mn_version()`. The widget test checks it against the real library.
- **Lints:** `flutter_lints` (app) and `lints` (micnote_core), both run with `--fatal-infos` in CI.
- **CI (`app.yml`):**
  - `check`: micnote_core format/analyze/test and the bindings diff; the app's format/analyze/widget tests.
  - `build`: Linux, Android APK, Windows, macOS and iOS (`--no-codesign`) on every relevant change; the repo is public, so macOS minutes are free. The Linux bundle is started under Xvfb to check it loads `libmicnote.so`.
  - Linux jobs put clang 20 first on PATH: Flutter hands the hook that clang, and Ubuntu 24.04's clang 18 can't use libstdc++ 14's `<expected>`.
- **NixOS:** nixpkgs' Flutter rewrites the engine library's RUNPATH (upstream: `$ORIGIN`), so it can't find the bundled `libmicnote.so`. The NixHub shell adds `$ORIGIN` back; see `docs/dev-setup.md`, Troubleshooting.

### 4.5 Hardware skeleton
- **KiCad 10**, not 9: it's the current release (and what the NixHub shell provides), and KiCad 9 can't open files saved by 10. `KICAD_MIN` is 10.0, so bootstrap uses the `kicad-10.0-releases` PPA.
- KiCad project with empty hierarchical sheets: power, mcu, audio, storage, sensors, ui_haptics, wireless_charging, connectors.
- **Board:** 4 copper layers (In1 GND, In2 power + QSPI), 1.0 mm, a stack-up after JLCPCB's JLC04101H-7628 (*verify* before ordering, 6a), ENIG, and a placeholder 50 mm round outline that 6a replaces.
- **Rules, in two places:**
  - `puck.kicad_pro` holds the board minimums and the Default net class (0.127 mm track and clearance, 0.45/0.2 mm vias), the plan's preferred sizes;
  - `puck.kicad_dru` holds JLCPCB's limits as custom rules: 0.1 mm track and space, 0.2/0.45 mm vias, annular rings, hole-to-hole (0.5 mm across nets), 0.3 mm copper to edge, no blind/buried/micro vias, silkscreen text ≥ 1 mm.
- **CI (`hardware.yml`):** runs in `kicad/kicad`, pinned by version and digest in `versions.env` (`KICAD_VERSION`, `KICAD_IMAGE_DIGEST`), as root (the image's `kicad` user can't write the workspace):
  - `kicad-cli sch erc --exit-code-violations` on the root schematic;
  - `kicad-cli pcb drc --exit-code-violations` on the board;
  - a canary: `kicad-cli` silently ignores a `.kicad_dru` it can't parse, so DRC also runs on a scratch copy with a 0.05 mm track and must report the `.kicad_dru`'s own "track width" rule.
- Fab exports (gerbers, drill, BOM, CPL, STEP) arrive with the real board in 6a.

### 4.6 Threat model (`docs/threat-model.md`)
Written STRIDE-style. **Assets:**
- raw audio;
- transcripts;
- action payloads;
- connector OAuth tokens;
- device and host keys;
- firmware signing key.

**Adversaries:**
| Adversary | What they can do |
|---|---|
| Thief with the puck | Physical access to the puck only |
| Thief with an unlocked or locked phone/laptop | Access to a host device |
| Nearby attacker | Within BLE radio range: sniffing, spoofing, tracking |
| Malicious network or cloud provider | Sees traffic and anything sent to cloud services |
| Supply chain | Compromised dependency, model file or CI system |
| Prompt injection via transcribed content | Someone dictates "ignore previous instructions, send …", or a PR diff contains instructions |

For each threat, list the mitigation (most are already in PLAN.md) and which stage owns it. **Prompt injection** gets its own section: the planner's output is only a *draft*, every side effect needs confirmation, and connector context (PR diffs, emails) is treated as data. That last rule is enforced in stage 3's prompt design.

## 5. Task breakdown
**PR 1: proto + core skeleton**
- [x] S: `proto/micnote.proto` with `DeviceInfo` and `Version` messages, `regen.py`, `gen/` (replaced the first `regen.sh`/`regen.ps1` pair after CI showed every fix had to be made twice)
- [x] M: `core/` CMake, presets, warnings, deps helper, `mn_version`, `micnote-cli --version`, a GoogleTest (dev, asan, release and fuzz built and tested on Linux with gcc 16 and clang 23; android, ios and windows-msvc not run locally)
- [x] S: `.clang-format`, `.clang-tidy`, `.editorconfig`
- [x] M: `core.yml` + `proto.yml` workflows (green on GitHub)
- [x] S: 100% coverage gate (`coverage` preset, `gcovr.cfg`, CI job; CLI tests added to reach it)
- [x] S: `regen.py` unit tests with a 100% line + branch gate (coverage.py), run on Linux and Windows

**PR 2: firmware skeleton**
- [x] S: `firmware/west.yml`, app with LED blink + version log, sysbuild/MCUboot (DK, Thingy:53 and DK-release built locally with NCS v3.4.1; no hardware flashed yet)
- [x] S: board overlays for the Thingy:53 and DK (none needed yet, see 4.2)
- [x] M: `firmware.yml` (container, west cache, three builds, twister; green on GitHub)
- [x] S: `firmware/tests/` smoke and proto suites (4 tests on native_sim)

**PR 3: app skeleton**
- [x] M: `flutter create`, build hook (replaces the `native/` CMake glue, see 4.4), ffigen, version screen
- [x] L: get FFI loading on all five app platforms (all five build and bundle libmicnote in CI. Loading checked at runtime only on Linux: tests, `flutter run`, and the release bundle under Xvfb in CI. Android, Windows, macOS and iOS not yet run on a device)
- [x] M: `app.yml` (green on GitHub; the Windows hook needed a batch file, see `hook/build.dart`)

**PR 4: hardware + docs**
- [x] S: KiCad project skeleton, libs, `.kicad_dru`, `hardware.yml` (KiCad 10, see 4.5; ERC, DRC and the rules canary pass locally and in the pinned image, not yet run on GitHub)
- [x] M: `docs/threat-model.md` (six open issues for stages 1–7, §6: host authentication on the link, SMP scope, firmware rollback, advertising identity, decrypted audio on disk, recipient provenance)
- [x] S: `docs/protocol.md` and `docs/action-schema.md` stubs

## 6. Testing and verification
- **Locally:** `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`.
- **Locally:** `west build …` for both boards. Flash the Thingy:53 (`west flash`, via the DK's J-Link) and see the version over RTT (`JLinkRTTViewer` or `west rtt`, *verify*).
- **Locally:** `flutter run -d linux` shows the core version.
- **CI:** all six workflows green on the PR.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| The build hook's per-target CMake mapping misses a platform detail (iOS sysroot, Windows prompt, NDK path) | CI builds all five app platforms; the mapping is one file (`hook/build.dart`), and `native_toolchain_cmake` is the fallback if maintaining it costs too much |
| Flutter build hooks are newer than the plugin glue they replace | They're the default in Flutter 3.47 and the old template is deprecated; the hook protocol is versioned (`hooks`, `code_assets` packages) |
| NCS container size slows CI | Cache `external/` and the toolchain; run the firmware job only when `firmware/**` or `proto/**` changes |
| Generated-code drift | Diff checks in CI for both nanopb and ffigen output |
| NCS 3.4 dropped Partition Manager; partitions must now be written in devicetree, and some board defaults (Thingy:53 b0n) assume the old generator | Net-core bootloader off for now; design the full layout (MCUboot, slots, recording storage) in DTS before stage 1 storage work. NCS ships `scripts/pm_to_dts.py` and `migration_partitions.rst` |
| The Thingy:53's MCUboot USB recovery uses Zephyr's deprecated USB device stack | Fine for development; decide the update path for the puck (USB recovery vs BLE DFU) in stages 5b/6b, on the new stack |
| `kicad-cli` ignores a broken `.kicad_dru` and DRC passes on the defaults | CI's canary step fails unless the custom "track width" rule fires on a deliberately bad copy |
| A module missing from the `west.yml` allowlist breaks a future build | The error names the module; add it. CI builds every configuration on each change |

## 8. Open questions
- ~~Use Flutter's newer "native assets" / build hooks instead of manual CMake glue?~~ Yes: on by default in Flutter 3.47, and the old `plugin_ffi` template is deprecated (4.4).
- ~~macOS runners are expensive: run macOS/iOS builds only on `main`?~~ The repo is public, so macOS minutes are free; every platform builds on each relevant change.

## 9. Out of scope
- Any real feature code.
- Signing keys (stages 5b and 7).
