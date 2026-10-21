# Stage 0a: dev environment

## 1. Goal
Any contributor on Linux, macOS or Windows can go from a fresh machine to a working Micnote dev setup by running one script. You also get a personal NixOS shell, kept in NixHub, that provides the same tool versions. The repo itself contains no Nix files.

**Exit criteria (from PLAN.md):**
- `scripts/doctor` passes on Linux, macOS and Windows in CI, and inside the NixHub shell.
- Secret files and build output are ignored by git.

## 2. Depends on / unblocks
- **Depends on:** nothing.
- **Unblocks:** everything else. 0b needs the toolchains to scaffold and build.

## 3. Deliverables
| Path | Purpose |
|---|---|
| `tools/versions.env` | Single source of truth for pinned tool versions |
| `scripts/bootstrap.sh` | Linux (apt/dnf/pacman) and macOS (Homebrew) installer for dev tools |
| `scripts/Brewfile` | Homebrew bundle used by `bootstrap.sh` on macOS |
| `scripts/bootstrap.ps1` | Windows installer for dev tools (winget) |
| `scripts/doctor.sh`, `scripts/doctor.ps1` | Check installed tools against `versions.env` |
| `scripts/lib/common.sh` | Shared shell helpers (logging, version compare, confirm prompt) |
| `docs/dev-setup.md` | Manual install steps, IDE setup, troubleshooting |
| `README.md` | Rewritten as described in PLAN.md §6 |
| `.gitignore` | Extended as described in PLAN.md §6 |
| `~/Documents/NixHub/dev/Personal/Micnote/flake.nix` (+ `flake.lock`) | Your personal Nix dev shell (outside the repo) |
| `~/Documents/NixHub/hosts/atta/configuration.nix` | Add `pkgs.nrf-udev` to `services.udev.packages` |

## 4. Design detail

### 4.1 `tools/versions.env`
This is a plain `KEY=value` file. bash can `source` it, and PowerShell parses it with a small regex reader. Comments say *why* each pin exists; the pinned values live only in the file itself.

### 4.2 `bootstrap.sh` behavior
```
bootstrap.sh [--yes] [--android] [--hardware] [--no-firmware] [--dry-run]
```
1. Source `scripts/lib/common.sh` and `tools/versions.env`.
2. Detect the platform: `uname -s`; on Linux, read `ID`/`ID_LIKE` from `/etc/os-release` and map it to `apt`, `dnf` or `pacman`. Anything else exits with a link to `docs/dev-setup.md`.
3. Build an install plan (a list of packages and actions), print it, and ask for confirmation unless `--yes` is given. `--dry-run` prints the plan and exits.
4. **Base packages:**
   | Need | apt | dnf | pacman | brew |
   |---|---|---|---|---|
   | compiler | `clang-18 lld-18 libc++-18-dev` or `g++-14` | `clang lld gcc-c++` | `clang lld gcc` | Xcode CLT (`xcode-select --install`) + `llvm` |
   | build | `cmake ninja-build pkg-config ccache git` | same names | `cmake ninja pkgconf ccache git` | `cmake ninja pkg-config ccache` |
   | BLE (SimpleBLE) | `libdbus-1-dev` | `dbus-devel` | `dbus` | - |
   | Flutter Linux desktop | `libgtk-3-dev libsecret-1-dev liblzma-dev` | `gtk3-devel libsecret-devel xz-devel` | `gtk3 libsecret xz` | - |
   | Vulkan (optional GPU builds) | `libvulkan-dev glslc` | `vulkan-loader-devel glslc` | `vulkan-icd-loader shaderc` | - (Metal is used on macOS) |
   | python (build tooling) | `python3 python3-venv` | `python3` | `python` | `python@3.12` |

   Package names marked here are best-effort; *verify* them on each distro's CI container.
   If the distro's clang is older than `CLANG_MIN`, apt systems add apt.llvm.org. Other distros fail with a clear message.
5. **Firmware** (skipped with `--no-firmware`):
   - Download `nrfutil` for the OS and architecture into `~/.local/bin`.
   - `nrfutil install toolchain-manager device`.
   - `nrfutil toolchain-manager install --ncs-version $NCS_VERSION`.
   - Print how to enter the toolchain environment (`nrfutil toolchain-manager launch --ncs-version … --shell`) or how to use it from VS Code.
   - SEGGER J-Link: `brew install --cask segger-jlink` on macOS. On Linux, print the download URL and wait for the user to confirm, because SEGGER's license forbids redistribution.
   - **Linux only:**
     - Install Nordic's udev rules (`nrf-udev` .deb or a rules file in `/etc/udev/rules.d/71-nrf.rules`) and reload with `udevadm control --reload`.
     - Run `usermod -aG dialout,plugdev "$USER"`, after a confirmation prompt, and print "log out and back in".
