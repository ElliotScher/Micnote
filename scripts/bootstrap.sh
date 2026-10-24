#!/usr/bin/env bash
# Install Micnote's development tools on Linux (apt, dnf, pacman) or macOS
# (Homebrew). Windows uses scripts/bootstrap.ps1.
#
#   scripts/bootstrap.sh [--yes] [--dry-run] [--no-firmware] [--android] [--hardware]
#
#   --yes          don't ask before installing
#   --dry-run      print the plan and exit
#   --no-firmware  skip nrfutil, the NCS toolchain, J-Link and device access
#   --android      add a JDK and the Android SDK, NDK and build tools
#   --hardware     add KiCad and FreeCAD
#
# Prints what it will do and asks first. Steps that are already satisfied are
# left out, so re-running is safe. Shell rc files are never edited: lines to
# add are printed at the end. Finishes by running scripts/doctor.sh.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=lib/common.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

ASSUME_YES=0 DRY_RUN=0 NO_FIRMWARE=0 WITH_ANDROID=0 WITH_HARDWARE=0
for arg in "$@"; do
  case "$arg" in
    --yes | -y) ASSUME_YES=1 ;;
    --dry-run) DRY_RUN=1 ;;
    --no-firmware) NO_FIRMWARE=1 ;;
    --android) WITH_ANDROID=1 ;;
    --hardware) WITH_HARDWARE=1 ;;
    -h | --help) sed -n '2,15s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $arg (see --help)" ;;
  esac
done
export ASSUME_YES

load_versions
OS="$(os_id)"
LOCAL_BIN="$HOME/.local/bin"
FLUTTER_DIR="$HOME/.local/share/flutter"
if [[ "$OS" == macos ]]; then ANDROID_SDK="$HOME/Library/Android/sdk"; else ANDROID_SDK="$HOME/Android/Sdk"; fi

# --- Platform ---------------------------------------------------------------
PM=""
case "$OS" in
  macos) PM=brew ;;
  linux)
    [[ -e /etc/NIXOS ]] && die "NixOS isn't supported by bootstrap; set up manually (docs/dev-setup.md)."
    ids="$(. /etc/os-release && echo " ${ID:-} ${ID_LIKE:-} ")"
    case "$ids" in
      *" debian "* | *" ubuntu "*) PM=apt ;;
      *" fedora "* | *" rhel "*) PM=dnf ;;
      *" arch "*) PM=pacman ;;
    esac
    [[ -n "$PM" ]] || die "unsupported distribution ($ids); see docs/dev-setup.md for a manual setup."
    ;;
  windows) die "on Windows, run: powershell -ExecutionPolicy Bypass -File scripts\\bootstrap.ps1" ;;
  *) die "unsupported OS: $(uname -s)" ;;
esac

# Root on Linux comes from sudo, or nothing when already root (containers).
as_root() {
  if [[ "$(id -u)" == 0 ]]; then
    "$@"
  elif have sudo; then
    sudo "$@"
  else
    die "this step needs root and sudo isn't installed: $*"
  fi
}

# The user to add to groups: the one who ran sudo, if any.
TARGET_USER="${SUDO_USER:-$(id -un)}"

# --- Packages ---------------------------------------------------------------
# Names per package manager. Brew packages live in scripts/Brewfile.
apt_base=(git curl ca-certificates unzip xz-utils zip file
  cmake ninja-build pkg-config ccache g++ clang lld clang-format clang-tidy
  libdbus-1-dev
  libgtk-3-dev libsecret-1-dev liblzma-dev libglu1-mesa
  libvulkan-dev glslc
  python3 python3-venv)
dnf_base=(git curl unzip xz zip file which
  cmake ninja-build pkgconf-pkg-config ccache gcc-c++ clang lld clang-tools-extra
  dbus-devel
  gtk3-devel libsecret-devel xz-devel mesa-libGLU
  vulkan-loader-devel glslc
  python3)
pacman_base=(git curl unzip xz zip file which
  cmake ninja pkgconf ccache gcc clang lld
  dbus
  gtk3 libsecret glu
  vulkan-icd-loader vulkan-headers shaderc
  python)
