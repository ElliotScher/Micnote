# Stage 5b: installers and releases

## 1. Goal
End users install Micnote with a normal, signed, native package on every platform. Each package includes:
- `libmicnote`, linked in;
- the matching puck firmware image;
- a first-run download of the AI models, with each file's hash checked;
- an update channel.

Nobody runs scripts or installs toolchains. Releases are built only in CI from a tag, and they ship with checksums, signatures and an SBOM.

**Exit criterion (from PLAN.md):** clean VMs (Ubuntu, Fedora, AppImage on any distro, macOS, Windows) and fresh phones install the app, pair, sync and execute an action, with no dev tools present.

## 2. Depends on / unblocks
- **Depends on:** 4 (desktop app), 5 (mobile apps), 1/6b (firmware images to bundle).
- **Unblocks:** public use, and stage 7's supply-chain review.

## 3. Deliverables
```
app/distribute_options.yaml           # fastforge
app/linux/packaging/{deb,rpm,appimage}/make_config.yaml
app/linux/packaging/70-micnote.rules  # udev
app/linux/packaging/dev.micnote.Micnote.desktop, icons/, metainfo.xml (AppStream)
app/macos/packaging/dmg/make_config.yaml, appcast/
app/windows/packaging/exe/make_config.yaml (Inno Setup), msix/ (later)
app/assets/firmware/                   # populated by CI: micnote_puck-<ver>.zip, thingy53-<ver>.zip (dev builds only)
models/manifest.json                   # pinned model URLs + sha256 (shipped inside app)
.github/workflows/release.yml
.github/workflows/nightly.yml          # unsigned dev builds
packaging/winget/, packaging/homebrew/ # manifest templates
docs/releasing.md                      # human checklist, key custody
```

## 4. Design detail

### 4.1 Versioning
- **SemVer** `MAJOR.MINOR.PATCH`. The git tag `vX.Y.Z` is the single source of truth.
- `app/pubspec.yaml` `version: X.Y.Z+BUILD`, where BUILD = the CI run number (store builds need integer build numbers that only go up).
- **Firmware version:** `firmware/VERSION`, kept in lockstep (the same X.Y.Z).
- **Compatibility:** the app reads the firmware's `proto_version` from `HelloAck`. If the puck firmware is older than the bundled image, the app offers an update. If the protocol is incompatible, sync is blocked until the puck is updated.

### 4.2 Per-platform packaging
| Platform | Tool | Output | Signing | Update |
|---|---|---|---|---|
| macOS | `flutter build macos` + fastforge `dmg` | `Micnote-X.Y.Z-universal.dmg` | `codesign --options runtime --timestamp` with the Developer ID Application cert, then `xcrun notarytool submit --wait`, then `xcrun stapler staple` | Sparkle 2 (`auto_updater` plugin), EdDSA-signed `appcast.xml` on GitHub Pages |
| Windows | `flutter build windows` + fastforge `exe` (Inno Setup) | `Micnote-X.Y.Z-x64-setup.exe` (per-user, no admin) | Azure Trusted Signing via `signtool` + `Azure.CodeSigning.Dlib` for the app exe, the DLLs and the installer | WinSparkle (`auto_updater`), the same appcast with Windows items |
| Linux deb | fastforge `deb` | `micnote_X.Y.Z_amd64.deb`, `_arm64.deb` | `dpkg-sig`, or a signed apt repo (`Release.gpg`) | apt repo |
| Linux rpm | fastforge `rpm` | `micnote-X.Y.Z-1.x86_64.rpm`, `.aarch64.rpm` | `rpm --addsign` (GPG) | dnf repo (`repo_gpgcheck=1`) |
| Linux AppImage | fastforge `appimage` (appimagetool) | `Micnote-X.Y.Z-x86_64.AppImage` | GPG signature embedded (`--sign`) | Embedded zsync info `gh-releases-zsync|…|latest|Micnote-*x86_64.AppImage.zsync`; in-app "Update available" via AppImageUpdate |
| iOS / iPadOS | `flutter build ipa` | IPA → App Store Connect | Distribution cert + provisioning profile (fastlane `match`, kept in a private repo) | App Store / TestFlight |
| Android | `flutter build appbundle` + `flutter build apk --split-per-abi` | AAB → Play; APKs → GitHub Releases | Play App Signing (upload key); APK signed with our release key (`apksigner`, v2+v3) | Play; Obtainium watches GitHub Releases |

*Verify* the fastforge maker names and config keys against its current docs. If any maker doesn't fit, call the underlying tool (appimagetool, Inno Setup `iscc`, `rpmbuild`) directly from CI.