6. **Flutter:** `git clone --depth 1 -b $FLUTTER_VERSION https://github.com/flutter/flutter ~/.local/share/flutter`, or check out that tag if the clone already exists. Print the line to add to the PATH (the script never edits shell rc files without asking). On macOS, also print `sudo gem install cocoapods` or `brew install cocoapods` for iOS.
7. **`--android`:**
   - Download Android cmdline-tools to `~/Android/Sdk` and accept the licenses (prompting the user).
   - `sdkmanager "platform-tools" "platforms;android-$ANDROID_PLATFORM" "build-tools;…" "ndk;$ANDROID_NDK_VERSION" "cmake;3.22.1"`.
8. **`--hardware`:** install KiCad ≥ `KICAD_MIN` and FreeCAD (apt PPA, dnf, pacman or brew cask).
9. Finish by running `scripts/doctor.sh`.

**Idempotency:** every step checks first (`command -v`, version compare, existing directory) and prints "already satisfied".

### 4.3 `bootstrap.ps1`
```
bootstrap.ps1 [-Yes] [-Android] [-Hardware] [-NoFirmware] [-DryRun]
```
- Requires winget and must not run elevated, except for the VS Build Tools step, which elevates itself.
- **winget IDs** (*verify*): `Git.Git`, `Kitware.CMake`, `Ninja-build.Ninja`, `LLVM.LLVM`, `Python.Python.3.12`, `Microsoft.VisualStudio.2022.BuildTools` (with `--override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"`), `SEGGER.JLink`, `KiCad.KiCad`, `FreeCAD.FreeCAD`, `Google.AndroidStudio`.
- nrfutil is downloaded straight from Nordic, then the same `toolchain-manager` steps run as on Linux and macOS.
- Flutter is installed by cloning or downloading the zip into `%LOCALAPPDATA%\flutter`, and the user `PATH` is updated after a prompt.

### 4.4 `doctor`
It prints a table of tool, required version, found version and status (OK / MISSING / OLD), and exits non-zero if anything required is missing.

**Checks:**
- `cmake`, `ninja`, the C++ compiler, and a C++23 compile probe:
  ```sh
  printf '#include <expected>\n#include <print>\nint main(){std::expected<int,int> e{1}; std::println("{}", *e);}' \
    | $CXX -std=c++23 -x c++ - -o /dev/null
  ```
  If the probe fails, doctor reports which header is missing, for example "libstdc++ too old for `<print>`".
