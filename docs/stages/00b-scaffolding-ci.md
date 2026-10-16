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
  regen.sh, regen.ps1      # regenerate gen/ with pinned nanopb generator
firmware/
  west.yml                 # self + sdk-nrf @ NCS_VERSION, path-prefix: external
  app/
    CMakeLists.txt, prj.conf, Kconfig
    src/main.c             # logs "micnote <version>" and blinks LED
    boards/thingy53_nrf5340_cpuapp.overlay
    boards/nrf5340dk_nrf5340_cpuapp.overlay
    sysbuild.conf          # MCUboot enabled
  VERSION                  # Zephyr app version file
core/
  CMakeLists.txt, CMakePresets.json (at repo root, see below)
  cmake/deps.cmake, cmake/warnings.cmake, cmake/sanitizers.cmake
  include/micnote.h        # mn_version() only, for now
  src/version.cpp
  cli/main.cpp             # `micnote-cli --version`
  tests/CMakeLists.txt, tests/version_test.cpp
  fuzz/CMakeLists.txt      # empty harness target, populated in stage 1
app/
  (flutter create --org dev.micnote --platforms=linux,macos,windows,ios,android micnote)
  lib/main.dart            # shows core version via FFI
  lib/ffi/bindings.dart    # ffigen output (committed)
  ffigen.yaml
  native/                  # CMake glue that builds libmicnote for each Flutter platform
hardware/
  puck/puck.kicad_pro      # empty project with sheet stubs
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
- The nanopb generator is pinned in `versions.env` (`NANOPB_VERSION`). `regen.sh` creates a throwaway venv in `build/.nanopb-venv`, installs `nanopb==$NANOPB_VERSION`, runs `nanopb_generator -I proto -D proto/gen micnote.proto`, then `clang-format`s the output so diffs are stable.
- **The same generated `.pb.c/.pb.h` is used by both sides:**
  - Firmware: `firmware/app/CMakeLists.txt` adds `../../proto/gen` and uses Zephyr's nanopb module for the runtime (`CONFIG_NANOPB=y`).
  - Core: the nanopb runtime comes through FetchContent (`pb_encode.c`, `pb_decode.c`, `pb_common.c`), and `proto/CMakeLists.txt` builds `micnote_proto`.
- **CI (`proto.yml`):** regenerate, then `git diff --exit-code proto/gen`.
- **`micnote.options`:** fixed max sizes for every repeated or bytes field (e.g. `Chunk.payload max_size:4096`), so the firmware never needs heap callbacks.

### 4.2 Firmware skeleton
- The **sysbuild** build with MCUboot is enabled from the start (`SB_CONFIG_BOOTLOADER_MCUBOOT=y`), so the flash partition layout is fixed early. Use the dev-only default signing key for now; stage 7 replaces it with a real key kept outside the repo.
- **`prj.conf` baseline:** `CONFIG_LOG=y`, `CONFIG_USE_SEGGER_RTT=y`, `CONFIG_LOG_BACKEND_RTT=y`, `CONFIG_GPIO=y`, `CONFIG_LED=y`, `CONFIG_SHELL=y` (RTT shell, for bring-up later), `CONFIG_ASSERT=y` in debug.
- **Build commands:**
  ```sh
  west init -l firmware && west update
  west build -b thingy53/nrf5340/cpuapp firmware/app -d build/fw-thingy53 --sysbuild
  west build -b nrf5340dk/nrf5340/cpuapp firmware/app -d build/fw-dk --sysbuild
  ```
- **CI (`firmware.yml`):**
  - Uses Nordic's NCS toolchain container or `nrfutil toolchain-manager` (*verify* which is the maintained option).
  - Caches `external/` keyed on `firmware/west.yml`.
  - Builds both boards and uploads `zephyr.signed.hex` plus `dfu_application.zip` as artifacts.
  - Also runs `west twister -T firmware/tests -p native_sim` (empty test suite for now).

### 4.3 Core skeleton
- **`CMakePresets.json`** at the repo root:
  | Preset | Generator | Notes |
  |---|---|---|
  | `dev` | Ninja Multi-Config | warnings as errors, `CMAKE_EXPORT_COMPILE_COMMANDS=ON` |
  | `asan` | Ninja | `-fsanitize=address,undefined`, clang only |
  | `release` | Ninja | `-O2`, LTO |
  | `fuzz` | Ninja | clang, `-fsanitize=fuzzer,address` |
  | `android-arm64` | Ninja | `$ANDROID_NDK/build/cmake/android.toolchain.cmake`, `ANDROID_PLATFORM=26` |
  | `ios` | Xcode | `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_DEPLOYMENT_TARGET=16.0` |
  | `windows-msvc` | Ninja | `/W4 /permissive- /MT` |
