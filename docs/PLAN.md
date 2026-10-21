# Micnote: final implementation plan

## Context
Commercial AI note-taking recorders send your audio to vendor clouds, and you can't check how that data is protected or used. Micnote is a recorder you build and own:

- A small magnetic **puck** records voice memos and meetings, with or without a phone nearby.
- It syncs to your phone or computer over an encrypted link.
- Speech-to-text runs on your own devices.
- Each todo item or meeting action becomes an AI-drafted **action**: send an email, file a GitHub issue, review a PR, and so on.
- You edit the action in a dedicated in-app editor and confirm it. **The app then carries it out itself through the service's API.** You never leave the app to finish a task.

The repo is greenfield: only the README, LICENSE (MIT), and .gitignore exist.

### Decisions made
| Area | Decision |
|---|---|
| Firmware | Embedded C on Zephyr (nRF Connect SDK), running on the nRF5340 chip |
| Core | C++23 library `libmicnote` (the newest ISO standard every target toolchain supports) |
| Apps | Flutter on iOS, iPadOS, Android, macOS, Windows and Linux, calling the C++ core through `dart:ffi` |
| AI | Hybrid. Local whisper.cpp and llama.cpp by default; a Claude API call is opt-in per item, after a redacted preview of what will be sent |
| Hardware | Prototype on a Thingy:53 and an nRF5340-DK, then a custom puck PCB with Qi2 magnets, wireless charging and haptics |
| Usage | Both quick memos and 30–90 minute meetings |
| Standalone | The puck records with no host nearby for days or weeks; hosts are only needed to process recordings |
| Actions | Every integration runs inside the app: AI draft → in-app editor → confirm → the app executes it via API |
| Python | Build tooling only (Zephyr/west, nanopb generator); the repo contains no Python code |
| Nix | Not in the repo. Your personal flake lives in NixHub; contributors use cross-OS bootstrap scripts; end users get native installers |

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

### Repo layout
- `proto/micnote.proto`: the device↔host message schema. nanopb generates C code for both firmware and core, and the generated code is committed in `proto/gen/`.
- `firmware/`: Zephyr app, `west.yml` (pins NCS; upstream code lands in `external/`), board files for `thingy53`, `nrf5340dk` and later `micnote_puck`.
- `core/`: `libmicnote`, `micnote-cli`, tests, `cmake/deps.cmake` (FetchContent pins), `CMakePresets.json`.
- `app/`: Flutter app, `distribute_options.yaml` (installers).
- `hardware/`: `puck/` (KiCad), `lib/` (local symbols, footprints and 3D models), `enclosure/` (FreeCAD).
- `scripts/`: `bootstrap.sh`, `bootstrap.ps1`, `Brewfile`, `doctor.sh`, `doctor.ps1`.
- `tools/versions.env`: pinned tool versions.
- `docs/`: dev-setup, protocol spec, threat model, action schema, hardware bring-up. Detailed per-stage plans live in `docs/stages/` (index: `docs/stages/README.md`).

## 1. Firmware (C, Zephyr, nRF5340)
- **Audio:**
  - Two PDM mics → 16 kHz → libopus fixed-point at about 16 kbps (about 7.2 MB per hour).
  - Encrypted, fixed-size chunks with sequence numbers, so a broken sync can resume.
  - Stored on QSPI NOR flash under LittleFS.
- **Controls:**
  - Button tap starts or stops a recording; a long press or a double-tap (accelerometer) marks an item boundary ("next todo").
  - RGB LED shows a visible recording indicator, battery and storage state.
  - Haptic patterns: short buzz for start/stop, double buzz for an item mark, long buzz when a sync completes.
- **Standalone operation:**
  - **Recording never needs a host.**
  - **Storage:** recordings are never overwritten before they sync. Warning at 90% full; recording stops with a long buzz when full.
  - **Time:** an RTC on a 32.768 kHz crystal is set on every sync. Each recording also stores a boot counter and uptime, so timestamps can be rebuilt if the battery died.
  - **Hosts:** up to 4 paired at once. A recording is deleted only after a host acknowledges receiving it.
