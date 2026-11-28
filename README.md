# Micnote

Micnote is an open-source, privacy-first AI note-taking recorder you build and own.

> **New here?** You don't need a technical background to follow this page: each section starts in plain words, and the technical details follow for those who want them. For the whole story without any jargon, read [How Micnote works](docs/how-it-works.md). Any unfamiliar word is explained in the [glossary](docs/glossary.md).

**In plain words:** Micnote is a small round recorder that snaps magnetically onto the back of your phone, or works on its own in a pocket or on a desk. You record a quick reminder, a to-do list or a whole meeting. Afterwards, your own phone or computer turns the recording into text and drafts the follow-up work for you (an email, a calendar event, a GitHub issue, a code review). You check each draft, change what you like, press confirm, and the app does it for you. Nothing is sent to anyone's servers unless you choose to for a particular item, and you see exactly what would be sent first.

Commercial note-taking recorders stream your raw conversations to proprietary cloud servers, leaving you with no way to verify how audio and transcripts are stored, processed, or shared. Micnote is designed from the silicon up to be completely local-first:

- **Standalone puck:** A small magnetic recorder captures voice memos and multi-hour meetings with or without a phone or computer nearby.
  *In plain words:* the recorder (the "puck") doesn't need your phone with it. It keeps everything safely until your phone or computer collects it.
- **Local-first speech-to-text:** Audio syncs over an authenticated, encrypted link and transcribes on your own hardware via whisper.cpp.
  *In plain words:* recordings travel to your devices scrambled so nobody else can listen in, and the speech is turned into text by free software (whisper.cpp) running on your own phone or computer, not on someone else's server.
- **In-app action execution:** Todo items and meeting action items are drafted into structured actions (sending emails, filing GitHub issues, reviewing PRs, creating calendar invites). You review and confirm every action in a dedicated editor—and the app carries out the action directly through the respective service API. You never leave the app to complete a task.
  *In plain words:* for each thing you said you'd do, the app prepares the work: it writes the email, fills in the issue, drafts the review. Each draft opens in a proper editor for that kind of task. When you confirm, the app sends it itself by talking directly to the service (Gmail, GitHub, your calendar), so you don't have to copy and paste into another app. (A GitHub *issue* is a tracked task or bug report; a *PR*, or pull request, is a proposed change to some code, and *reviewing* it means commenting on it and approving it or asking for changes.)
- **Privacy by default:** Local inference by default (whisper.cpp and llama.cpp). External model calls (e.g. Claude) are strictly opt-in per item with a redacted preview. Zero telemetry, zero cloud recording storage, and zero background data exfiltration.
  *In plain words:* both AI programs (one for speech, llama.cpp for drafting) run on your own device. For a tricky item you can ask a more capable online AI (Claude) to draft it instead, but only for that item, after Micnote has blanked out details like email addresses and phone numbers and shown you exactly what would be sent. The app never reports how you use it ("telemetry"), never stores your recordings online, and never sends data in the background.

---

## Architecture

**In plain words:** there are three parts. The **puck** records and encrypts. Your **phone, tablet or computer** (a "host") collects the recordings over Bluetooth or a USB cable. On the host, the **app** is what you see and use, and underneath it the **core** is the engine that does the work: talking to the puck, unlocking and storing recordings, turning speech into text and drafting actions. The same core runs under every version of the app.

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