# nrfutil's device command talks to probes through libusb.
apt_firmware=(libusb-1.0-0)
dnf_firmware=(libusb1)
pacman_firmware=(libusb)
apt_android=("openjdk-$JAVA_VERSION-jdk-headless")
# Fedora 44 only ships the newest JDKs; Temurin comes from Adoptium's repo,
# which Fedora packages (see install_dnf_jdk).
dnf_android=(adoptium-temurin-java-repository)
pacman_android=("jdk$JAVA_VERSION-openjdk")

pkg_installed() {
  case "$PM" in
    apt) dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep 'install ok installed' >/dev/null ;;
    dnf) rpm -q "$1" >/dev/null 2>&1 ;;
    pacman) pacman -Q "$1" >/dev/null 2>&1 ;;
  esac
}

APT_UPDATED=0
pm_install() {
  case "$PM" in
    apt)
      # Retries: PPAs on launchpadcontent.net fail transiently now and then.
      if ((!APT_UPDATED)); then as_root apt-get -o Acquire::Retries=3 update; APT_UPDATED=1; fi
      as_root env DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends "$@" ;;
    dnf) as_root dnf install -y "$@" ;;
    # Arch doesn't support partial upgrades, so sync and upgrade together.
    pacman) as_root pacman -Syu --needed --noconfirm "$@" ;;
  esac
}

# --- Plan -------------------------------------------------------------------
PLAN_DESC=()
PLAN_FN=()
step() { PLAN_DESC+=("$1"); PLAN_FN+=("$2"); }

NOTES=() # printed at the end: PATH lines and other follow-ups
note() { NOTES+=("$1"); }

# Linux distro packages.
MISSING_PKGS=()
wanted=()
case "$PM" in
  apt) wanted=("${apt_base[@]}") fw=("${apt_firmware[@]}") jdk=("${apt_android[@]}") ;;
  dnf) wanted=("${dnf_base[@]}") fw=("${dnf_firmware[@]}") jdk=("${dnf_android[@]}") ;;
  pacman) wanted=("${pacman_base[@]}") fw=("${pacman_firmware[@]}") jdk=("${pacman_android[@]}") ;;
esac
if [[ "$PM" != brew ]]; then
  ((NO_FIRMWARE)) || wanted+=(${fw[@]+"${fw[@]}"})
  ((!WITH_ANDROID)) || wanted+=(${jdk[@]+"${jdk[@]}"})
  for p in "${wanted[@]}"; do
    pkg_installed "$p" || MISSING_PKGS+=("$p")
  done