- **Security:**
  - Each recording has a random content key, wrapped (X25519 ECIES) to the public key of every paired host. The puck cannot decrypt its own stored audio, so a stolen puck yields only ciphertext.
  - Before the first pairing, keys are wrapped to a device key stored in the KMU (the chip's key storage), then re-wrapped when a host pairs.
  - Encryption uses the CryptoCell-312 hardware.
  - MCUboot secure boot with signed images; debug port locked (APPROTECT) in release builds.
  - Signed firmware updates over BLE/USB (MCUmgr/SMP).
- **BLE:**
  - LE Secure Connections pairing, confirmed by comparing a color sequence on the puck's LED with the one shown in the app, plus a press of the puck's button (see `docs/stages/01-firmware-and-sync.md` §4.6).
  - A GATT control characteristic carries nanopb messages: `ListRecordings`, `Fetch{id,offset}`, `Ack`, `DeviceStatus`, `SetTime`.
  - Bulk audio moves over an L2CAP connection-oriented channel, falling back to GATT notifications.
  - The puck advertises **only** when snapped onto a phone, when the button is pressed, or briefly after a recording ends, always with a resolvable private address. That makes it hard to track over the air.
- **Snap detection:**
  - A 3-axis Hall sensor watches for a *change* in the magnetic field compared with a baseline calibrated at boot (the puck's own magnet ring would saturate a simple on/off sensor). An accelerometer shock confirms the snap.
  - When the Qi receiver reports a charger, the puck treats it as "on a charger", not "on a phone" (except Pixel Battery Share, where it is also on a phone).
- **USB:** a USB CDC interface for fast desktop sync and firmware updates.
- **Power:** the nPM1300 PMIC charges from USB-C or Qi and provides a fuel gauge, with both reported in `DeviceStatus`.

## 2. C++ core (`libmicnote`, C++23)
- **Standard:** `CMAKE_CXX_STANDARD 23` with extensions off.
  - Features used: `std::expected` (errors across the FFI), `std::span`/`mdspan`, `std::format`/`print`, `std::flat_map`, `std::move_only_function`, deducing `this`.
  - CI adds an optional `-std=c++2c` job. The baseline moves to C++26 once Apple clang, the Android NDK and MSVC all support it; reflection could then generate the schemas and FFI glue.
- **Dependencies:** all fetched through FetchContent with pinned versions: opus, GoogleTest, SimpleBLE, whisper.cpp, llama.cpp, nanopb, SQLite3MultipleCiphers. The only system libraries are ones the OS provides (dbus/BlueZ on Linux, plus optional Vulkan).
- **Transports** (`ITransport`):
  - SimpleBLE on desktop.
  - USB CDC serial on desktop.
  - A byte-pipe bridge on mobile, because native Swift/Kotlin owns the BLE lifecycle there to get OS background wake-up.
- **Sync:** list → fetch with resume → verify each chunk's MAC → unwrap the key and decrypt → ack.
- **Audio and speech-to-text:**
  - Opus decode → resample → voice-activity detection → whisper.cpp.
  - Acceleration: Metal/Core ML on Apple, Vulkan/CUDA on desktop, NEON on Android.
  - Models: `large-v3-turbo` quantized on desktop, `base.en`/`small.en` on mobile.
- **Planner:**
  1. **Segment** the recording (todo list vs. meeting notes, using item markers from the puck as hints) and extract the items.
  2. **Produce one typed action per item**, chosen only from registered connector schemas. Each action carries its parameters, a rationale, a risk level and a confidence score.
  - Local planning uses llama.cpp (3–8B instruct model, Q4) with GBNF grammar-constrained JSON.
  - Cloud planning (opt-in) uses Claude (`claude-opus-5-5` by default, configurable) with structured outputs (JSON schemas generated from the same registry), sent only after you approve the redacted preview. See `docs/stages/03-planner.md`.
  - The planner may also call **read-only** context fetchers, for example to resolve "Jeff" against your contacts or pull a PR diff, so the draft starts out accurate.
- **Store:** SQLite with SQLite3MultipleCiphers encryption holds recordings, transcripts, actions (with an edit history) and an execution audit log. The encryption key lives in the OS keychain.
- **API:** a flat C ABI (`micnote.h`) with opaque handles, async callbacks and UTF-8 JSON. `micnote-cli` (`pair`, `sync`, `transcribe`, `plan`) is both the dev harness and the hardware bring-up tool.

## 3. Flutter app: actions run in-app, end to end
### Action lifecycle
```
AI draft ──► Review (connector editor) ──► Confirm ──► Execute (in-app API call) ──► Result + audit log
                 ▲  edit / regenerate / discard             │ failure → back to editor with error
```
- Each action is shown in its connector's **native editor screen**, not as raw JSON. The editor is fully editable and validated live against the connector's schema.
- Buttons: **Confirm**, **Regenerate** (with an optional instruction such as "make it shorter"), and **Discard**.
- **Confirm** shows a summary of the exact final payload ("Send to jeff@… · Subject … · 1 attachment").
  - Higher-risk actions need a second step: a hold-to-confirm for sending an email or submitting an APPROVE review.
  - Then the **app executes it directly through the service's API**. It never hands off to another app, a compose sheet, `mailto:` or a browser.
- **Undo where the service allows it:**
  - Email is held for a 10-second undo window before it's actually sent.
  - A GitHub issue can be closed again.
  - A calendar event can be deleted.
- Results link back to the created object (issue URL, sent message). Everything is recorded in the audit log.
- Execution is idempotent: each action carries an idempotency key, so a retry after a network failure can't double-send.

### Connector interface (Dart, `app/lib/connectors/`)
Each connector implements:
- `schema`: the JSON schema registered with the core planner.
- `contextFetchers`: read-only lookups (contacts, repos, labels, the PR diff).
- `editor`: a Flutter widget that edits a typed draft.
- `validate`: returns field-level errors shown in the editor.
- `preview`: the human-readable "what will happen" summary for the confirm sheet.
- `execute`: makes the API call(s) and returns a result or a typed error.
- `undo`: optional.

**Auth:** OAuth 2 with PKCE inside an in-app auth sheet (ASWebAuthenticationSession on iOS/macOS, Custom Tabs on Android, an embedded webview or loopback redirect on desktop), or device flow for GitHub. This happens once per account at setup, never while running an action. Tokens are stored in the OS keychain.

### Integrations (v1)
| Connector | Editor UI | Executes via |
|---|---|---|
| **Email** | To/Cc/Bcc chips with contact autocomplete; subject; rich-text body with an AI rewrite toolbar; attachments; a choice of which account to send from | Gmail API `messages.send`, Microsoft Graph `sendMail`, or IMAP/SMTP for any other provider. The sent mail appears in your Sent folder |
| **GitHub issue** | Repo picker, title, markdown body with live preview, labels, assignees, milestone, project | GitHub REST `POST /repos/{o}/{r}/issues` |
| **GitHub PR review** | In-app diff viewer (file tree, side-by-side or unified view). AI-drafted inline comments are anchored to lines and each can be edited, moved or deleted; a summary comment; a verdict picker (Comment / Approve / Request changes) | Pending review created, comments added, then `submitEvent`, all via GraphQL |
| **GitHub comment** | Target (issue or PR), markdown body with preview | REST comments API |
| **Calendar event** | Title, time with timezone, attendees, location/video link, notes | EventKit (iOS/macOS) or CalendarContract (Android), all in-process; Google Calendar or Graph on desktop |
| **Reminder / task** | Title, due date, list, notes | EventKit Reminders, Google Tasks, Microsoft To Do |

Later connectors use the same interface: Slack, Linear, Jira, Notion.

### Other app features
- **Screens:** pairing and status (battery, storage, firmware version); inbox (recordings → transcripts → actions); action queue; connector accounts; privacy settings (cloud toggle, redaction rules, model management); firmware update.
- **Auto-sync when the puck snaps on:**
  - **iOS:** Core Bluetooth `bluetooth-central` background mode with state restoration and a pending `connect()` to the bonded puck. Transcription runs later in a `BGProcessingTask`.
  - **Android:** `CompanionDeviceManager` presence observation wakes a `CompanionDeviceService`; a `connectedDevice` foreground service does the transfer, and WorkManager runs transcription.
  - **Desktop:** a tray app that auto-connects over BLE or USB.
- **Notifications:** "3 new actions ready to review". Tapping one opens straight into the editor.

## 4. Hardware
### Sizing numbers that drive the choices
- **Storage:** about 7.2 MB per hour, so 64 MB holds about 9 hours.
- **Sync time:** BLE at roughly 0.4–1 Mbps syncs an hour of audio in 1–2 minutes; USB takes seconds.
- **Power:** recording draws about 5–7 mA, sleep 10–30 µA. A 250 mAh cell gives about 35 hours of recording, or weeks on standby.

### Stage A: prototype (about $150)
- **Nordic Thingy:53:** nRF5340, VM3011 PDM mic, 8 MB flash, BMM150 magnetometer, accelerometers, nPM1100 PMIC, 1350 mAh battery, RGB LED, USB-C. With an adhesive MagSafe ring (54–56 mm outer, 46 mm inner) stuck on, it is the walking prototype for snap detection.
- **nRF5340-DK:** debugger and bench board, with IM73D122 mics, a 64 MB QSPI NOR, a TMAG5273 Hall sensor, and a DRV2605L + LRA on breakouts.
- **Power Profiler Kit II:** measures real current draw.
- **BQ51013B evaluation module + Rx coil:** for bench-testing Qi charging.

### Stage B: custom puck BOM (about $40–60 at quantity 5)
| Function | Part |
|---|---|
| SoC | Raytac MDBT53 (pre-certified nRF5340 module, chip antenna; u.FL variant as Plan B) |
| PMIC | Nordic nPM1300 (charger, buck converters, load switches, fuel gauge, LED drivers) |
| Mics | 2× Infineon IM73D122 (73 dB SNR PDM, bottom-port), one on each side of the rim |
| Storage | Winbond W25Q512NW (64 MB, 1.8 V QSPI NOR) |
| Sensors | TI TMAG5273 3-axis Hall sensor, ST LIS2DW12 accelerometer |
| Feedback | RGB LED (nPM1300 drivers), DRV2605L + LRA, side tactile button |
| Wireless charging | TI BQ51013B Qi receiver + Würth Rx coil inside the ring + ferrite sheet; TI TPS2116 picks between Qi and USB-C power for the nPM1300 |
| Power and IO | 250 mAh LiPo with NTC, USB-C (16-pin, CC pull-downs, ESD protection) |
| Mount | N52 MagSafe/Qi2-compatible magnet ring, 54 × 46 mm. The same ring works on iPhone MagSafe and Pixel Pixelsnap |

**Enclosure:** about 57 mm across × 10–11.5 mm thick, SLA resin print. The back wall over the coil is ≤1.5 mm and non-metallic.

### PCB design
**Tools:** KiCad 9 in `hardware/puck/`, with hierarchical sheets: power, mcu, audio, storage, sensors, ui_haptics, wireless_charging, connectors.

**Stack inside the puck, top to bottom:**
1. Top shell, button and light pipe.
2. PCB, about 50 mm round, components on the top side only.
3. LiPo.
4. Ferrite sheet.
5. Coil inside the magnet ring.
6. Back wall.

**Board:**
- 4-layer, 1.0 mm thick, JLCPCB rules (≥0.1 mm track and space, 0.2/0.45 mm vias).
- Layers: L1 signals + parts, L2 solid ground, L3 power + QSPI, L4 ground + low-speed signals.
- 0402 passives. QFN/LGA packages only (no BGA), so it can be hand-reworked.

**Power tree:**
- USB-C VBUS and the Qi 5 V output go through the TPS2116 into nPM1300 VBUS.
- BUCK1 at 1.8 V feeds the module, flash, mics and sensors.
- LOADSW1 gates mic power.
- VSYS feeds the DRV2605L.
- 0 Ω jumpers on each rail allow per-rail current measurement with the PPK2.

**Pins:** shared PDM CLK/DIN with L/R straps; QSPI on its fixed pins; one I²C bus (PMIC, Hall, accelerometer, haptic); interrupts for the accelerometer, Hall sensor, PMIC and button; Qi PG/CHG; USB on the dedicated pins; SWD on a Tag-Connect TC2030-NL footprint plus UART pads.

**Layout rules:**
- Module antenna on the board edge with its keep-out and a stitching-via fence. The magnet ring runs under the outer 4 mm of the board, so test RF performance with the puck on each phone.
- Bottom-port mic holes drilled through the PCB, with gasketed acoustic channels to holes in the side rim (mics about 45 mm apart).
- Short PDM clock with ground guarding, kept away from the coil and the BQ51013B's switching node.
- LRA and Hall sensor at least 5 mm from the magnet ring.
- Qi receiver away from the battery's NTC.
- Fiducials, a round outline with mouse bites, and test points on every rail, I²C, PDM and QSPI CS.

**Checks:** CI runs `kicad-cli sch erc` and `pcb drc` and exports gerbers, drill, BOM and CPL to `hardware/puck/fab/`. Before ordering, review against the Nordic, Raytac and nPM1300 reference designs.

**Revisions and bring-up:**
- **Rev A:** final outline, 5 boards fully assembled.
- **Bring-up order** (written up in `docs/hardware-bringup.md`):
  1. Current-limited power-on; check the rails.
  2. SWD connect; flash blinky.
  3. Zephyr shell tests for each peripheral.
  4. USB enumeration.
  5. RSSI with the puck on each phone.
  6. Qi charging under a thermal camera.
  7. PPK2 power profile.
- **Rev B:** fixes plus the final enclosure.

### Alternatives considered
- **ESP32-S3:** draws about 10× the power. Rejected.
- **nRF54L15:** no USB.
- **nRF54LM20A:** has high-speed USB and more memory. The migration target for a later board revision.

## 5. Distribution and installers (end users)
End users never run scripts. Every platform gets a signed native package, built in CI, that bundles `libmicnote` and all libraries.

| Platform | Artifact | Signing / updates | Platform notes |
|---|---|---|---|
| macOS | Universal `.dmg`; Homebrew cask | Developer ID signing and notarization; Sparkle with an EdDSA-signed appcast | Bluetooth entitlement and usage strings |
| Windows | Inno Setup `.exe` (per-user install, no admin needed); winget manifest; MSIX later | Azure Trusted Signing; WinSparkle | Static CRT; WinRT BLE; USB CDC needs no driver |
| Linux | `.deb`, `.rpm`, `.AppImage`; Flatpak later | GPG-signed packages, later signed apt/dnf repos; AppImage zsync updates | deb/rpm declare dependencies on bluez, gtk3, libsecret (and recommend libvulkan1) and ship the `70-micnote.rules` udev rule. The AppImage offers to install the rule via `pkexec` |
| iOS / iPadOS | App Store + TestFlight | Apple distribution certificate | `bluetooth-central` background mode; privacy label "Data Not Collected" |
| Android | Play AAB; signed APK on GitHub Releases (works with Obtainium); F-Droid later | Play App Signing, plus our own key for the APK | BLE, companion-device and `connectedDevice` foreground service permissions |

- **Packaging tools:** `fastforge` (`app/distribute_options.yaml`) for dmg, exe, deb, rpm and AppImage; the Flutter CLI with Xcode/Gradle for mobile.
- **Bundled with the app:** the matching signed puck firmware, offered as an in-app update over BLE or USB.
- **Not bundled:** AI models. They are downloaded on first run from pinned URLs and checked against pinned SHA-256 hashes, or loaded from a local file. That download is the only network access the app makes without you asking, and there is no telemetry.
- **USB identity:** a free VID/PID from pid.codes.
- **`release.yml`:** runs on a `v*` tag across ubuntu, macos and windows runners plus a Zephyr container. Every release gets SHA-256 sums, a CycloneDX SBOM and signatures, with app and firmware versions in lockstep.

## 6. Development environment
**The repo itself contains no Nix files.**

1. **Project dependencies, pinned in the repo:**
   - `firmware/west.yml` (NCS and Zephyr → `external/`);
   - `core/cmake/deps.cmake` (FetchContent);
   - `CMakePresets.json` (`dev`, `release`, `asan`, `android-*`, `ios`);
   - `app/pubspec.lock`;
   - committed nanopb code in `proto/gen/` (CI checks it matches a fresh regeneration).
2. **Contributor system dependencies:** the bootstrap scripts.
   - `tools/versions.env` pins the NCS version, Flutter version, minimum CMake/Ninja, minimum compilers (clang ≥18, gcc ≥14, MSVC 17.10+) and the NDK.
   - `scripts/bootstrap.sh` (Linux via apt/dnf/pacman; macOS via Homebrew + `Brewfile`) and `scripts/bootstrap.ps1` (Windows via winget):
     - **Base install:** compilers, CMake, Ninja, and the Linux dev headers.
     - **Firmware:** `nrfutil` + `toolchain-manager` with the pinned NCS; SEGGER J-Link; Nordic udev rules and `dialout` on Linux.
     - **App:** the Flutter SDK at the pinned version. On Windows, VS 2022 Build Tools.
     - **Optional:** `--android` and `--hardware` (KiCad, FreeCAD).
     - The scripts show their plan and ask for confirmation (`--yes` skips the prompt), and are idempotent.
   - `scripts/doctor.sh` and `scripts/doctor.ps1` check installed tools against `versions.env`.
   - `docs/dev-setup.md` covers manual install and IDE setup.
3. **Personal NixOS flake, in NixHub only:** `~/Documents/NixHub/dev/Personal/Micnote/flake.nix`.
   - **Conventions:** follows `dev/Personal/Metrology/flake.nix`: `nixos-unstable`, `forEachSystem`, `allowUnfree`, `../../lib/shell-banner.nix`. The automatic `Micnote` alias comes from `lib/dev-flake-aliases.nix`.
   - **Inputs:** `nixpkgs` and `zephyr-nix`. zephyr-nix supplies the pinned Zephyr SDK (`arm-zephyr-eabi`), because Nordic's toolchain-manager binaries don't run on NixOS.
   - **pkgs config:** `allowUnfree`, `android_sdk.accept_license`, `segger-jlink.acceptLicense`.
   - **devShells:**
     - **`default`:**
       - Firmware: Zephyr SDK, cmake, ninja, dtc, gperf, ccache, python3 + uv (`.venv` with west and the NCS requirements), nrfutil, segger-jlink, pyocd, nrfconnect.
       - Core: `llvmPackages_latest` (clang, clangd, tidy, format, lld, libFuzzer), latest gcc, pkg-config, protobuf (`protoc` for nanopb regeneration), dbus, bluez, Vulkan headers/loader/shaderc, whisper-cpp and llama-cpp CLIs.
       - App: flutter + gtk3, libsecret, libepoxy, xz, with `LD_LIBRARY_PATH`/`PKG_CONFIG_PATH` exported.
     - **`android`:** `default` plus androidenv (pinned NDK).
     - **`hardware`:** kicad, freecad, openscad.
   - **shellHook:** banner; exports `ZEPHYR_TOOLCHAIN_VARIANT`, `ZEPHYR_SDK_INSTALL_DIR` and `CMAKE_EXPORT_COMPILE_COMMANDS=1`; syncs the venv; hints `west init -l firmware && west update`; runs `scripts/doctor.sh`.
   - **Host change:** add `pkgs.nrf-udev` to `services.udev.packages` in `~/Documents/NixHub/hosts/atta/configuration.nix`. You run `nixos-rebuild switch` yourself. Nothing is committed in NixHub unless you ask.

### `.gitignore` additions (on top of the existing C++ template)
- **West/Zephyr:** `.west/`, `external/`, `twister-out*/`.
- **Python:** `.venv/`, `__pycache__/`, `*.pyc`.
- **Flutter:** `app/.dart_tool/`, `app/build/`, `.flutter-plugins*`, `app/**/Pods/`, `app/ios/Flutter/Generated.xcconfig`, `flutter_export_environment.sh`, `app/**/ephemeral/`, `app/android/local.properties`, `app/android/.gradle/`, `*.iml`, `.idea/`.
- **Packaging output:** `dist/`.
- **Secrets:** `*.pem`, `*.key`, `*.jks`, `*.keystore`, `*.p12`, `.env*`, `firmware/keys/`.
- **Models and audio:** `models/`, `*.gguf`, `ggml-*.bin`, `/recordings/`, with `!core/tests/fixtures/**` re-included.
- **KiCad:** `*-backups/`, `fp-info-cache`, `*.kicad_prl`, `_autosave-*`, `\#auto_saved_files\#`, `*.lck`, but keep `hardware/puck/fab/` tracked.
- **OS/editor:** `.DS_Store`, `.vscode/*` with `!.vscode/extensions.json`.

### `README.md`
Sections:
1. What Micnote is, and its privacy principles: local-first, explicit confirmation, actions run in-app, no telemetry.
2. Architecture diagram.
3. Repo layout.
4. Hardware summary.
5. Installing the app: links to releases per platform.
6. Development setup: bootstrap → doctor → west init/update → building firmware (`west build -b thingy53/nrf5340/cpuapp firmware/app`), the core (`cmake --preset dev`) and the app (`flutter run`).
7. Roadmap and status ("pre-alpha, Phase 0").
8. License (MIT).

The README does not mention Nix.

## Milestones
Stage progress is tracked here: a box is ticked only when its "Done when" is met, and the stage being worked on gets an *In progress* line. Task-level progress is the checkboxes in each stage doc's task breakdown.

- [ ] **[0a](stages/00a-dev-environment.md) Dev environment:** bootstrap/doctor scripts, `versions.env`, NixHub flake + udev, README, .gitignore.\
  *Done when:* `doctor` passes on Linux, macOS and Windows (CI) and inside the Nix shell; secret and build paths are ignored.\
  *In progress:* NixHub flake, udev rule, `versions.env` and doctor done; bootstrap scripts (PR 2) next. The flake still pins Zephyr SDK 0.17.4, and NCS v3.4.1 needs 1.0.1.
- [ ] **[0b](stages/00b-scaffolding-ci.md) Scaffolding and CI:** firmware, core, app and proto skeletons; CI (firmware build, core tests on 3 OSes, `flutter analyze`, KiCad ERC/DRC); threat model.\
  *Done when:* CI is green.
- [ ] **[1](stages/01-firmware-and-sync.md) Firmware and sync:** firmware on the Thingy:53/DK (record, Opus, encrypted storage, standalone behavior, BLE/USB sync) together with the core's transport and protocol + `micnote-cli pair`/`sync`.\
  *Done when:* `micnote-cli sync` pulls a recording made with no host present; it decrypts and plays back.
- [ ] **[2](stages/02-core-transcription.md) Core: transcription and store:** decode, whisper.cpp, encrypted store, `micnote-cli transcribe`.\
  *Done when:* a DK recording becomes a transcript; protocol and crypto unit tests pass.
- [ ] **[3](stages/03-planner.md) Planner:** planner + action schema registry (local and Claude), `micnote-cli plan`.\
  *Done when:* a golden set of about 30 recorded todos gives valid, correct actions.
- [ ] **[4](stages/04-desktop-app-and-actions.md) Desktop app and in-app actions:** Flutter desktop (Linux first): inbox, action queue, connector framework, Email + GitHub issue/PR review editors and executors, OAuth, audit log, undo.\
  *Done when:* "Email Jeff about X" and "review PR #N" each go from recording → editor → confirm → executed, without leaving the app.
- [ ] **[5](stages/05-mobile-and-autosync.md) Mobile apps and auto-sync:** iOS/Android native BLE bridges, background auto-sync, calendar and reminder connectors.\
  *Done when:* snapping the puck on with the app in the background syncs within about 10 s; actions run in-app on the phone.
- [ ] **[5b](stages/05b-distribution.md) Installers and releases:** installers and the release pipeline.\
  *Done when:* clean VMs (Ubuntu, Fedora, AppImage on any distro, macOS, Windows) and fresh phones install the app, pair, sync and execute an action, with no dev tools present.
- [ ] **[6a](stages/06a-pcb-rev-a.md) PCB Rev A design** (can run in parallel with stages 2–4).\
  *Done when:* ERC/DRC clean, review passes, boards ordered.
- [ ] **[6b](stages/06b-bringup.md) Rev A bring-up:** `micnote_puck` board port, OTA, battery tuning.\
  *Done when:* every bring-up step passes; the puck lasts a full day on battery.
- [ ] **[6c](stages/06c-rev-b-enclosure.md) Rev B and enclosure.**\
  *Done when:* the puck snaps onto an iPhone and a Pixel, syncs and charges on a Qi pad.
- [ ] **[7](stages/07-hardening.md) Hardening:** libFuzzer on parsers, security review, BLE pairing pen-test.\
  *Done when:* no open high-severity findings.

## Verification
- **Firmware:** Zephyr `twister` tests (framing, key wrapping, storage policy); RTT logs; a standalone test that records for days with no host, then syncs everything.
- **Protocol:** `micnote-cli` against real hardware; libFuzzer on the nanopb decoding and framing parsers; nRF Sniffer + Wireshark to confirm the link is encrypted.
- **Core:** GoogleTest; word-error rate against reference transcripts; planner eval (schema validity, intent accuracy).
- **Actions:**
  - Unit tests for each connector: validate, preview, and execute against recorded HTTP fixtures.
  - Flutter widget tests for every editor (edit → validate → confirm sheet shows the exact payload).
  - Integration tests that execute against sandbox accounts (a test GitHub repo, a test mailbox), checking idempotency and undo.
  - A test that fails if any code path launches an external URL or another app during execution.
- **Hardware:** the bring-up checklist, PPK2 power profiles, thermal camera during Qi charging, RSSI with the puck on each phone.
- **Privacy:** in local-only mode, a network monitor (OpenSnitch or Little Snitch) shows no traffic except confirmed connector calls and model downloads.
- **Distribution:** install tests on clean VMs and devices for every artifact; signatures and checksums verified.

## First step after approval
Phase 0a, then 0b. Write the bootstrap/doctor scripts and `tools/versions.env`, the NixHub flake and udev line, the README and the .gitignore. Then scaffold `proto/`, `firmware/`, `core/` and `app/` with CI.
