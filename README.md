# Micnote

Micnote is an open-source, privacy-first AI note-taking recorder you build and own.

Commercial note-taking recorders stream your raw conversations to proprietary cloud servers, leaving you with no way to verify how audio and transcripts are stored, processed, or shared. Micnote is designed from the silicon up to be completely local-first:

- **Standalone puck:** A small magnetic recorder captures voice memos and multi-hour meetings with or without a phone or computer nearby.
- **Local-first speech-to-text:** Audio syncs over an authenticated, encrypted link and transcribes on your own hardware via whisper.cpp.
- **In-app action execution:** Todo items and meeting action items are drafted into structured actions (sending emails, filing GitHub issues, reviewing PRs, creating calendar invites). You review and confirm every action in a dedicated editor—and the app carries out the action directly through the respective service API. You never leave the app to complete a task.
- **Privacy by default:** Local inference by default (whisper.cpp and llama.cpp). External model calls (e.g. Claude) are strictly opt-in per item with a redacted preview. Zero telemetry, zero cloud recording storage, and zero background data exfiltration.

---

## Architecture

```
[Puck: nRF5340 · 2 mics · 64 MB flash · Qi RX]
   firmware/ (C, Zephyr)
        │  BLE (LE Secure Connections + per-recording AEAD) or USB-C
        ▼
[Host: phone / tablet / desktop]
   app/  Flutter UI · action editors · connector executors · OAuth/keychain
     │ dart:ffi (C ABI, JSON payloads)
   core/ libmicnote (C++23)
         transport · sync protocol · Opus decode · whisper.cpp ASR ·
         llama.cpp / Claude planner · encrypted SQLite store · action schema registry
```

---

## Repository layout

- [`proto/`](proto/): Device↔host protocol buffer schemas (`micnote.proto`). Generated nanopb C code is committed under `proto/gen/`.
- [`firmware/`](firmware/): Zephyr RTOS application for Nordic nRF5340 (`west.yml` pins nRF Connect SDK; boards for Thingy:53, nRF5340-DK, and custom `micnote_puck`).
- [`core/`](core/): `libmicnote` C++23 library, `micnote-cli`, unit tests, and CMake presets.
- [`app/`](app/): Flutter client for desktop (Linux, macOS, Windows) and mobile (iOS, Android).
- [`hardware/`](hardware/): Puck schematics and PCB layout (KiCad) and enclosure models (FreeCAD).
- [`scripts/`](scripts/): Developer setup scripts (`bootstrap.sh`, `bootstrap.ps1`, `doctor.sh`, `doctor.ps1`, `Brewfile`).
- [`tools/`](tools/): Pinned toolchain versions (`versions.env`).
- [`docs/`](docs/): Architecture documentation, developer guide, threat model, and stage implementation plans ([`docs/stages/`](docs/stages/)).

---

## Hardware summary

- **SoC:** Nordic Semiconductor nRF5340 dual-core ARM Cortex-M33 (application + network core).
- **Microphones:** Dual PDM digital microphones with voice-optimized layout.
- **Audio pipeline:** 16 kHz capture encoded with fixed-point libopus at ~16 kbps (~7.2 MB/hour).
- **Storage:** 64 MB QSPI NOR flash on LittleFS. Audio chunks are encrypted with random per-recording AEAD keys before hitting flash.
- **Mounting & power:** Qi2/MagSafe magnetic ring, nPM1300 PMIC supporting Qi wireless charging and USB-C, fuel gauging, and haptic feedback.
- **Snap detection:** 3-axis Hall-effect sensor and accelerometer detect attachment to magnetic phone rings.

---

## Installing the app

Pre-built binaries and native installers will be available on the GitHub Releases page once Stage 5b is complete:

- **Linux:** AppImage, `.deb`, `.rpm`
- **macOS:** Universal `.dmg`
- **Windows:** `.msi` / installer executable
- **Mobile:** iOS TestFlight / Android APK & F-Droid releases

---

## Development setup

Micnote provides cross-platform bootstrap scripts that install and verify the required toolchains against pinned versions in [`tools/versions.env`](tools/versions.env). For complete manual instructions and IDE configuration, see the [Development Setup Guide](docs/dev-setup.md).

### 1. Bootstrap developer tools

**Linux (Ubuntu 24.04+, Debian 13+, Fedora, Arch) / macOS:**
```bash
./scripts/bootstrap.sh
```

**Windows 10/11 (PowerShell):**
```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```

Flags supported by both scripts:
- `--yes` / `-Yes`: Proceed without interactive confirmation prompts.
- `--no-firmware` / `-NoFirmware`: Skip nRF Connect SDK and embedded toolchains (useful for core/app only development).
- `--android` / `-Android`: Install Android SDK, NDK, and JDK.
- `--hardware` / `-Hardware`: Install KiCad and FreeCAD.

### 2. Verify environment

Run `doctor` to check installed compilers, libraries, and tools:
```bash
# Linux / macOS
./scripts/doctor.sh

# Windows
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1
```

### 3. Initialize firmware dependencies

```bash
# Inside the NCS toolchain environment
west init -l firmware
west update
```

### 4. Build components

- **Firmware (Thingy:53 / DK):**
  ```bash
  west build -b thingy53/nrf5340/cpuapp firmware/app
  # or
  west build -b nrf5340dk/nrf5340/cpuapp firmware/app
  ```
- **C++ core library & CLI (`libmicnote`):**
  ```bash
  cmake --preset dev
  cmake --build --preset dev
  ctest --preset dev
  ```
  Other presets: `asan`, `fuzz` and `release` (`CC=clang CXX=clang++` for the first two), plus `windows-msvc`, `android-arm64` and `ios`.
  CI requires 100% line, function and branch coverage of `core/`'s own code: run the `coverage` preset (gcc) and then `gcovr build/coverage` to check locally (`gcovr.cfg` has the rules).
- **Protocol schema:** after editing `proto/micnote.proto` or `proto/micnote.options`, regenerate the committed nanopb code:
  ```bash
  python3 proto/regen.py   # Windows: py proto\regen.py
  ```
  Its tests (`proto/test_regen.py`) run with `python3 -m unittest discover -s proto -p "test_*.py"`; CI also requires them to cover 100% of `regen.py` (see `pyproject.toml`).
- **Flutter application:**
  ```bash
  cd app
  flutter run
  ```

---

## Roadmap and status

Micnote is currently in **pre-alpha (Phase 0)**.

- [x] **Stage 0a:** Development environment & tooling verification
- [ ] **Stage 0b:** Project scaffolding & continuous integration
- [ ] **Stage 1:** Firmware and BLE/USB sync (Thingy:53 / DK prototype)
- [ ] **Stage 2:** Core transcription (whisper.cpp) & encrypted store
- [ ] **Stage 3:** Local & hybrid AI planner and action registry
- [ ] **Stage 4:** Desktop application & in-app action executors
- [ ] **Stage 5:** Mobile applications & background sync
- [ ] **Stage 5b:** Installers & distribution pipeline
- [ ] **Stage 6:** Custom puck hardware (KiCad Rev A & Rev B bring-up)
- [ ] **Stage 7:** Hardening, fuzzing, and security audits

Detailed progress notes and stage specifications are maintained in [`docs/PLAN.md`](docs/PLAN.md) and [`docs/stages/`](docs/stages/).

---

## License

This project is licensed under the [MIT License](LICENSE).