fi
install_packages() { pm_install "${MISSING_PKGS[@]}"; }
if ((${#MISSING_PKGS[@]})); then
  step "Install $PM packages: ${MISSING_PKGS[*]}" install_packages
fi
install_dnf_jdk() {
  as_root dnf install -y --enablerepo=adoptium-temurin-java-repository "temurin-$JAVA_VERSION-jdk"
}
if [[ "$PM" == dnf ]] && ((WITH_ANDROID)) && ! pkg_installed "temurin-$JAVA_VERSION-jdk"; then
  step "Install Temurin JDK $JAVA_VERSION (Adoptium repo)" install_dnf_jdk
fi

# macOS: Xcode Command Line Tools, Homebrew, then the Brewfile.
BREWFILE="$MICNOTE_ROOT/scripts/Brewfile"
install_clt() {
  xcode-select --install || true
  die "finish the Command Line Tools installer that just opened, then re-run bootstrap."
}
install_brewfile() { brew bundle --file="$BREWFILE"; }
if [[ "$PM" == brew ]]; then
  xcode-select -p >/dev/null 2>&1 || step "Install the Xcode Command Line Tools" install_clt
  have brew || die "Homebrew is required: install it from https://brew.sh, then re-run bootstrap."
  brew bundle check --file="$BREWFILE" >/dev/null 2>&1 ||
    step "Install Homebrew packages from scripts/Brewfile" install_brewfile
  ((!WITH_ANDROID)) || brew list --formula "openjdk@$JAVA_VERSION" >/dev/null 2>&1 ||
    step "Install openjdk@$JAVA_VERSION (Homebrew)" install_brew_jdk
fi
install_brew_jdk() { brew install "openjdk@$JAVA_VERSION"; }

# A C++23 compiler. Checked when the step runs, after the packages above.
ensure_cxx23() {
  CXX_FOUND="$(find_cxx23 || true)"
  if [[ -z "$CXX_FOUND" && "$PM" == apt ]]; then
    info "Default g++ can't build C++23 (<print>); installing g++-14."
    pm_install g++-14 || die "no g++-14 for this release; install gcc >= $GCC_MIN or clang >= 19 with libstdc++ 14 (docs/dev-setup.md)."
    CXX_FOUND="$(find_cxx23 || true)"
  fi
  [[ -n "$CXX_FOUND" ]] || die "no compiler here builds C++23 (<expected>, <print>); see docs/dev-setup.md."
  info "C++23 compiler: $CXX_FOUND"
}
CXX_FOUND="$(find_cxx23 || true)"
[[ -n "$CXX_FOUND" ]] || step "Make sure a C++23 compiler is available (g++-14 on older apt releases)" ensure_cxx23

# Flutter, cloned at the pinned tag.
flutter_version_of() { extract_version "$("$1" --version 2>/dev/null | grep -m1 '^Flutter' || true)"; }
install_flutter() {
  if [[ -d "$FLUTTER_DIR/.git" ]]; then
    git -C "$FLUTTER_DIR" fetch --depth 1 origin tag "$FLUTTER_VERSION"
    git -C "$FLUTTER_DIR" checkout -q "$FLUTTER_VERSION"
  else
    mkdir -p "$(dirname "$FLUTTER_DIR")"
    git clone --depth 1 --branch "$FLUTTER_VERSION" https://github.com/flutter/flutter.git "$FLUTTER_DIR"
  fi
  "$FLUTTER_DIR/bin/flutter" --version # first run downloads the Dart SDK
}
if [[ -x "$FLUTTER_DIR/bin/flutter" && "$(flutter_version_of "$FLUTTER_DIR/bin/flutter")" == "$FLUTTER_VERSION" ]]; then
  :
elif have flutter && [[ "$(flutter_version_of flutter)" == "$FLUTTER_VERSION" ]]; then
  FLUTTER_DIR="$(cd "$(dirname "$(command -v flutter)")/.." && pwd)"
else
  step "Install Flutter $FLUTTER_VERSION into $FLUTTER_DIR" install_flutter
fi

# Firmware: nrfutil, the NCS toolchain, J-Link, udev rules and serial access.
NRFUTIL="$(command -v nrfutil || echo "$LOCAL_BIN/nrfutil")"
install_nrfutil() {
  local target
  case "$OS-$(uname -m)" in
    linux-x86_64) target=x86_64-unknown-linux-gnu ;;
    linux-aarch64) target=aarch64-unknown-linux-gnu ;;
    macos-*) target=universal-apple-darwin ;;
    *) die "no nrfutil build for $(uname -m); see docs/dev-setup.md" ;;
  esac
  mkdir -p "$LOCAL_BIN"
  curl -fL --progress-bar -o "$LOCAL_BIN/nrfutil" \
    "https://files.nordicsemi.com/artifactory/swtools/external/nrfutil/executables/$target/nrfutil"
  chmod +x "$LOCAL_BIN/nrfutil"
  NRFUTIL="$LOCAL_BIN/nrfutil"
}
ncs_installed() {
  [[ -x "$NRFUTIL" ]] && "$NRFUTIL" toolchain-manager list 2>/dev/null |
    grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | grep -Fx "$NCS_VERSION" >/dev/null
}
install_ncs() {
  "$NRFUTIL" toolchain-manager --version >/dev/null 2>&1 || "$NRFUTIL" install toolchain-manager
  "$NRFUTIL" device --version >/dev/null 2>&1 || "$NRFUTIL" install device
  "$NRFUTIL" toolchain-manager install --ncs-version "$NCS_VERSION"
}
install_jlink() {
  if [[ "$PM" == brew ]]; then
    brew install --cask segger-jlink
    return
  fi
  info "SEGGER's licence doesn't allow redistributing J-Link, so it can't be installed automatically."
  info "Download the Linux .deb/.rpm/.tgz from https://www.segger.com/downloads/jlink/ and install it."
  info "(pyOCD is an open-source alternative for flashing over CMSIS-DAP.)"
  if ((!ASSUME_YES)) && [[ -t 0 ]]; then
    read -rp "Press Enter once it's installed, or to skip for now. " _
  fi
}
udev_installed() {
  local d
  for d in /etc/udev/rules.d /lib/udev/rules.d /usr/lib/udev/rules.d; do
    [[ -e "$d/71-nrf.rules" ]] && return 0
  done
  return 1
}
install_udev() {
  local tmp
  tmp="$(mktemp -d)"
  if [[ "$PM" == apt ]]; then
    curl -fsSL -o "$tmp/nrf-udev.deb" \
      "https://github.com/NordicSemiconductor/nrf-udev/releases/download/v$NRF_UDEV_VERSION/nrf-udev_$NRF_UDEV_VERSION-all.deb"
    chmod 0644 "$tmp/nrf-udev.deb"
    pm_install "$tmp/nrf-udev.deb"
  else
    curl -fsSL -o "$tmp/71-nrf.rules" \
      "https://raw.githubusercontent.com/NordicSemiconductor/nrf-udev/v$NRF_UDEV_VERSION/nrf-udev_$NRF_UDEV_VERSION-all/lib/udev/rules.d/71-nrf.rules"
    as_root install -D -m 0644 "$tmp/71-nrf.rules" /etc/udev/rules.d/71-nrf.rules
  fi
  rm -rf "$tmp"
  if have udevadm; then as_root udevadm control --reload || true; fi
}
# Serial devices belong to dialout on Debian/Fedora and uucp on Arch.
serial_group() {
  if getent group dialout >/dev/null 2>&1; then echo dialout; elif getent group uucp >/dev/null 2>&1; then echo uucp; fi
}
add_to_serial_group() {
  local g
  g="$(serial_group)"
  if ((ASSUME_YES)) || confirm "Add $TARGET_USER to the $g group (needed for USB serial)?"; then
    as_root usermod -aG "$g" "$TARGET_USER"
    note "Log out and back in so the $g group membership takes effect."
  else
    note "To use USB serial later: sudo usermod -aG $g $TARGET_USER"
  fi
}
if ((!NO_FIRMWARE)); then
  have nrfutil || [[ -x "$LOCAL_BIN/nrfutil" ]] || step "Download nrfutil into $LOCAL_BIN" install_nrfutil
  ncs_installed || step "Install the NCS $NCS_VERSION toolchain (nrfutil toolchain-manager; a large download)" install_ncs
  have JLinkExe || step "SEGGER J-Link (macOS: Homebrew cask; Linux: manual download)" install_jlink
  if [[ "$OS" == linux ]]; then
    udev_installed || step "Install Nordic's udev rules (nrf-udev $NRF_UDEV_VERSION)" install_udev
    g="$(serial_group)"
    if [[ -n "$g" && "$TARGET_USER" != root ]] && ! id -nG "$TARGET_USER" | tr ' ' '\n' | grep -x "$g" >/dev/null; then
      step "Add $TARGET_USER to the $g group (asks first)" add_to_serial_group
    fi
  fi