**Linux package dependencies:**
| | deb `Depends` | rpm `Requires` |
|---|---|---|
| GTK runtime | `libgtk-3-0` | `gtk3` |
| Secret storage | `libsecret-1-0` | `libsecret` |
| BLE | `bluez` | `bluez` |
| Recommended | `libvulkan1` (`Recommends:`) | `vulkan-loader` (`Recommends:`) |

**`70-micnote.rules`** (installed to `/usr/lib/udev/rules.d/`; deb `postinst` and rpm `%post` run `udevadm control --reload && udevadm trigger`):
```
# Micnote puck (USB CDC + SMP) – grant the logged-in user access
SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="XXXX", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="XXXX", TAG+="uaccess", SYMLINK+="micnote%n"
```
`1209` is the pid.codes VID; `XXXX` is the PID we're assigned.

**AppImage:** no root at install time. The first time USB sync is used, the app offers "Install USB access rule (requires your password)" via `pkexec install -m644 … /etc/udev/rules.d/70-micnote.rules`. BLE works without it (BlueZ D-Bus, no root).

**Flatpak (later):** `org.freedesktop.Platform` runtime, `--system-talk-name=org.bluez`, `--device=all` for USB (or a portal once one exists), `--talk-name=org.freedesktop.secrets`. Submitted to Flathub.

### 4.3 Bundled firmware
- CI builds the firmware for each board (`micnote_puck`, plus `thingy53` in dev builds) and copies `dfu_application.zip` with `manifest.json` into `app/assets/firmware/`.
- The app ships with them. Updating the puck uses MCUmgr: SMP over BLE on mobile and desktop, or over USB on desktop.
- MCUboot checks the image signature (the production key from stage 7), so a tampered app package can't install arbitrary puck firmware unless it was also signed with the firmware key, which only CI holds.

### 4.4 Models
**`models/manifest.json`** (inside the app):
```json
{"version":3,"models":[
 {"id":"whisper-base.en-q5_1","kind":"asr","url":"https://huggingface.co/<org>/<repo>/resolve/<commit>/ggml-base.en-q5_1.bin",
  "sha256":"…","bytes":59700000,"min_ram_mb":1024,"classes":["mobile-small","mobile","desktop"]},
 {"id":"whisper-large-v3-turbo-q5_0","kind":"asr","url":"…/resolve/<commit>/…","sha256":"…","bytes":574000000,"min_ram_mb":2048,"classes":["desktop"]},
 {"id":"llm-<chosen>-q4_k_m","kind":"llm","url":"…","sha256":"…","bytes":4900000000,"min_ram_mb":6144,"classes":["desktop"]}
]}
```
- **URLs:** they pin a Hugging Face commit (`resolve/<commit>`), never `main`.
- **Download:** resumable HTTP range requests; SHA-256 verified before use; stored in the app support directory. A "Use a local file" option is also verified against the manifest hash, or the user explicitly accepts an unverified custom model (shown with a warning badge).
- **First-run screen:** shows the sizes, recommends models for the device class, and offers "download on Wi-Fi only".
- **Model licenses:** each model's license is listed in About → Licenses. Only models whose licenses allow redistribution or download for this purpose are chosen (*verify per model*).

### 4.5 Release pipeline (`release.yml`, on `v*` tags)
```
jobs:
  firmware   (ubuntu, NCS container): build puck + thingy53 → sign with prod key (stage 7 secret) → artifact
  core-tests (matrix 3 OS): ctest (gate)
  linux      (ubuntu-22.04 for older glibc baseline; x64 + arm64 runners): flutter build linux → deb, rpm, AppImage → GPG sign
  macos      (macos-latest): universal build → codesign → notarize → staple → dmg ; ios: build ipa → upload TestFlight (fastlane)
  windows    (windows-latest): build → sign exe/dlls → Inno Setup → sign installer
  android    (ubuntu): appbundle + split APKs → sign → upload Play internal track (fastlane supply)
  publish    (needs all): SHA256SUMS, SHA256SUMS.asc, CycloneDX SBOM (app + core + firmware via syft/cdxgen),
             GitHub Release (draft), update appcast.xml + apt/dnf repo metadata on gh-pages, winget/homebrew PRs (manual approval step)
```
- **Build environment:** Linux builds use Ubuntu 22.04 runners so the binaries work on older glibc (*verify* the oldest Flutter-supported baseline).
- **Secrets:** stored as GitHub Actions environment secrets in the `release` environment, which requires manual approval. These include Apple certs and the notary API key, the Azure signing identity, the GPG key, the Android upload and release keystores, and the firmware signing key.
- **Provenance:** GitHub artifact attestations (SLSA build provenance) on every artifact.
- **Publishing:** releases start as **drafts** and are published by hand after the clean-VM checklist (§6).