- `python3` ≥ min, `west` (inside the NCS toolchain env or on PATH), `nrfutil`, the `toolchain-manager` NCS version, `JLinkExe`.
- `flutter --version` matches `FLUTTER_VERSION`. `flutter doctor` is summarized for the host desktop target.
- **Optional checks** (reported but don't fail the run): Android SDK + NDK, KiCad, Vulkan, Xcode.
- On Linux, it also checks that the udev rule file exists and that the user is in `dialout`.

### 4.5 NixHub flake
**Built** (NixHub `993db58`). Where it differs from the outline below: it uses `segger-jlink-headless` (libjlinkarm + udev rules, enough for nrfutil and pyOCD to flash) instead of the full J-Link pack, whose Qt4 GUI nixpkgs flags insecure, so `JLinkExe` isn't in the shell; and GCC comes from `mkShell`'s stdenv rather than a pinned `gccNN`.

The full outline is in PLAN.md §6.3. Implementation notes:
- **Imports:** copy the structure of `dev/Personal/Metrology/flake.nix` and import `../../lib/shell-banner.nix` for the banner.
- **zephyr-nix:**
  - Input `zephyr-nix.url = "github:nix-community/zephyr-nix"` (*verify* the repo location) with `inputs.nixpkgs.follows = "nixpkgs"`.
  - Use `zephyr-nix.packages.${system}.sdk-0_17.override { targets = [ "arm-zephyr-eabi" ]; }` (*verify* the attribute name) and `hosttools-nix`.
- **Python:** don't use zephyr-nix's `pythonEnv`. It is built from upstream Zephyr's requirements, while NCS has its own. Instead the shellHook creates `.venv` with uv:
  - `uv pip install west`;
  - once `external/zephyr/scripts/requirements.txt` exists, `uv pip install -r` that file and NCS's `external/nrf/scripts/requirements.txt`.
  - A stamp file (`.venv/.micnote-reqs-hash`) holds a hash of the requirements files, so reinstalls only happen when they change.
- **devShells:** `default`, `android` (`default` + `androidenv.composeAndroidPackages { ndkVersions = [ … ]; platformVersions = [ … ]; includeNDK = true; }`), and `hardware`.
- **Environment:** `FLUTTER_ROOT` is unset (nixpkgs' flutter wrapper handles it). Set `CHROME_EXECUTABLE` to nothing so `flutter doctor` stops nagging.
- **Last step:** the shellHook calls `scripts/doctor.sh --quiet` if the script exists in `$PWD`.

**Host change:** in `hosts/atta/configuration.nix`, change `services.udev.packages = [ pkgs.librealsense ];` to `[ pkgs.librealsense pkgs.nrf-udev ];`, with a comment matching the existing style. You run `nixos-rebuild switch` yourself.

### 4.6 README and .gitignore
Contents are as listed in PLAN.md §6. The README "Development setup" section is a short copy-paste block per OS that links to `docs/dev-setup.md`.

## 5. Task breakdown
**PR 1: versions and doctor**
- [x] S: `tools/versions.env` with the versions confirmed at implementation time (NCS v3.4.1, Zephyr SDK 1.0.1, Flutter 3.47.7, NDK r30 LTS; `PYTHON_MIN` raised to 3.12 for Zephyr 4.4, `DTC_MIN` added)
- [x] M: `scripts/lib/common.sh` + `doctor.sh` (version parsing, C++23 probe). In a Zephyr SDK shell (`ZEPHYR_SDK_INSTALL_DIR` set) it checks the SDK version instead of toolchain-manager; a missing J-Link and a Flutter patch mismatch are warnings, not failures
- [x] M: `doctor.ps1` (PSScriptAnalyzer clean; exercised under pwsh on Linux, not yet on a Windows machine; CI in PR 4 covers that)

**PR 2: bootstrap**
- [ ] L: `bootstrap.sh`: distro detection, plan/confirm, base, firmware, Flutter, `--android`, `--hardware`
- [ ] S: `Brewfile`
- [ ] L: `bootstrap.ps1`
- [ ] M: `docs/dev-setup.md`, including the manual path and VS Code setup (nRF Connect extension pack, clangd, Flutter)

**PR 3: repo hygiene**
- [ ] S: `.gitignore` additions
- [ ] M: README rewrite

**PR 4: CI for the scripts** (lands together with 0b's CI skeleton)
- [ ] M: `.github/workflows/devenv.yml`:
  - `shellcheck` and PSScriptAnalyzer;
  - matrix: `bootstrap.sh --yes --no-firmware` + `doctor.sh` in ubuntu:24.04, fedora:latest and archlinux:latest containers, plus macos-latest;
  - `bootstrap.ps1 -Yes -NoFirmware` on windows-latest;
  - a separate weekly job that runs the full firmware install on ubuntu (it's slow).

**NixHub (separate repo, not committed unless asked)**
- [x] M: `dev/Personal/Micnote/flake.nix` + lock (committed in NixHub `993db58`)
- [x] S: udev line in `hosts/atta/configuration.nix` (committed in NixHub `993db58`; takes effect after `nixos-rebuild switch`)

## 6. Testing and verification
- `shellcheck scripts/*.sh scripts/lib/*.sh`
- `pwsh -c "Invoke-ScriptAnalyzer -Path scripts -Recurse -Severity Warning"`
- `bash scripts/bootstrap.sh --dry-run` prints a sensible plan on each CI distro.
- The CI matrix from PR 4 is green.
- `nix flake check ~/Documents/NixHub/dev/Personal/Micnote`, then `nix develop … -c scripts/doctor.sh` passes.
- `nix develop …#android -c flutter doctor` lists an Android toolchain.
- `git check-ignore -v external/x .venv/x firmware/keys/a.pem app/build/x models/a.gguf` matches each path.
- **Manual:** a fresh Ubuntu VM and a Windows VM go from nothing to a passing `doctor` using only the README.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Distro package names drift | The CI containers catch it weekly; package names live in one table inside `bootstrap.sh` |
| `toolchain-manager` NCS version not yet published for an OS/architecture (e.g. Linux aarch64) | doctor flags it; `dev-setup.md` documents the manual Zephyr SDK + `pip install west` fallback |
| SEGGER license blocks automated install on Linux | Prompt-and-wait step; pyOCD documented as an open-source alternative for flashing over CMSIS-DAP |
| zephyr-nix lags the Zephyr SDK version NCS needs | Pin the zephyr-nix input to a matching revision; fallback is a `fetchurl` + `autoPatchelfHook` derivation of the official SDK tarball in the NixHub flake |
| Scripts editing user shell configs annoys people | Never edit rc files without an explicit prompt; print the lines instead |

## 8. Open questions
- Exact `NCS_VERSION` and `FLUTTER_VERSION`: take the latest stable releases on the day 0a starts.
- Support Linux aarch64 dev hosts (e.g. Asahi, ARM laptops)? This depends on NCS toolchain availability.
- Should doctor also check `clang-format`/`clang-tidy` versions so formatting matches CI? Probably yes, once 0b settles on a version.

## 9. Out of scope
- End-user installers (stage 5b).
- Dev containers and Codespaces (can be added later if contributors ask).
- Committing anything in NixHub without being asked.