fi

# Android: command-line tools, then SDK packages. sdkmanager is deprecated in
# favour of the `android` CLI, and only the CLI itself accepts --no-metrics
# (it collects usage data otherwise), so call it directly.
ANDROID_CLI="$ANDROID_SDK/cmdline-tools/latest/bin/android"
install_cmdline_tools() {
  local os tmp
  if [[ "$OS" == macos ]]; then
    if [[ "$(uname -m)" == arm64 ]]; then os=mac_arm64; else os=mac_x86_64; fi
  else
    os=linux
  fi
  tmp="$(mktemp -d)"
  curl -fL --progress-bar -o "$tmp/tools.zip" \
    "https://dl.google.com/android/repository/commandlinetools-$os-${ANDROID_CMDLINE_TOOLS}_latest.zip"
  unzip -q "$tmp/tools.zip" -d "$tmp"
  mkdir -p "$ANDROID_SDK/cmdline-tools"
  rm -rf "$ANDROID_SDK/cmdline-tools/latest"
  mv "$tmp/cmdline-tools" "$ANDROID_SDK/cmdline-tools/latest"
  rm -rf "$tmp"
}
android_java_home() {
  if [[ "$PM" == brew ]]; then
    echo "$(brew --prefix "openjdk@$JAVA_VERSION")/libexec/openjdk.jdk/Contents/Home"
  fi
}
install_android_packages() {
  local jh
  jh="$(android_java_home)"
  if [[ -n "$jh" ]]; then export JAVA_HOME="$jh"; fi
  "$ANDROID_CLI" --no-metrics --sdk="$ANDROID_SDK" sdk install "platform-tools" "platforms;android-$ANDROID_PLATFORM" \
    "build-tools;$ANDROID_BUILD_TOOLS" "ndk;$ANDROID_NDK_VERSION" "cmake;$ANDROID_CMAKE"
}
if ((WITH_ANDROID)); then
  [[ -x "$ANDROID_CLI" ]] || step "Download Android command-line tools into $ANDROID_SDK" install_cmdline_tools
  if [[ ! -d "$ANDROID_SDK/platforms/android-$ANDROID_PLATFORM" || ! -d "$ANDROID_SDK/ndk/$ANDROID_NDK_VERSION" ||
    ! -d "$ANDROID_SDK/build-tools/$ANDROID_BUILD_TOOLS" ]]; then
    step "Install Android SDK packages (platform $ANDROID_PLATFORM, NDK $ANDROID_NDK_VERSION; under Google's SDK terms)" install_android_packages
  fi