### 4.6 Nightly builds
`nightly.yml` builds unsigned Linux, Windows and Android packages from `main` with version `X.Y.Z-nightly.YYYYMMDD`. They are uploaded as workflow artifacts only (no public release).

## 5. Task breakdown
**PR 1: accounts and keys** (mostly manual; see `docs/releasing.md`)
- [ ] S: apply for a pid.codes VID/PID; update firmware + the udev rule
- [ ] M: Apple Developer account, certs, App Store Connect app; Google Play console app; Azure Trusted Signing; GPG release key (offline primary, CI subkey)
- [ ] S: `docs/releasing.md` with key custody and the rotation procedure

**PR 2: desktop packaging**
- [ ] M: fastforge config + Linux deb/rpm/AppImage + desktop file + AppStream metadata + udev rule
- [ ] M: macOS dmg, signing, notarization, Sparkle
- [ ] M: Windows Inno Setup, signing, WinSparkle

**PR 3: mobile packaging**
- [ ] M: fastlane for iOS (match, pilot) and Android (supply); store listings; privacy labels ("Data Not Collected"); Play data-safety form

**PR 4: models and firmware assets**
- [ ] M: manifest, downloader with resume + hash check, first-run UI
- [ ] S: firmware asset bundling + the in-app update flow wired to the bundled image

**PR 5: pipeline**
- [ ] L: `release.yml` + `nightly.yml` + SBOM + attestations + appcast/repo publishing
- [ ] S: winget and Homebrew cask manifest templates + submission steps

## 6. Testing and verification
**Release checklist** (in `docs/releasing.md`, run on each draft release):
- [ ] Ubuntu 24.04 VM: `sudo apt install ./micnote_*.deb` → launch → udev rule present → pair over BLE and USB → sync → execute an action
- [ ] Fedora VM: `sudo dnf install ./micnote-*.rpm` → same
- [ ] Arch (or any distro) VM: AppImage `chmod +x` → run → install the rule via `pkexec` → same
- [ ] macOS (clean user): open the dmg → drag to Applications → Gatekeeper passes without warnings (`spctl -a -vv`) → same flow → Sparkle detects a staged newer test version
- [ ] Windows 11 VM: run the installer (no admin prompt) → SmartScreen shows the publisher name → same flow → WinSparkle update test
- [ ] iPhone via TestFlight: install → pair (snapped on) → background auto-sync → action
- [ ] Pixel via Play internal track: same; also sideload the GitHub APK on a second device
- [ ] `sha256sum -c SHA256SUMS`; `gpg --verify SHA256SUMS.asc`; `gh attestation verify`
- [ ] Model download: interrupt and resume; a corrupted file is rejected
- [ ] Firmware update from the bundled image succeeds; tampered firmware (wrong signature) is refused by MCUboot

**Automated:** the packaging jobs install the built packages in clean containers (deb/rpm) and run `micnote --version` headless (a `--self-test` flag that loads core, runs `mn_version`, and opens and closes a temporary DB).

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Notarization or Gatekeeper failures (missing hardened-runtime entitlements for the bundled dylibs) | Sign every nested binary; run `codesign --verify --deep --strict` in CI |
| SmartScreen reputation for a new publisher | Azure Trusted Signing builds reputation over time; document the "More info → Run anyway" step during early releases |
| Google OAuth sensitive-scope verification | Start early (see stage 4); the IMAP/SMTP path works meanwhile |
| App Store review for background BLE | Clear usage strings; a demo video of snap-to-sync; explain the companion-device purpose in the review notes |
| glibc baseline too new for some distros | Build on the oldest supported Ubuntu runner; the AppImage covers the rest |

## 8. Open questions
- MSIX + Microsoft Store: after v1, depending on demand.
- Self-host the apt/dnf repos on GitHub Pages, or use a service (e.g. Cloudsmith OSS)? GitHub Pages first.
- F-Droid: needs reproducible builds and no proprietary dependencies. Check the whisper.cpp/llama.cpp builds and any Google Play Services dependencies (avoid them).

## 9. Out of scope
- A web app: Web Bluetooth doesn't support L2CAP or background use, so it isn't a target.
- Enterprise MDM packaging.