**Reading the diagram, line by line:**
- **Puck:** its main chip is a Nordic **nRF5340** (a whole tiny computer with a Bluetooth radio, on one chip). It has 2 microphones, 64 MB of **flash** memory (storage that keeps its contents without power) and a **Qi RX** (a receiver for wireless charging pads). Its built-in software, the **firmware**, is written in the C language on **Zephyr**, a small operating system for chips like this.
- **The link:** recordings travel over **BLE** (Bluetooth Low Energy, the low-power Bluetooth used by watches and trackers) or a USB-C cable. *LE Secure Connections* is Bluetooth's modern secure pairing; *per-recording AEAD* means every recording is also encrypted with its own key in a way that detects any tampering, so it's protected even apart from Bluetooth's own security.
- **App:** built with **Flutter**, which lets one app run on iPhone, Android, Mac, Windows and Linux. It contains the editors for each kind of action, the *connector executors* (the code that actually sends the email or creates the issue), and the sign-in to your accounts (**OAuth**, the "Sign in with Google" process, which never shows the app your password). Sign-ins are kept in your system's **keychain** (its protected vault for passwords).
- **dart:ffi (C ABI, JSON payloads):** how the app (written in the Dart language) calls the core (written in C++): through a small set of plain C functions that pass requests and answers as **JSON**, a simple text format for structured data.
- **Core, `libmicnote`:** written in C++23 (the 2023 version of the C++ language). In order: the *transport* (Bluetooth or USB connection), the *sync protocol* (the rules for collecting recordings), *Opus decode* (unpacking the compressed audio), *whisper.cpp ASR* (automatic speech recognition), the *planner* (the AI that drafts actions, local llama.cpp or opt-in Claude), an *encrypted SQLite store* (a database file nobody can read without its key), and the *action schema registry* (the exact description of every kind of action the app can do).

---

## Repository layout

**In plain words:** the project's files are grouped into folders, one per part of the system. A *repository* (or *repo*) is the whole collection of a project's files and their history.

- [`proto/`](proto/): Device↔host protocol buffer schemas (`micnote.proto`). Generated nanopb C code is committed under `proto/gen/`.
  *The exact definition of every message the puck and the app send each other. A tool (nanopb) turns it into code for both sides, so they can never disagree.*
- [`firmware/`](firmware/): Zephyr RTOS application for Nordic nRF5340 (`west.yml` pins nRF Connect SDK; boards for Thingy:53, nRF5340-DK, and custom `micnote_puck`).
  *The software inside the puck. It can be built for two off-the-shelf prototype boards (the Thingy:53 and the nRF5340-DK) and for Micnote's own board. `west.yml` fixes the exact version of Nordic's software kit it builds on.*
- [`core/`](core/): `libmicnote` C++23 library, `micnote-cli`, unit tests, and CMake presets.
  *The shared engine, a command-line tool for developers (`micnote-cli`), the automatic tests, and the build settings.*
- [`app/`](app/): Flutter client for desktop (Linux, macOS, Windows) and mobile (iOS, Android).
  *The app you use, for every platform.*
- [`hardware/`](hardware/): Puck schematics and PCB layout (KiCad) and enclosure models (FreeCAD).
  *The design of the puck's circuit board (made in the free KiCad program) and its case (made in FreeCAD).*
- [`models/`](models/): `manifest.json`, the pinned list of AI models (download address, size, SHA-256) and which one each kind of device uses by default.
  *The list of speech-recognition models the app can download, each with a fingerprint so a corrupted or tampered download is rejected. The model files themselves are downloaded, not stored here.*
- [`scripts/`](scripts/): Developer setup scripts (`bootstrap.sh`, `bootstrap.ps1`, `doctor.sh`, `doctor.ps1`, `Brewfile`).
  *Scripts that install (bootstrap) and check (doctor) everything a developer needs.*
- [`tools/`](tools/): Pinned toolchain versions (`versions.env`).
  *The one file that says which version of each development tool to use.*