fi

# Hardware: KiCad and FreeCAD.
kicad_ok() {
  have kicad-cli && version_ge "$(extract_version "$(kicad-cli --version 2>/dev/null || true)")" "$KICAD_MIN"
}
# freecad_packaged: the distro has a freecad package (Ubuntu 24.04 and
# Fedora 44 dropped theirs).
freecad_packaged() {
  case "$PM" in
    apt) apt-cache policy freecad 2>/dev/null | grep 'Candidate: [0-9]' >/dev/null ;;
    dnf) dnf -q list --available freecad >/dev/null 2>&1 || pkg_installed freecad ;;
    *) return 0 ;;
  esac
}
install_freecad_appimage() {
  local name tmp
  name="FreeCAD_$FREECAD_VERSION-Linux-$(uname -m)-py311.AppImage"
  tmp="$(mktemp -d)"
  curl -fL --progress-bar -o "$tmp/$name" "https://github.com/FreeCAD/FreeCAD/releases/download/$FREECAD_VERSION/$name"
  curl -fsSL -o "$tmp/$name.sha256" "https://github.com/FreeCAD/FreeCAD/releases/download/$FREECAD_VERSION/$name-SHA256.txt"
  (cd "$tmp" && sha256sum -c <(awk -v n="$name" '{print $1 "  " n}' "$name.sha256")) ||
    die "FreeCAD AppImage checksum mismatch"
  mkdir -p "$LOCAL_BIN"
  install -m 0755 "$tmp/$name" "$LOCAL_BIN/freecad"
  rm -rf "$tmp"
}
install_hardware() {
  case "$PM" in
    apt)
      if [[ "$(. /etc/os-release && echo "${ID:-}")" == ubuntu ]]; then
        # Ubuntu's own KiCad trails; the KiCad project's PPA has the pinned series.
        pm_install software-properties-common
        as_root add-apt-repository -y "ppa:kicad/kicad-${KICAD_MIN%%.*}.0-releases"
        APT_UPDATED=0
      fi
      pm_install kicad ;;
    dnf | pacman) pm_install kicad ;;
    brew) brew install --cask kicad freecad; return ;;
  esac
  if have freecad || [[ -x "$LOCAL_BIN/freecad" ]]; then
    :
  elif freecad_packaged; then
    pm_install freecad
  else
    info "No FreeCAD package for this release; installing the official $FREECAD_VERSION AppImage."
    # AppImages need FUSE 2 at runtime.
    case "$PM" in
      apt) pm_install libfuse2t64 || pm_install libfuse2 ;;
      dnf) pm_install fuse-libs ;;
    esac
    install_freecad_appimage
  fi
}
if ((WITH_HARDWARE)); then
  if ! kicad_ok || ! { have freecad || have FreeCAD || [[ -x "$LOCAL_BIN/freecad" ]]; }; then
    step "Install KiCad >= $KICAD_MIN and FreeCAD" install_hardware
  fi
