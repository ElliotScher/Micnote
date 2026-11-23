# Glossary

Every technical term the Micnote docs use, in plain words. You don't need any background to read it: where a term builds on another, that one is explained here too. If you're new to the project, read [How Micnote works](how-it-works.md) first; it tells the whole story without jargon and points back here.

Terms are in alphabetical order. *In Micnote* lines say how the project uses the idea.

**Jump to:** [A](#a) · [B](#b) · [C](#c) · [D](#d) · [E](#e) · [F](#f) · [G](#g) · [H](#h) · [I](#i) · [J](#j) · [K](#k) · [L](#l) · [M](#m) · [N](#n) · [O](#o) · [P](#p) · [Q](#q) · [R](#r) · [S](#s) · [T](#t) · [U](#u) · [V](#v) · [W](#w) · [X](#x) · [Z](#z)

---

## A

**AAB (Android App Bundle):** the file format Google Play wants an Android app uploaded in. Play turns it into the right installable package for each phone.

**ABI, C ABI:** *Application Binary Interface*: the exact rules two separately built pieces of software use to call each other (which functions exist, what goes in, what comes out, in what memory layout). A "C ABI" uses the conventions of the C language, which almost every other language can call. *In Micnote:* the app (written in Dart) talks to the core library (written in C++) through a small C ABI described in `core/include/micnote.h`. Every function in it starts with `mn_`. Requests and answers are passed as JSON text, so the list of functions can stay tiny while the requests grow.

**Accelerometer:** a tiny sensor that measures movement and shocks (the same thing that rotates your phone's screen). *In Micnote:* it notices the small knock when the puck snaps onto a phone, and a double-tap on the puck.

**Ack (acknowledge):** a message saying "I received this safely." *In Micnote:* the puck deletes a recording only after a paired phone or computer acks it, so nothing is lost if a transfer breaks.

**Action:** in Micnote, a concrete task drafted from something you said: "email Jeff about the budget" becomes a ready-to-send email draft. You review and confirm it, and the app does it for you. See [action schema](action-schema.md).

**Action schema, registry:** the precise description of every kind of action (what fields an email has, which are required, and so on). The *registry* is the list of all of them. The same descriptions drive the AI that drafts actions, the checks that validate them and the screens that edit them.

**Advertising (Bluetooth):** a Bluetooth device announcing "I'm here" so others can find and connect to it. *In Micnote:* the puck advertises only when it needs to (snapped onto a phone, button pressed, or just after a recording), so it can't easily be tracked.

**AEAD:** *Authenticated Encryption with Associated Data*. Encryption that also proves the data wasn't changed: decrypting tampered data fails instead of producing garbage. *In Micnote:* every chunk of audio is sealed this way.

**AES-256-CCM, AES-GCM:** two standard, very widely used AEAD encryption methods built on AES (the encryption standard banks and governments use). "256" is the key size in bits, which is far beyond what anyone can guess. *In Micnote:* audio chunks use AES-256-CCM because the puck's chip has hardware for it.

**API:** *Application Programming Interface*: the set of requests one program offers to others. A web service's API is how apps send email through Gmail, create GitHub issues, and so on. *In Micnote:* actions are carried out by calling the service's API directly from the app.

**AppImage:** a Linux app packaged as a single file you can download and run on almost any Linux distribution.

**APPROTECT:** a setting in Nordic chips that locks the debug port, so nobody can plug in a debugger and read the chip's memory. *In Micnote:* turned on in release builds of the firmware.

**ASR:** *Automatic Speech Recognition*: turning speech into text. See *whisper.cpp*.

**AddressSanitizer (ASan), UBSan:** tools that build the program with extra checks that catch memory mistakes and undefined behaviour while tests run. *In Micnote:* the `asan` build preset turns them on.

**Audit log:** a permanent, append-only record of what happened and when. *In Micnote:* every action the app executes is written there.

## B

**BabbleSim:** a simulator of the Bluetooth radio world, so Bluetooth firmware can be tested with no real hardware. *In Micnote:* the puck's Bluetooth code is tested in it in CI.

**Backoff:** waiting longer after each failed retry (1 minute, 2, 4, 8...), so a failing task doesn't hammer a service. *In Micnote:* the job queue retries failed jobs this way.

**Beam search, greedy decoding:** two ways a speech or language model picks its words. Greedy takes the single best next word each time (fast); beam search keeps several candidate sentences going and picks the best overall (slower, more accurate). *In Micnote:* beam search on desktops, greedy on phones.

**BLE (Bluetooth Low Energy):** the low-power kind of Bluetooth used by watches, fitness trackers and earbuds' controls. *In Micnote:* the puck syncs to phones and computers over BLE.

**Bonding:** two Bluetooth devices remembering each other after pairing, so they reconnect securely later without pairing again.

**BOM (Bill of Materials):** the shopping list of every part on a circuit board, with quantities and part numbers.

**Boot count, uptime:** the number of times a device has started, and how long it has been running since the last start. *In Micnote:* every recording stores both, so its real date can be worked out later even if the puck's clock was lost when the battery died.

**Bootstrap script:** a script that installs everything a developer needs. *In Micnote:* `scripts/bootstrap.sh` (Linux and macOS) and `scripts/bootstrap.ps1` (Windows).

**Branch (git):** a separate line of changes in a repository, so work can happen without disturbing the main version until it's ready.

**Branch coverage:** see *coverage*.

**Breakout board:** a tiny circuit board that carries one chip and brings its pins out to easy-to-wire holes, so you can try the chip on a bench before designing your own board. *In Micnote:* the puck's real microphones and sensors are tried on breakouts wired to the nRF5340-DK.

**Buck converter:** a circuit that efficiently turns a higher voltage (the battery's) into a lower one (what the chips need).

**Build, build system:** turning source code (text people write) into a program a computer runs. A build system (here *CMake* for the core and *west* for the firmware) works out what to compile and how.

## C

**CDC ACM (USB CDC):** a standard way for a USB device to appear as a simple serial port ("a pipe for bytes"), which every operating system supports without drivers. *In Micnote:* the puck's USB sync uses it.

**CEK (Content Encryption Key):** the random key that encrypts one recording. *In Micnote:* each recording gets a new CEK, which is then *wrapped* (see *key wrapping*) for each paired host.

**Challenge-response:** proving you know a secret without sending it: one side sends a random number (the challenge), the other answers with something only the secret could produce from it. *In Micnote:* this is how the puck and a paired host recognise each other at the start of every connection.

**Characteristic (GATT):** see *GATT*.

**Checksum, hash:** see *SHA-256*.

**Chunk:** a fixed-size piece of a recording. *In Micnote:* audio is stored and sent in encrypted chunks, so an interrupted transfer can resume from the last good chunk.

**CI (Continuous Integration):** servers that automatically build and test every change pushed to the repository, so mistakes are caught before they land. *In Micnote:* GitHub Actions, configured in `.github/workflows/`.

**clang, gcc, MSVC:** the three main C and C++ *compilers* (programs that turn source code into machine code). *In Micnote:* the core must build with all three.

**clang-format, clang-tidy:** a tool that lays out code consistently (format), and one that spots likely bugs and bad patterns (tidy). CI runs both.

**CLI (Command-Line Interface):** a program you use by typing commands in a terminal rather than clicking. *In Micnote:* `micnote-cli` is the developer tool that pairs, syncs and transcribes from the terminal.

**CMake, CMake preset:** the build system for the core library. A preset is a named bundle of build settings (`dev`, `release`, `asan`, ...), so one command picks them all.

**Codec:** a method for compressing audio or video. See *Opus*.

**Commit (git):** one saved set of changes in a repository's history, with a message saying what changed.

**CompanionDeviceManager:** an Android system service that lets an app say "this Bluetooth device is mine"; Android then wakes the app when the device is near. *In Micnote:* how the Android app notices the puck snapping on.

**Compiler:** a program that turns source code into a program the computer can run.

**Connector:** in Micnote, the part of the app that knows one service (email, GitHub, calendar, reminders): how to draft its actions, show its editor, check it, and carry it out.

**Context fetcher:** a read-only lookup a connector offers the planner, such as "find contacts named Jeff" or "get this pull request's changes", so drafts start out accurate. It can never change anything.

**Core Bluetooth:** Apple's Bluetooth framework on iPhone, iPad and Mac.

**Core library, `libmicnote`:** the shared engine of Micnote, written in C++: syncing with the puck, decrypting, storing, transcribing and planning. Every app (phone or desktop) uses the same core.

**Coverage (line, function, branch):** how much of the code the tests actually run. *Line* coverage counts lines, *function* coverage counts functions, and *branch* coverage counts both sides of every decision (the "if" and the "else"). *In Micnote:* CI requires 100% of all three for the core's own code.

**CPL (Component Placement List):** a file telling a board assembler where each part goes on a circuit board.

**CryptoCell-312:** the hardware encryption engine inside the nRF5340 chip.

**Cryptography:** the mathematics of keeping data secret (encryption), proving who sent it (signatures) and proving it wasn't changed (authentication).

**CycloneDX:** a standard format for an *SBOM*.

## D

**Dart, Flutter:** Dart is a programming language; Flutter is Google's toolkit for building one app that runs on iPhone, Android, Mac, Windows and Linux from the same code. *In Micnote:* the app is written in Flutter.

**`dart:ffi`, ffigen:** `dart:ffi` lets Dart code call C functions directly; *ffigen* reads a C header file and writes the matching Dart code automatically. *In Micnote:* how the app calls the core library.

**Database, SQLite:** a structured store for data that can be searched and updated safely. SQLite is a small database that lives in a single file, used inside almost every phone app. *In Micnote:* recordings, transcripts and actions are kept in an encrypted SQLite database.

**Debug port, SWD:** a set of pins on a chip that a *debugger* connects to, to load firmware and look inside the running chip. SWD (*Serial Wire Debug*) is Arm's version. See also *APPROTECT* and *J-Link*.

**Device class:** in Micnote, a rough category of machine (desktop, mobile, or mobile-small) used to choose AI models that will run well on it.

**Devicetree, Kconfig:** two configuration systems in Zephyr. *Devicetree* describes the hardware (which chip is wired to which pins); *Kconfig* switches software features on and off.

**DFU (Device Firmware Update):** installing new firmware on a device. *In Micnote:* done over Bluetooth or USB through MCUmgr (see *MCUboot*).

**DK (Development Kit):** a manufacturer's board for trying a chip, with extra buttons, LEDs and a built-in debugger. *In Micnote:* the Nordic nRF5340-DK.

**Doctor script:** a script that checks every tool a developer needs is installed at the right version, and says how to fix what isn't. *In Micnote:* `scripts/doctor.sh` and `scripts/doctor.ps1`.

**DRC, ERC:** automatic checks on a circuit board design. *ERC* (Electrical Rules Check) looks for wiring mistakes in the schematic; *DRC* (Design Rules Check) checks the physical layout against what the factory can make. CI runs both.

## E

**ECDH:** a way for two parties to agree on a shared secret over a public channel, each using their own key pair, without the secret ever being sent. See *X25519*.

**ECIES:** encrypting data to someone's public key using ECDH plus normal encryption. *In Micnote:* how a recording's key is wrapped for each paired host.

**Embedded software:** software that runs on a small dedicated chip inside a device rather than on a general-purpose computer. See *firmware*.

**Enclosure:** the physical case around a circuit board. *In Micnote:* the puck's case, 3D-printed in resin.

**Encryption, decryption:** scrambling data with a key so only someone with the right key can unscramble (decrypt) it.

**ESD (Electrostatic Discharge):** a static shock. Boards add protection parts at connectors so a shock from a finger doesn't destroy a chip.

**Event (C ABI):** a message the core sends to the app on its own, such as "job 7 is 42% done".

## F

**Fab (fabrication):** the factory that makes circuit boards. "Send to fab" means order the boards.

**Ferrite sheet:** a thin magnetic sheet placed behind a wireless-charging coil to guide the magnetic field and shield the parts behind it.

**FetchContent:** a CMake feature that downloads a dependency at a pinned version during the build. *In Micnote:* every third-party library the core uses is declared in `core/cmake/deps.cmake`.

**Fiducial:** a small marker on a circuit board that assembly machines use to line up.

**Firmware:** the software built into a device's chip. *In Micnote:* the puck's firmware, in `firmware/`, written in C on Zephyr.

**Flash memory, NOR flash, QSPI:** flash is memory that keeps its contents with the power off (like a USB stick). NOR flash is a reliable kind used for storage on small devices. QSPI (*Quad SPI*) is the fast four-wire connection between the chip and the flash. *In Micnote:* recordings are kept on a 64 MB QSPI NOR flash chip.

**Footprint (PCB):** the pattern of copper pads on a circuit board that one part solders onto.

**Framing:** marking where each message starts and ends in a continuous stream of bytes. *In Micnote:* every message is sent as its length followed by its content (see *varint*).

**FTS5 (full-text search):** SQLite's built-in search engine for text. *In Micnote:* makes transcripts searchable.

**Fuel gauge:** a circuit that estimates how much charge is left in a battery.

**Fuzzing, libFuzzer:** testing by feeding a program huge numbers of random and malformed inputs to find crashes. libFuzzer is a popular fuzzer. *In Micnote:* every piece of code that reads data from outside (radio messages, recording files) is fuzzed.

## G

**GATT, characteristic:** GATT is how Bluetooth Low Energy devices expose data: a device offers *services*, each with *characteristics* (named values another device can read, write or subscribe to). *In Micnote:* the puck's control messages travel over a GATT characteristic.

**GBNF grammar, structured outputs:** ways to force an AI model to produce text in an exact format (here, JSON matching an action schema) instead of free prose. GBNF is llama.cpp's grammar format; *structured outputs* is Claude's equivalent.

**Gerbers:** the standard files that describe each layer of a circuit board for the factory.

**git, GitHub, repository (repo):** git records every version of a project's files (the *repository*); GitHub hosts git repositories online, with issues, pull requests and CI.

**GitHub issue, pull request (PR), PR review:** an *issue* is a tracked task or bug report on GitHub. A *pull request* proposes a set of changes to a repository; a *review* is someone's comments and verdict on it (approve, comment, request changes). *In Micnote:* the app can create issues and write PR reviews for you.

**GoogleTest:** a C++ testing framework. *In Micnote:* the core's unit tests use it.

**GPU backends (Metal, Vulkan, CUDA, NEON, Core ML):** ways to run AI models on a computer's graphics chip or special hardware, which is much faster than the main processor alone. Metal and Core ML are Apple's; Vulkan is cross-platform; CUDA is NVIDIA's; NEON is the vector instructions in phone processors.

## H

**Hall sensor (magnetometer):** a sensor that measures magnetic fields. *In Micnote:* it notices the magnetic ring of a phone (MagSafe or Pixelsnap) when the puck snaps on.

**Haptics, LRA, DRV2605L:** haptics is feedback you feel (vibration). An LRA (*Linear Resonant Actuator*) is the small vibration motor phones use; the DRV2605L is the chip that drives it. *In Micnote:* short buzzes confirm starting, stopping and marking.

**Hash, SHA-256:** a hash is a short fingerprint of some data: change one bit of the data and the fingerprint changes completely, and nobody can make two different files with the same one. SHA-256 is the standard hash. *In Micnote:* used to check recordings, AI model downloads and releases weren't corrupted or tampered with.

**HKDF:** a standard method for turning one secret into several separate keys for different purposes.

**HMAC:** a hash combined with a secret key: only someone with the key can produce or check it. *In Micnote:* used in the challenge-response that starts each connection.

**Host:** in Micnote, any phone, tablet or computer the puck syncs with.

## I

**I²C:** a simple two-wire connection used to talk to small chips (sensors, power chips) on a board.

**Idempotent, idempotency key:** an operation is idempotent if doing it twice has the same effect as doing it once. An *idempotency key* is a unique label sent with a request so the service can recognise a retry. *In Micnote:* if the network fails mid-send, retrying can't send the email twice.

**Item:** in Micnote, one thing to do or note, found in a transcript ("next todo"). Each item gets one action draft.

## J

**J-Link:** a popular debugger (made by SEGGER) that loads firmware onto chips and lets you inspect them while they run. The nRF5340-DK has one built in.

**Job queue:** a list of long-running tasks (sync, transcribe, plan) worked through in the background, remembered across restarts and retried when they fail.

**JSON:** a simple, widely used text format for structured data, like `{"name": "Jeff", "age": 40}`.

**JSON Schema:** a standard way to describe what valid JSON looks like (which fields, what types). *In Micnote:* action schemas are written in it.

## K

**Key (encryption):** the secret number that locks and unlocks encrypted data. A *key pair* has a *private key* (kept secret) and a *public key* (safe to share): data encrypted to the public key can only be decrypted with the private one.

**Key wrapping:** encrypting a key with another key. *In Micnote:* each recording's key is wrapped separately for every paired host, so any of them (and nobody else) can open it.

**Keychain, secure storage:** the operating system's protected vault for passwords and keys (Keychain on Apple devices, Credential Manager on Windows, the Secret Service on Linux, Keystore on Android). *In Micnote:* the database key and the host's private key are kept there.

**KiCad, FreeCAD:** free programs for designing circuit boards (KiCad) and 3D parts like enclosures (FreeCAD).

**KMU (Key Management Unit):** a protected area of the nRF5340 chip for storing keys so firmware can use them but not read them out.

## L

**L2CAP CoC:** a Bluetooth Low Energy feature (*Connection-oriented Channel*) for streaming larger amounts of data efficiently. *In Micnote:* recordings are transferred over it, falling back to GATT if a phone doesn't support it.

**LED, RGB LED, light pipe:** a small light. An RGB LED can show any colour. A light pipe is a clear plastic piece that carries the light to the outside of the case.

**LESC (LE Secure Connections):** the modern, secure way Bluetooth Low Energy devices pair.

**LiPo, NTC:** a LiPo is a lithium-polymer rechargeable battery (the flat kind in phones). An NTC is a temperature sensor fitted to the battery so charging stops if it gets too hot.

**LittleFS:** a small file system designed to survive power cuts on flash memory. *In Micnote:* the puck stores recordings as files on it.

**llama.cpp, LLM:** an LLM (*Large Language Model*) is an AI that reads and writes text, like ChatGPT or Claude. llama.cpp is free software that runs such models on your own computer or phone. *In Micnote:* it drafts actions locally, with nothing sent anywhere.

**Load switch:** a chip that turns power to one part of a board on and off.

**LTS (Long-Term Support):** a release of software that gets security fixes for a long time.

## M

**MagSafe, Qi2, Pixelsnap:** magnetic rings in the backs of phones (Apple's MagSafe, the Qi2 standard, Google's Pixelsnap) that hold chargers and accessories in place. *In Micnote:* the puck has a matching magnet ring and snaps onto the phone.

**MCUboot, secure boot, signed image:** MCUboot is a small program that runs first when the chip starts and checks the main firmware's digital signature before running it (*secure boot*). A *signed image* is firmware carrying that signature. So only firmware signed with Micnote's key can run on the puck.

**MCUmgr, SMP:** a standard way (and its message protocol, SMP) to manage small devices: upload new firmware, reset, and so on. *In Micnote:* firmware updates over Bluetooth or USB.

**Migration (database):** a numbered step that changes a database's structure when a new version of the app needs it, run automatically and only once.

**Model (AI), quantization:** a model is the trained data an AI program uses (a big file). *Quantization* stores it with less precision (for example "q5" = 5 bits per number), making it several times smaller and faster with little loss of accuracy. *In Micnote:* models are downloaded on first run and checked against pinned hashes (see *model registry*).

**Model registry, manifest:** in Micnote, the built-in list of AI models (`models/manifest.json`): where each is downloaded from, its size, its SHA-256 fingerprint, which devices it suits, and which one each kind of device uses by default.

**Mono, stereo:** one audio channel, or two (left and right).

## N

**nanopb, protobuf:** protobuf (*Protocol Buffers*) is Google's compact format for structured messages, described in a `.proto` file. nanopb is a version small enough for microcontrollers. *In Micnote:* every message between puck and host is defined in `proto/micnote.proto`.

**native_sim, QEMU:** ways to run firmware without the real chip. `native_sim` builds the Zephyr firmware as an ordinary program on your computer; QEMU emulates an Arm processor. *In Micnote:* most firmware tests run this way in CI.

**NCS (nRF Connect SDK), SDK:** an SDK (*Software Development Kit*) is a bundle of code and tools for building software for a platform. NCS is Nordic's, built around Zephyr.

**Nonce:** a number used only once, usually random. Including fresh nonces stops an attacker replaying an old recorded message.

**Notarization:** Apple checking a Mac app for malware and stamping it, so macOS will open it without warnings.

**nPM1300, nPM1100, PMIC:** a PMIC (*Power Management IC*) is a chip that charges the battery and supplies the right voltages to everything else. The nPM1300 and nPM1100 are Nordic's.

**nRF5340:** the main chip in the puck, made by Nordic Semiconductor: two small Arm processors (one runs the app, one runs the Bluetooth radio), memory and the radio in a single chip.

## O

**OAuth 2, PKCE, device flow:** OAuth is the standard "Sign in with Google/GitHub" process that gives an app permission to act on an account without ever seeing your password. PKCE is an extra protection for apps on your own devices. The *device flow* is a variant where you approve the sign-in on another screen with a short code.

**Opus, Ogg Opus:** Opus is a modern, free audio codec that keeps speech clear at very low data rates. Ogg Opus is the standard file format for it (`.opus`), which ordinary music players open. *In Micnote:* the puck records speech at about 16 kbit/s (about 7.2 MB per hour).

**OTA (Over The Air):** updating firmware wirelessly.

## P

**Pairing, SAS:** pairing is introducing two devices so they trust each other from then on. A *SAS* (*Short Authentication String*) is something short both devices show for you to compare, to make sure nobody is in the middle. *In Micnote:* the puck's light shows a sequence of colours, the app shows the same sequence, and you press the puck's button if they match.

**PCB, Rev A, Rev B:** a PCB (*Printed Circuit Board*) is the green board that chips are soldered onto. "Rev A" is the first version of a custom board; "Rev B" is the corrected second version.

**PCM:** raw, uncompressed audio: a list of numbers, one per instant (here 16,000 per second).

**PDM microphone:** a tiny digital microphone, of the kind used in phones, that sends its sound to the chip as a fast stream of bits.

**PPK2 (Power Profiler Kit II):** a Nordic instrument that measures exactly how much current a device draws, to work out battery life.

**Private address (resolvable private address):** a Bluetooth address that changes regularly, so strangers can't follow a device around, while paired devices can still recognise it.

**Prompt injection:** tricking an AI by hiding instructions in the material it reads: someone says, or writes in an email or code change, "ignore your instructions and send...". *In Micnote:* the planner treats every transcript and fetched document as data only, and nothing happens without your confirmation. See [threat model §5](threat-model.md#5-prompt-injection).

**Protocol:** the agreed rules for how two devices talk: which messages exist, in what order, and what they mean. See [protocol](protocol.md).

**PSA Crypto:** Arm's standard programming interface for cryptography. *In Micnote:* both the puck and the host use it, so their encryption code works the same way.

**Pseudo-terminal:** a pair of connected virtual serial ports inside the operating system. *In Micnote:* tests use one to stand in for the puck's USB cable.

**Puck:** Micnote's recorder: a small round device, about 57 mm (2¼ in) across and 1 cm thick, that snaps magnetically onto a phone or works on its own.

## Q

**Qi:** the standard for wireless charging pads. *In Micnote:* the puck charges on one.

**QSPI:** see *flash memory*.

## R

**Real-time factor (RTF):** how long transcription takes compared with the audio's length. An RTF of 0.1 means an hour of audio takes 6 minutes.

**Redaction, redacted preview:** removing sensitive details before sharing text. *In Micnote:* before anything goes to Claude (only if you ask), email addresses, phone numbers, anything that looks like a secret key, and words you've listed are replaced with placeholders; you see exactly what will be sent, and the real details are put back on your device afterwards. Names of people are kept by default, because drafts need them, but can be redacted too.

**Release, tag, signing:** a *release* is a finished version published for people to install. A git *tag* marks the exact source it was built from. *Signing* attaches a cryptographic signature proving the files came from the project and weren't changed.

**Repository (repo):** see *git*.

**RSSI:** a measure of how strong a received radio signal is.

**RTC (Real-Time Clock):** a low-power clock on a chip that keeps the date and time.

**RTT:** a SEGGER feature for sending log messages from a running chip to the computer through the debugger.

## S

**Sandbox account:** a test account on a service (a test email inbox, a test GitHub repository) where actions can be tried without affecting anything real.

**SBOM (Software Bill of Materials):** a list of every third-party component inside a piece of software, so security problems in any of them can be tracked.

**Schema:** a precise description of the structure of some data. See *JSON Schema* and *nanopb*.

**SDK:** see *NCS*.

**Signing key:** the private key used to sign firmware or releases. Whoever holds it can produce "official" software, so it's guarded closely.

**SimpleBLE:** a C++ library for using Bluetooth Low Energy on desktop operating systems.

**SLA printing:** a 3D-printing method that cures liquid resin with light, giving smooth, precise parts.

**SoC (System on Chip):** a single chip containing a whole small computer: processors, memory and radio. The nRF5340 is one.

**SQLCipher, SQLite3MultipleCiphers:** encrypted versions of SQLite: the database file is unreadable without its key.

**Stage:** in Micnote's plan, one milestone of the project (0a through 7). See [the stage plans](stages/README.md).

**STRIDE:** a checklist for thinking about security threats: **S**poofing (pretending to be someone), **T**ampering (changing data), **R**epudiation (denying you did something), **I**nformation disclosure (leaks), **D**enial of service (stopping it working), **E**levation of privilege (gaining powers you shouldn't have).

**SWD:** see *debug port*.

**Sync:** copying recordings from the puck to a phone or computer.

**Sysbuild:** Zephyr's way of building several firmware images together (here the app, the Bluetooth radio's firmware, and MCUboot).

## T

**Telemetry:** usage data an app quietly sends back to its makers. *In Micnote:* there is none.

**Test point:** a small exposed pad on a circuit board where a probe can measure a signal.

**Test vectors:** sample inputs with their known correct outputs. *In Micnote:* recordings the puck's code wrote are checked in, and the host's code must decrypt them exactly (`proto/testvectors/`).

**Thingy:53:** a small Nordic gadget with the nRF5340, a microphone, sensors, a battery and a light. *In Micnote:* the first prototype of the puck.

**Threat model:** a document listing what needs protecting, who might attack it, and how each attack is stopped. See [threat model](threat-model.md).

**Token (OAuth):** the permission an app gets after you sign in with OAuth: like a valet key for one account. *Token (AI)* means something else: a word or word-piece an AI model reads or writes.

**Toolchain:** the set of programs (compiler, linker, build tools) needed to build software for a particular platform.

**Transcript, segment:** the text of a recording. A *segment* is one stretch of it, with its start and end time.

**Transport:** the channel messages travel over: here Bluetooth or USB.

**twister:** Zephyr's tool for running many firmware tests across different boards and simulators.

## U

**UART:** a simple two-wire serial connection, often used for log messages from a chip.

**udev rule:** a Linux configuration file that gives normal users permission to use a particular USB device.

**Unit test, integration test:** a *unit test* checks one small piece of code on its own; an *integration test* checks several pieces working together.

**USB-C, VID/PID:** USB-C is the reversible USB connector. Every USB device reports a *VID* (vendor ID) and *PID* (product ID) so computers know what it is.

## V

**VAD (Voice Activity Detection):** working out which parts of a recording contain speech. *In Micnote:* silence is skipped before transcription, which is faster and stops the AI inventing words in quiet stretches. Uses the Silero VAD model through whisper.cpp.

**Varint:** a compact way of writing whole numbers in which small numbers take fewer bytes.

**Verify (in the plans):** text marked *verify* is a fact still to be checked against a datasheet or vendor documentation.

**VM (virtual machine):** a whole simulated computer running inside another, used to test installers on a clean system.

## W

**WAL (Write-Ahead Logging):** a SQLite mode where changes are written to a log first, so the database survives crashes and can be read while it's being written.

**WER (Word Error Rate):** the share of words a transcription gets wrong compared with a correct reference. Lower is better.

**west:** the command-line tool for Zephyr projects: it fetches the right versions of Zephyr and Nordic's code, builds firmware and flashes it.

**whisper.cpp:** free software that runs OpenAI's Whisper speech-recognition models on your own computer or phone, with no internet needed. *In Micnote:* it turns recordings into text.

## X

**X25519:** a modern, fast and widely trusted method for key agreement (see *ECDH*). *In Micnote:* each host has an X25519 key pair; recordings are wrapped to the hosts' public keys.

## Z

**Zephyr:** a free, open-source operating system for small chips, backed by the Linux Foundation. *In Micnote:* the puck's firmware runs on it.

**Zeroization:** overwriting a secret in memory with zeros as soon as it's no longer needed, so it can't be recovered later.