- [`docs/`](docs/): Architecture documentation, developer guide, threat model, and stage implementation plans ([`docs/stages/`](docs/stages/)).
  *All the documentation. See [the documentation map](#documentation-map) below.*

---

## Hardware summary

**In plain words:** the puck is built from standard parts: one main chip that does the computing and Bluetooth, two microphones, a memory chip for recordings, a power chip that charges the battery from a cable or a wireless charging pad, a magnet ring that holds it on a phone, and two sensors that notice when it's snapped on.

- **SoC:** Nordic Semiconductor nRF5340 dual-core ARM Cortex-M33 (application + network core).
  *A "System on Chip": a whole small computer on one chip, with two processors: one runs Micnote's software, the other runs the Bluetooth radio.*
- **Microphones:** Dual PDM digital microphones with voice-optimized layout.
  *Two tiny digital microphones of the kind phones use, placed to pick up voices well.*
- **Audio pipeline:** 16 kHz capture encoded with fixed-point libopus at ~16 kbps (~7.2 MB/hour).
  *Sound is measured 16,000 times a second (plenty for speech) and compressed with Opus, a modern audio format, to about 7.2 MB per hour: the 64 MB memory holds about 9 hours.*
- **Storage:** 64 MB QSPI NOR flash on LittleFS. Audio chunks are encrypted with random per-recording AEAD keys before hitting flash.
  *A 64 MB memory chip, organised with LittleFS, a file system built to survive sudden power loss. Audio is encrypted before it's ever written, with a new random key for each recording, so a stolen puck reveals nothing. The puck itself can't decrypt it: only your paired devices can.*
- **Mounting & power:** Qi2/MagSafe magnetic ring, nPM1300 PMIC supporting Qi wireless charging and USB-C, fuel gauging, and haptic feedback.
  *A magnet ring compatible with iPhone MagSafe, the Qi2 standard and Google's Pixelsnap. A power chip (PMIC, power management chip) charges the battery from a wireless pad or USB-C, estimates how much charge is left ("fuel gauge"), and a small motor gives buzzes you can feel ("haptic feedback").*
- **Snap detection:** 3-axis Hall-effect sensor and accelerometer detect attachment to magnetic phone rings.
  *A magnetic-field sensor (Hall sensor) notices the phone's magnets, and a motion sensor (accelerometer, the kind that rotates your phone screen) feels the small knock of snapping on. Together they tell the puck it's on a phone, so it can sync.*

---

## Installing the app

**In plain words:** ready-made installers, the kind you download and double-click, aren't available yet. They're planned for stage 5b (see [the roadmap](#roadmap-and-status)). Until then, Micnote can only be built from source by developers, using the steps below.

Pre-built binaries and native installers will be available on the GitHub Releases page once Stage 5b is complete:

- **Linux:** AppImage, `.deb`, `.rpm`
  *An AppImage runs on almost any Linux; `.deb` is for Ubuntu and Debian; `.rpm` is for Fedora and similar.*
- **macOS:** Universal `.dmg`
  *A normal Mac disk image, for both Apple-chip and Intel Macs.*
- **Windows:** `.msi` / installer executable
- **Mobile:** iOS TestFlight / Android APK & F-Droid releases
  *TestFlight is Apple's way of installing apps before they're in the App Store; an APK is an Android app file; F-Droid is a store for open-source Android apps.*

---

## Development setup

**In plain words:** this section is for building Micnote yourself from its source code. It takes three steps: install the tools (one script does it), check them (another script), then build each part.

### Before you start: the basics

If you've never built software from source, here's what you need to know first.

- **A terminal** is a window where you type commands instead of clicking. On macOS, open *Terminal* (in Applications → Utilities). On Linux, open *Terminal* from your applications menu. On Windows, open *PowerShell* from the Start menu. Each command below is typed (or pasted) into it and run with Enter. Lines starting with `#` inside the code boxes are comments to explain things; you don't type them.
- **git** is the tool that downloads a project's code and keeps track of its history. If you don't have it, install it from [git-scm.com](https://git-scm.com/downloads) (on macOS, running `git` in Terminal offers to install it for you). Then download Micnote and move into its folder:
  ```bash
  git clone https://github.com/ElliotScher/Micnote.git
  cd Micnote
  ```
  (`git clone` copies the project to your computer; `cd` means "change directory", moving the terminal into that folder. Every command below is run from inside it.)
- **A toolchain** is the set of programs that turn source code into working software: compilers (which translate code into something the computer runs), build tools, and the software kit for the puck's chip. The bootstrap script below installs all of them for you, at the exact versions the project uses.
- **What you're building:** the puck's **firmware** (the software inside the puck), the **core library** with its developer command-line tool (`micnote-cli`), and the **app**. You don't need all three: working on the app or core alone doesn't need the firmware tools (see `--no-firmware` below).

Micnote provides cross-platform bootstrap scripts that install and verify the required toolchains against pinned versions in [`tools/versions.env`](tools/versions.env). For complete manual instructions and IDE configuration, see the [Development Setup Guide](docs/dev-setup.md).

### 1. Bootstrap developer tools

**What this does:** installs every tool Micnote needs (compilers, the build system, the puck's software kit, Flutter for the app). It first prints exactly what it will install and asks before doing anything. Anything you already have at the right version is skipped, so it's safe to run again. On a fresh machine it can take a while, because these are large downloads.

**Linux (Ubuntu 24.04+, Debian 13+, Fedora, Arch) / macOS:**
```bash
./scripts/bootstrap.sh
```
(`./` means "the file in this folder". On macOS you need [Homebrew](https://brew.sh), a popular installer for developer tools, first.)

**Windows 10/11 (PowerShell):**
```powershell
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
```
(`-ExecutionPolicy Bypass` lets Windows run this one script, which it would otherwise block because it was downloaded rather than written on your machine.)

Flags supported by both scripts (add them after the command, for example `./scripts/bootstrap.sh --no-firmware`):
- `--yes` / `-Yes`: Proceed without interactive confirmation prompts.
- `--no-firmware` / `-NoFirmware`: Skip nRF Connect SDK and embedded toolchains (useful for core/app only development).
  *Use this if you only want to work on the app or core: it skips the large set of tools for building the puck's software.*
- `--android` / `-Android`: Install Android SDK, NDK, and JDK.
  *The extra tools for building the Android app.*
- `--hardware` / `-Hardware`: Install KiCad and FreeCAD.
  *The programs for opening the circuit board and case designs.*

At the end, the script may print a few lines to add to your shell's settings (so the new tools can be found); copy them as it says.

### 2. Verify environment

**What this does:** checks that every tool is installed at a suitable version, and for anything that isn't, says what to do. It changes nothing.

Run `doctor` to check installed compilers, libraries, and tools:
```bash
# Linux / macOS
./scripts/doctor.sh

# Windows
powershell -ExecutionPolicy Bypass -File scripts\doctor.ps1
```

### 3. Initialize firmware dependencies

**What this does:** downloads the exact versions of Zephyr and Nordic's software kit that the puck's firmware is built on, into a folder called `external/`. Skip this if you're not building firmware. "Inside the NCS toolchain environment" means in a terminal set up by the bootstrap script's firmware tools (it tells you how), where the `west` command is available.

```bash
# Inside the NCS toolchain environment
west init -l firmware
west update
```

### 4. Build components

**What this does:** turns the source code of each part into something you can run, and runs the automatic tests that check it works.

- **Firmware (Thingy:53 / DK):**
  *Builds the puck's software for one of the two prototype boards. `-b` picks the board.*
  ```bash
  west build -b thingy53/nrf5340/cpuapp firmware/app
  # or
  west build -b nrf5340dk/nrf5340/cpuapp firmware/app
  ```
- **C++ core library & CLI (`libmicnote`):**
  *The three commands configure the build, build it, and run its tests. A "preset" is a named bundle of build settings; `dev` is the everyday one for development.*
  ```bash
  cmake --preset dev
  cmake --build --preset dev
  ctest --preset dev
  ```
  Other presets: `asan`, `fuzz` and `release` (`CC=clang CXX=clang++` for the first two), plus `windows-msvc`, `android-arm64` and `ios`.
  *`asan` builds with extra checks that catch memory mistakes; `fuzz` builds the fuzzers, which feed random input to the code to find crashes; `release` is the optimised build. `CC=clang CXX=clang++` in front of a command chooses the clang compiler. The last three build for Windows with Microsoft's compiler, for Android, and for iPhone.*
  CI requires 100% line, function and branch coverage of `core/`'s own code: run the `coverage` preset (gcc) and then `gcovr build/coverage` to check locally (`gcovr.cfg` has the rules).
  *CI (Continuous Integration) is the set of servers that automatically build and test every change. "Coverage" measures how much of the code the tests actually run; Micnote requires every line, every function, and both sides of every decision in the code to be tested.*
- **Speech recognition (optional):** the `asr` preset adds whisper.cpp and `micnote-cli transcribe`. `micnote-cli models list` shows the models this machine would use, where to download them, and which are already there; `micnote-cli models verify` checks downloaded files against their pinned SHA-256.
  *Turning speech into text needs a large AI model file (from about 60 MB to about 570 MB). `models list` tells you which one suits your computer and gives the command to download it; `models verify` confirms the download is exactly the expected file.*
  ```bash
  cmake --preset asr
  cmake --build --preset asr
  build/asr/core/bin/micnote-cli models list
  ```
- **Using the graphics chip (optional):** the `asr` preset uses Metal (Mac) or Vulkan (Linux, Windows) when the tools for it are installed. `--backend cpu` on `transcribe` or `run` uses the main processor instead; `-DMICNOTE_ASR_GPU=off` when configuring leaves the graphics chip out.
  *A computer's graphics chip (GPU) can run the speech model much faster than its main processor. Metal and Vulkan are the standard ways for programs to use it.*
- **From puck to transcript, with the CLI:** `micnote-cli run --usb PORT` keeps running, and each time the puck is plugged in it collects the new recordings and (in `asr` builds, with a model downloaded) turns them into text. `micnote-cli db recordings`, `db transcripts` and `db search WORDS` then show what's stored. Stop `run` with Ctrl-C.
  *`run` is a daemon: a program that stays running in the background and acts when something happens, here the puck appearing. `PORT` is the name the computer gives the puck's USB connection, such as `/dev/ttyACM0` on Linux. Everything is kept in an encrypted database in your user folder (`~/.local/share/micnote` on Linux and macOS, `%LOCALAPPDATA%\micnote` on Windows); `--data DIR` puts it somewhere else.*
  ```bash
  build/asr/core/bin/micnote-cli run --usb /dev/ttyACM0
  build/asr/core/bin/micnote-cli db transcripts
  ```
- **Drafting actions, with the CLI:** built with `-DMICNOTE_ASR=ON -DMICNOTE_LLM=ON`, `micnote-cli plan --recording ID --llm MODEL.gguf` splits a transcript into items and drafts an action for each; `db actions` shows the drafts. `run --llm MODEL.gguf` does it for every new recording. Nothing is sent: these are drafts only.
  *The language model is a file you download (a few gigabytes); [the stage 3 measurements](docs/measurements/stage3.md) compare some. To let drafts use real addresses, put people in `~/.config/micnote/contacts.csv` as `Jeff Park, jeff@example.com` lines, and your GitHub repositories in `repos.txt` there as `owner/name` lines.*
- **Drafting with Claude instead, with the CLI (optional):** for one item, if you want. Build with `-DMICNOTE_HTTPS=ON` too (it needs libcurl), then:
  *This sends text to Anthropic, so it's never automatic: you look at exactly what would be sent first, and only that is sent. Email addresses, phone numbers, anything like a password or key, and words you list are replaced with placeholders, and put back on your computer afterwards. It uses your own API key and costs a little per item.*
  ```bash
  micnote-cli db items --recording 1          # the items, with their IDs
  micnote-cli cloud key < my-key.txt          # once: your Claude API key, kept in the OS keychain
  micnote-cli cloud preview --item 3          # what would be sent, and its hash
  micnote-cli cloud draft --item 3 --hash HASH
  ```
- **Protocol schema:** after editing `proto/micnote.proto` or `proto/micnote.options`, regenerate the committed nanopb code:
  *Only needed if you change the definition of the messages between the puck and the app: this rebuilds the code generated from it.*
  ```bash
  python3 proto/regen.py   # Windows: py proto\regen.py
  ```
  Its tests (`proto/test_regen.py`) run with `python3 -m unittest discover -s proto -p "test_*.py"`; CI also requires them to cover 100% of `regen.py` (see `pyproject.toml`).
- **Flutter application:**
  *Builds the app and starts it on your computer.*
  ```bash
  cd app
  flutter run
  ```

If something goes wrong, the [Development Setup Guide](docs/dev-setup.md) has a troubleshooting section, and running `doctor` again often says what's missing.

---

## Roadmap and status

**In plain words:** Micnote is being built in stages, each finished before the next depends on it. It's still early ("pre-alpha"): not yet usable day to day. A ticked box means that stage is complete. Today, the software for the first two stages after setup (the recorder's firmware and syncing, and turning recordings into text) is largely written and tested on simulators; the rest of stage 1 waits for the prototype boards.

Micnote is currently in **pre-alpha (Phase 0)**.

- [x] **Stage 0a:** Development environment & tooling verification
  *The install and check scripts.*
- [x] **Stage 0b:** Project scaffolding & continuous integration
  *The skeleton of every part, with automatic building and testing of every change.*
- [ ] **Stage 1:** Firmware and BLE/USB sync (Thingy:53 / DK prototype)
  *A working recorder on prototype boards, syncing to a computer over Bluetooth and USB.*
- [ ] **Stage 2:** Core transcription (whisper.cpp) & encrypted store
  *Turning recordings into searchable text, stored encrypted, on your own device.*
- [ ] **Stage 3:** Local & hybrid AI planner and action registry
  *Drafting actions from the text, on your device or (if you choose) with Claude.*
- [ ] **Stage 4:** Desktop application & in-app action executors
  *The desktop app: reviewing, editing, confirming and carrying out actions.*
- [ ] **Stage 5:** Mobile applications & background sync
  *The phone apps, syncing automatically when the puck snaps on.*
- [ ] **Stage 5b:** Installers & distribution pipeline
  *Normal installers for every platform.*
- [ ] **Stage 6:** Custom puck hardware (KiCad Rev A & Rev B bring-up)
  *Micnote's own circuit board and case: a first version (Rev A), testing it, then a corrected second version (Rev B).*
- [ ] **Stage 7:** Hardening, fuzzing, and security audits
  *A full security check before version 1.0.*

Detailed progress notes and stage specifications are maintained in [`docs/PLAN.md`](docs/PLAN.md) and [`docs/stages/`](docs/stages/).

---

## Documentation map

Every document opens with a short plain-language summary, then goes into full detail.

| Document | What it covers |
|---|---|
| [How Micnote works](docs/how-it-works.md) | The whole system, start to finish, with no jargon. Start here |
| [Glossary](docs/glossary.md) | Every technical term, in plain words |
| [Master plan](docs/PLAN.md) | Every design decision, and the milestone checklist |
| [Stage plans](docs/stages/README.md) | One detailed plan per stage, with task checkboxes |
| [Development setup](docs/dev-setup.md) | Installing the tools by hand, editor setup, troubleshooting |
| [Protocol](docs/protocol.md) | Exactly how the puck and the app talk |
| [Threat model](docs/threat-model.md) | What's protected, from whom, and how |
| [Action schema](docs/action-schema.md) | What an action draft looks like |
| [Stage 2 measurements](docs/measurements/stage2.md) | How accurate and fast speech recognition is |
| [Stage 3 measurements](docs/measurements/stage3.md) | How well local language models draft actions |
| [Prototype parts](docs/hardware/prototype-parts.md) | What to buy to build the prototype |

---

## License

This project is licensed under the [MIT License](LICENSE).

*In plain words: you're free to use, copy, change and share Micnote, including commercially, as long as you keep the copyright notice. It comes with no warranty.*
