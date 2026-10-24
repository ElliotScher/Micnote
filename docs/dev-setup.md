# Development setup

Micnote needs four toolchains: a C++23 compiler for the core, the nRF Connect SDK (NCS) for the firmware, Flutter for the app, and optionally Android and KiCad. Every version is pinned in [`tools/versions.env`](../tools/versions.env); this page doesn't repeat the numbers, so they can't drift.

## Quick start

| OS | Command |
|---|---|
| Ubuntu 24.04+, Debian 13+, Fedora, Arch | `scripts/bootstrap.sh` |
| macOS (needs [Homebrew](https://brew.sh)) | `scripts/bootstrap.sh` |
| Windows 10/11 | `powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1` |

Bootstrap prints what it's going to install and asks before doing anything. Steps that are already satisfied are skipped, so it's safe to re-run after a pin changes. It ends by running doctor.

| bootstrap.sh | bootstrap.ps1 | Effect |
|---|---|---|
| `--yes` | `-Yes` | Don't ask |
| `--dry-run` | `-DryRun` | Print the plan and exit |
| `--no-firmware` | `-NoFirmware` | Skip nrfutil, the NCS toolchain, J-Link and device access |
| `--android` | `-Android` | Add a JDK and the Android SDK, NDK and build tools |
| `--hardware` | `-Hardware` | Add KiCad and FreeCAD |

Bootstrap never edits your shell profile. On Linux and macOS it prints the lines to add at the end (Flutter and nrfutil on `PATH`, and `CXX` if your default compiler is too old). On Windows it asks before changing your user `PATH`.

### Checking a setup: doctor

```sh
scripts/doctor.sh                 # Linux, macOS
scripts/doctor.sh --no-firmware   # core/app work only
```
```powershell
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1 [-NoFirmware]
```

Doctor prints each tool's required and found versions and exits non-zero if something required is missing or too old. It installs nothing. Run it whenever a build fails in a way that looks like a toolchain problem.

## Manual setup

Use this on systems bootstrap doesn't support (NixOS, other distros, older releases), or if you'd rather install things yourself. Doctor tells you when you're done.

### C++ compiler

The core is C++23 and uses `<expected>` and `<print>`. Any of these works:

| Compiler | Notes |
|---|---|
| GCC ≥ 14 | Ubuntu 24.04's default `g++` is 13: install `g++-14` and `export CXX=g++-14` |
| Clang ≥ 19 with libstdc++ 14 | Clang 18 can't use libstdc++'s `std::expected` |
| Clang ≥ 18 with libc++ 18 | Needs `-stdlib=libc++` |
| MSVC (VS 2022 17.10 or later) | Install VS Build Tools with the "Desktop development with C++" workload |
| Apple clang | Needs a recent Xcode; CI tests the version on `macos-latest` (Apple clang 21) |

CMake picks `$CXX`, else `c++`, `g++`, `clang++`; doctor checks the same compiler CMake would use. You also need CMake, Ninja, `pkg-config`, the D-Bus development headers (Linux, for BLE) and, optionally, Vulkan headers and `glslc` for GPU builds of whisper.cpp.

### Firmware

Firmware builds use Nordic's NCS toolchain bundle, which contains the Zephyr SDK, `west`, `dtc` and the matching Python packages:

1. Download `nrfutil` for your OS from [Nordic](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) and put it on `PATH`.
2. `nrfutil install toolchain-manager device`
3. `nrfutil toolchain-manager install --ncs-version <NCS_VERSION>`
4. Build inside the toolchain environment: `nrfutil toolchain-manager launch --ncs-version <NCS_VERSION> --shell` (on Windows, `--shell` isn't supported: use `… launch --ncs-version <NCS_VERSION> -- powershell`).

If `toolchain-manager` has no build for your platform, install the Zephyr SDK version from `versions.env` yourself, set `ZEPHYR_SDK_INSTALL_DIR`, and `pip install west` into a Python ≥ `PYTHON_MIN` virtualenv. Doctor checks the SDK version instead of the toolchain bundle when `ZEPHYR_SDK_INSTALL_DIR` is set.

**Flashing and USB access:**
- **SEGGER J-Link:** SEGGER's licence forbids redistribution, so download it from [segger.com](https://www.segger.com/downloads/jlink/). [pyOCD](https://pyocd.io/) is an open-source alternative for CMSIS-DAP probes.
- **Linux udev rules:** install Nordic's [nrf-udev](https://github.com/NordicSemiconductor/nrf-udev) rules (`71-nrf.rules` in `/etc/udev/rules.d/`), then `sudo udevadm control --reload`.
- **Linux serial group:** `sudo usermod -aG dialout $USER` (`uucp` on Arch), then log out and back in.

### Flutter

Clone the pinned release and put its `bin/` on `PATH`:

```sh
git clone --depth 1 --branch <FLUTTER_VERSION> https://github.com/flutter/flutter.git ~/.local/share/flutter
```

The Linux desktop build also needs clang, GTK 3, libsecret and liblzma development packages. iOS and macOS builds need Xcode and CocoaPods.

### Android (optional)

Install a JDK (`JAVA_VERSION`; Fedora 44 no longer packages it, so use Adoptium's Temurin repo: `dnf install adoptium-temurin-java-repository`, then `dnf install --enablerepo=adoptium-temurin-java-repository temurin-21-jdk`), then Google's command-line tools into `~/Android/Sdk/cmdline-tools/latest` (`~/Library/Android/sdk` on macOS, `%LOCALAPPDATA%\Android\Sdk` on Windows), set `ANDROID_HOME`, and run:

```sh
android --no-metrics --sdk="$ANDROID_HOME" sdk install "platform-tools" "platforms;android-<ANDROID_PLATFORM>" \
  "build-tools;<ANDROID_BUILD_TOOLS>" "ndk;<ANDROID_NDK_VERSION>" "cmake;<ANDROID_CMAKE>"
```

`android` is Google's new Android CLI in `cmdline-tools/latest/bin/`; `sdkmanager` is deprecated and now just calls it. The CLI collects usage data unless you pass `--no-metrics`, which `sdkmanager` can't pass on.

### Hardware (optional)

KiCad ≥ `KICAD_MIN` and FreeCAD. Ubuntu's own KiCad is older: use the KiCad project's PPA (`ppa:kicad/kicad-9.0-releases`). Ubuntu 24.04 and Fedora 44 have no FreeCAD package; use the official [AppImage](https://github.com/FreeCAD/FreeCAD/releases) (`FREECAD_VERSION`), which needs FUSE 2 (`libfuse2t64` / `fuse-libs`).

## Editor setup

**VS Code** extensions:
- nRF Connect for VS Code Extension Pack (`nordic-semiconductor.nrf-connect-extension-pack`): firmware builds, flashing and debugging.
- clangd (`llvm-vs-code-extensions.vscode-clangd`): C/C++. CMake writes `compile_commands.json` when `CMAKE_EXPORT_COMPILE_COMMANDS=1`.
- CMake Tools (`ms-vscode.cmake-tools`) for the core's presets.
- Flutter (`Dart-Code.flutter`).

**CLion** opens `core/` as a CMake project. Set the toolchain's C++ compiler to the one doctor reports.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Doctor: `C++23 probe … fails`, with `export CXX=…` in the note | Your default compiler is too old but a newer one is installed. Add that `export` to your shell profile |
| Doctor: `C++23 probe … won't run`, `GLIBCXX_… not found` | Something put an older libstdc++ on `LD_LIBRARY_PATH` (some IDE launchers do). Unset it in the shell you build from |
| Doctor: `flutter … same minor, different patch` | Usually harmless. Re-run bootstrap to move to the pinned patch |
| `.ps1 cannot be loaded because running scripts is disabled` | Run it with `powershell -ExecutionPolicy Bypass -File …` as shown above |
| Windows: `python` opens the Microsoft Store | That's the Store alias. Install Python with bootstrap, or turn the alias off under Settings → Apps → Advanced app settings → App execution aliases |
| Flashing fails with a permissions error on Linux | Missing udev rules or serial group (see Firmware above). Log out and back in after changing groups |
| `nrfutil toolchain-manager install` is slow | It downloads several GB once per NCS version |