- **`cmake/deps.cmake`:** a helper `micnote_fetch(name GIT_REPOSITORY … GIT_TAG <full sha> …)`. It requires full commit hashes, never branches or tags, so dependency versions can't move silently. In 0b it only fetches GoogleTest and nanopb; the rest arrive in stages 1–3.
- **Warnings:** `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (MSVC equivalents).
- **Tidy:** `.clang-tidy` enables `bugprone-*,cert-*,cppcoreguidelines-*,modernize-*,performance-*,readability-*` minus the noisy checks (documented inline).
- **Library type:** `libmicnote` builds as a **shared** library for Flutter desktop and Android, and as a **static** library in an XCFramework for iOS (Flutter FFI plugin rules; *verify* against current Flutter FFI plugin docs).
- **CI (`core.yml`):** matrix of ubuntu (gcc-14 and clang), macos (Apple clang) and windows (MSVC). Runs configure → build → `ctest --output-on-failure`, and also an `asan` job on ubuntu and a `clang-tidy` job.

### 4.4 App skeleton
- **FFI plugin layout:** `app/` is the Flutter app. `app/native/` has a `CMakeLists.txt` that `add_subdirectory(../../core)` and links `micnote`. Each platform's Flutter runner points at it:
  - Linux/Windows/Android: `CMakeLists.txt` / `externalNativeBuild`.
  - macOS/iOS: a podspec that builds via CMake or uses a prebuilt XCFramework from a script. Of all the platforms, this one is the most fiddly.
- **Bindings:** `ffigen.yaml` points at `core/include/micnote.h` and writes `lib/ffi/bindings.dart`. Generated bindings are committed; CI regenerates them and diffs.
- **First screen:** the minimal UI shows `mn_version()`, proving the FFI works on every platform.
- **Lints:** `flutter_lints` + `very_good_analysis` (or a curated `analysis_options.yaml`).
- **CI (`app.yml`):**
  - `flutter analyze` and `flutter test`.
  - Builds for Linux (ubuntu), macOS + iOS without codesigning (`flutter build ios --no-codesign`, on macos), Windows, and an Android APK on ubuntu.

### 4.5 Hardware skeleton
- KiCad project with empty hierarchical sheets: power, mcu, audio, storage, sensors, ui_haptics, wireless_charging, connectors.
- A `.kicad_dru` custom rule file with JLCPCB 4-layer limits.
- **CI (`hardware.yml`):** runs in a KiCad container (`kicad/kicad:9.0`, *verify*):
  - `kicad-cli sch erc --exit-code-violations hardware/puck/puck.kicad_sch`
  - `kicad-cli pcb drc --exit-code-violations hardware/puck/puck.kicad_pcb`

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
- [ ] S: `proto/micnote.proto` with `DeviceInfo` and `Version` messages, `regen.sh`/`.ps1`, `gen/`
- [ ] M: `core/` CMake, presets, warnings, deps helper, `mn_version`, `micnote-cli --version`, a GoogleTest
- [ ] S: `.clang-format`, `.clang-tidy`, `.editorconfig`
- [ ] M: `core.yml` + `proto.yml` workflows

**PR 2: firmware skeleton**
- [ ] S: `firmware/west.yml`, app with LED blink + version log, sysbuild/MCUboot
- [ ] S: board overlays for the Thingy:53 and DK
- [ ] M: `firmware.yml` (container, west cache, two board builds, twister)

**PR 3: app skeleton**
- [ ] M: `flutter create`, `native/` CMake glue, ffigen, version screen
- [ ] L: get FFI loading on all six platforms (macOS/iOS podspec, Windows DLL placement, Android jniLibs)
- [ ] M: `app.yml`

**PR 4: hardware + docs**
- [ ] S: KiCad project skeleton, libs, `.kicad_dru`, `hardware.yml`
- [ ] M: `docs/threat-model.md`
- [ ] S: `docs/protocol.md` and `docs/action-schema.md` stubs

## 6. Testing and verification
- **Locally:** `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`.
- **Locally:** `west build …` for both boards. Flash the Thingy:53 (`west flash`, via the DK's J-Link) and see the version over RTT (`JLinkRTTViewer` or `west rtt`, *verify*).
- **Locally:** `flutter run -d linux` shows the core version.
- **CI:** all six workflows green on the PR.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Building libmicnote inside Flutter's iOS/macOS build is fragile | Fallback: a `scripts/build-apple-xcframework.sh` that pre-builds the XCFramework, consumed by the podspec |
| NCS container size slows CI | Cache `external/` and the toolchain; run the firmware job only when `firmware/**` or `proto/**` changes |
| Generated-code drift | Diff checks in CI for both nanopb and ffigen output |

## 8. Open questions
- Use Flutter's newer "native assets" / build hooks instead of manual CMake glue, if they are stable at implementation time? (*verify*)
- GitHub Actions minutes: macOS runners are expensive. Run macOS/iOS builds only on `main` and on release tags?

## 9. Out of scope
- Any real feature code.
- Signing keys (stages 5b and 7).