fi

# --- Show, confirm, run -----------------------------------------------------
if ((${#PLAN_DESC[@]} == 0)); then
  info "Nothing to install: everything bootstrap manages is already here."
else
  info "bootstrap will:"
  for ((i = 0; i < ${#PLAN_DESC[@]}; i++)); do
    info "  $((i + 1)). ${PLAN_DESC[i]}"
  done
  if ((DRY_RUN)); then
    info "(dry run: nothing was changed)"
    exit 0
  fi
  confirm "Continue?" || die "nothing changed. Re-run with --yes to skip this question."
  # Steps run as plain commands so `set -e` stops them at the first failing
  # command (it's switched off inside `step || die`); the trap names the step.
  CURRENT_STEP=""
  trap 'rc=$?; if ((rc != 0)) && [[ -n "$CURRENT_STEP" ]]; then err "step failed: $CURRENT_STEP"; fi' EXIT
  for ((i = 0; i < ${#PLAN_DESC[@]}; i++)); do
    info ""
    info "${C_GREEN}==> ${PLAN_DESC[i]}${C_RESET}"
    CURRENT_STEP="${PLAN_DESC[i]}"
    "${PLAN_FN[i]}"
    CURRENT_STEP=""
  done
fi
((!DRY_RUN)) || exit 0

# --- Follow-ups and doctor --------------------------------------------------
extra_path=""
add_path() {
  case ":$PATH:" in *":$1:"*) ;; *)
    extra_path="$1${extra_path:+:$extra_path}"
    note "Add to your shell profile: export PATH=\"$1:\$PATH\"" ;;
  esac
}
[[ -x "$FLUTTER_DIR/bin/flutter" ]] && add_path "$FLUTTER_DIR/bin"
if [[ -x "$LOCAL_BIN/nrfutil" || -x "$LOCAL_BIN/freecad" ]]; then add_path "$LOCAL_BIN"; fi
if ((WITH_ANDROID)); then
  note "Add to your shell profile: export ANDROID_HOME=\"$ANDROID_SDK\""
  jh="$(android_java_home)"
  if [[ -n "$jh" ]]; then note "Add to your shell profile: export JAVA_HOME=\"$jh\""; fi
fi
[[ -n "$CXX_FOUND" ]] || CXX_FOUND="$(find_cxx23 || true)"
case "$CXX_FOUND" in
  "" | c++ | g++ | clang++) ;;
  *) note "Add to your shell profile: export CXX=$CXX_FOUND  (your default compiler can't build C++23)" ;;
esac
if ((!NO_FIRMWARE)) && ncs_installed; then
  note "Firmware builds run inside the NCS toolchain: nrfutil toolchain-manager launch --ncs-version $NCS_VERSION --shell"
fi
if [[ "$PM" == brew ]]; then
  note "For iOS builds: install Xcode from the App Store, then: brew install cocoapods"
fi

if ((${#NOTES[@]})); then
  info ""
  info "Next steps:"
  for n in "${NOTES[@]}"; do info "  - $n"; done
fi

info ""
info "Running scripts/doctor.sh:"
doctor_args=()
((!NO_FIRMWARE)) || doctor_args+=(--no-firmware)
PATH="${extra_path:+$extra_path:}$PATH" CXX="${CXX_FOUND:-${CXX:-}}" \
  "$MICNOTE_ROOT/scripts/doctor.sh" ${doctor_args[@]+"${doctor_args[@]}"}
