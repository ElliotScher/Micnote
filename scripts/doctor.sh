#!/usr/bin/env bash
# Check the dev toolchain against tools/versions.env.
#
#   scripts/doctor.sh [--quiet]
#
# Prints tool / required / found / status and exits non-zero if a required
# tool is missing or too old. --quiet prints only the problems (the NixHub
# shell runs it that way on entry). Optional tools never fail the run.
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=lib/common.sh
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

QUIET=0
for arg in "$@"; do
  case "$arg" in
    --quiet | -q) QUIET=1 ;;
    -h | --help) sed -n '2,8s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $arg" ;;
  esac
done

load_versions
OS="$(os_id)"

# Each row: tool|required|found|status|note. Status is one of
#   OK, MISSING, OLD (fail the run), WARN (reported, doesn't fail), SKIP.
ROWS=()
FAILED=0
row() {
  ROWS+=("$1|$2|$3|$4|${5:-}")
  [[ "$4" == MISSING || "$4" == OLD ]] && FAILED=1
  return 0
}

# check_min NAME MIN FOUND_VERSION [optional]: OK/OLD/MISSING row.
check_min() {
  local name="$1" min="$2" found="$3" optional="${4:-}"
  if [[ -z "$found" ]]; then
    row "$name" ">= $min" "-" "$([[ -n "$optional" ]] && echo WARN || echo MISSING)"
  elif version_ge "$found" "$min"; then
    row "$name" ">= $min" "$found" OK
  else
    row "$name" ">= $min" "$found" "$([[ -n "$optional" ]] && echo WARN || echo OLD)"
  fi
}

# tool_version CMD [ARGS...]: first version number in the command's output.
tool_version() {
  have "$1" || return 0
  extract_version "$("$@" 2>&1 | head -n 5 || true)"
}

# --- Build tools ------------------------------------------------------------
check_min cmake "$CMAKE_MIN" "$(tool_version cmake --version)"
check_min ninja "$NINJA_MIN" "$(tool_version ninja --version)"

# --- C++ compiler and C++23 probe -------------------------------------------
CXX_BIN="${CXX:-}"
cxx_ver=""
if [[ -z "$CXX_BIN" ]]; then
  for c in clang++ g++ c++; do
    if have "$c"; then CXX_BIN="$c"; break; fi
  done
fi

if [[ -z "$CXX_BIN" ]] || ! have "$CXX_BIN"; then
  row "C++ compiler" "clang >= $CLANG_MIN or gcc >= $GCC_MIN" "-" MISSING
  row "C++23 probe" "<expected>, <print>" "-" SKIP "no compiler"
else
  cxx_banner="$("$CXX_BIN" --version 2>&1 | head -n 1 || true)"
  cxx_ver="$(extract_version "$cxx_banner")"
  case "$cxx_banner" in
    "")
      row "C++ compiler ($CXX_BIN)" "clang >= $CLANG_MIN or gcc >= $GCC_MIN" "?" MISSING "--version printed nothing" ;;
    *"Apple clang"*)
      # Apple's numbering doesn't track upstream LLVM; the probe below decides.
      row "C++ compiler ($CXX_BIN)" "Xcode clang" "$cxx_ver" OK "Apple clang: checked by the probe" ;;
    *clang*) check_min "C++ compiler ($CXX_BIN, clang)" "$CLANG_MIN" "$cxx_ver" ;;
    *) check_min "C++ compiler ($CXX_BIN, gcc)" "$GCC_MIN" "$cxx_ver" ;;
  esac
fi

if [[ -n "$CXX_BIN" ]] && have "$CXX_BIN" && [[ -z "$cxx_ver" ]]; then
  row "C++23 probe" "<expected>, <print>" "-" SKIP "no working compiler"
elif [[ -n "$CXX_BIN" ]] && have "$CXX_BIN"; then
  probe_dir="$(mktemp -d)"
  trap 'rm -rf "$probe_dir"' EXIT
  printf '#include <expected>\n#include <print>\nint main(){std::expected<int,int> e{1}; std::println("{}", *e);}\n' \
    >"$probe_dir/probe.cpp"
  if "$CXX_BIN" -std=c++23 "$probe_dir/probe.cpp" -o "$probe_dir/probe" >"$probe_dir/log" 2>&1; then
    # Run it too: a binary can build fine and still load an older libstdc++
    # at runtime (an inherited LD_LIBRARY_PATH outranks the RUNPATH).
    if "$probe_dir/probe" >"$probe_dir/run" 2>&1; then
      row "C++23 probe" "<expected>, <print>" "builds, runs" OK
    else
      run_err="$(grep -m1 -o "version \`[^']*' not found" "$probe_dir/run" || head -n 1 "$probe_dir/run")"
      row "C++23 probe" "<expected>, <print>" "won't run" OLD \
        "$run_err${LD_LIBRARY_PATH:+ (LD_LIBRARY_PATH is set; unset it)}"
    fi
  else
    missing=()
    for h in expected print; do
      printf '#include <%s>\nint main(){}\n' "$h" >"$probe_dir/h.cpp"
      "$CXX_BIN" -std=c++23 -fsyntax-only "$probe_dir/h.cpp" >/dev/null 2>&1 || missing+=("<$h>")
    done
    if ((${#missing[@]})); then
      row "C++23 probe" "<expected>, <print>" "fails" OLD "standard library too old for ${missing[*]}"
    else
      row "C++23 probe" "<expected>, <print>" "fails" OLD "$(head -n 1 "$probe_dir/log")"
    fi
  fi
fi

# --- Firmware ---------------------------------------------------------------
check_min python3 "$PYTHON_MIN" "$(tool_version python3 --version)"

if [[ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" ]]; then
  # Zephyr SDK provided directly (the NixHub shell, or a manual install).
  sdk_ver="$(cat "$ZEPHYR_SDK_INSTALL_DIR/sdk_version" 2>/dev/null || true)"
  if [[ -z "$sdk_ver" ]]; then
    row "Zephyr SDK" "$ZEPHYR_SDK_VERSION" "-" MISSING "no sdk_version in \$ZEPHYR_SDK_INSTALL_DIR"
  elif [[ "$sdk_ver" == "$ZEPHYR_SDK_VERSION" ]]; then
    row "Zephyr SDK" "$ZEPHYR_SDK_VERSION" "$sdk_ver" OK
  else
    row "Zephyr SDK" "$ZEPHYR_SDK_VERSION" "$sdk_ver" OLD "NCS $NCS_VERSION documents SDK $ZEPHYR_SDK_VERSION"
  fi
  check_min west "1.0" "$(tool_version west --version)"
  check_min dtc "$DTC_MIN" "$(tool_version dtc --version)"
  in_ncs_env=0
else
  # Nordic's toolchain-manager bundles west, dtc and the SDK per NCS version.
  ncs_found=""
  if have nrfutil && nrfutil toolchain-manager --version >/dev/null 2>&1; then
    ncs_found="$(nrfutil toolchain-manager list 2>/dev/null | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | grep -Fx "$NCS_VERSION" | head -n 1 || true)"
  fi
  if [[ -n "$ncs_found" ]]; then
    row "NCS toolchain" "$NCS_VERSION" "$ncs_found" OK "nrfutil toolchain-manager"
    in_ncs_env=1
  else
    row "NCS toolchain" "$NCS_VERSION" "-" MISSING "nrfutil toolchain-manager install --ncs-version $NCS_VERSION"
    in_ncs_env=0
  fi
  for t in west dtc; do
    if have "$t"; then
      row "$t" "-" "$(tool_version "$t" --version)" OK
    elif ((in_ncs_env)); then
      row "$t" "-" "in NCS env" OK "nrfutil toolchain-manager launch --ncs-version $NCS_VERSION --shell"
    else
      row "$t" "-" "-" MISSING
    fi
  done
fi

if have nrfutil; then
  row nrfutil "-" "$(tool_version nrfutil --version)" OK
else
  row nrfutil "-" "-" MISSING "see docs/dev-setup.md"
fi

if have JLinkExe; then
  # Presence only: JLinkExe has no non-interactive --version, and starting
  # it can block waiting for a probe or a licence dialog.
  row "J-Link" "-" "JLinkExe" OK
elif have pyocd; then
  row "J-Link" "-" "-" WARN "no JLinkExe; flash with pyOCD or nrfutil instead"
else
  row "J-Link" "-" "-" WARN "no JLinkExe or pyOCD; needed only for flashing"
fi

# --- App --------------------------------------------------------------------
if have flutter; then
  fl_ver="$(extract_version "$(flutter --version 2>/dev/null | grep -m1 '^Flutter' || true)")"
  if [[ -z "$fl_ver" ]]; then
    row flutter "$FLUTTER_VERSION" "?" MISSING "couldn't parse flutter --version"
  elif [[ "$fl_ver" == "$FLUTTER_VERSION" ]]; then
    row flutter "$FLUTTER_VERSION" "$fl_ver" OK
  elif [[ "${fl_ver%.*}" == "${FLUTTER_VERSION%.*}" ]]; then
    row flutter "$FLUTTER_VERSION" "$fl_ver" WARN "same minor, different patch"
  else
    row flutter "$FLUTTER_VERSION" "$fl_ver" OLD
  fi
else
  row flutter "$FLUTTER_VERSION" "-" MISSING
fi

# --- Optional ---------------------------------------------------------------
android_sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [[ -z "$android_sdk" ]]; then
  for d in "$HOME/Android/Sdk" "$HOME/Library/Android/sdk"; do
    if [[ -d "$d" ]]; then android_sdk="$d"; break; fi
  done
fi
if [[ -z "$android_sdk" || ! -d "$android_sdk" ]]; then
  row "Android SDK (optional)" "platform $ANDROID_PLATFORM" "-" SKIP "bootstrap --android"
else
  if [[ -d "$android_sdk/platforms/android-$ANDROID_PLATFORM" ]]; then
    row "Android SDK (optional)" "platform $ANDROID_PLATFORM" "$android_sdk" OK
  else
    row "Android SDK (optional)" "platform $ANDROID_PLATFORM" "$android_sdk" WARN "android-$ANDROID_PLATFORM not installed"
  fi
  if [[ -d "$android_sdk/ndk/$ANDROID_NDK_VERSION" ]]; then
    row "Android NDK (optional)" "$ANDROID_NDK_VERSION" "$ANDROID_NDK_VERSION" OK
  else
    row "Android NDK (optional)" "$ANDROID_NDK_VERSION" "-" WARN
  fi
fi

if have kicad-cli; then
  kicad_ver="$(extract_version "$(kicad-cli --version 2>/dev/null || true)")"
  if [[ -n "$kicad_ver" ]]; then
    check_min "KiCad (optional)" "$KICAD_MIN" "$kicad_ver" optional
  else
    row "KiCad (optional)" ">= $KICAD_MIN" "?" WARN "kicad-cli --version failed"
  fi
else
  row "KiCad (optional)" ">= $KICAD_MIN" "-" SKIP "bootstrap --hardware"
fi

if have vulkaninfo || { have pkg-config && pkg-config --exists vulkan 2>/dev/null; }; then
  row "Vulkan (optional)" "-" "$( (have pkg-config && pkg-config --modversion vulkan 2>/dev/null) || echo present)" OK
elif [[ "$OS" != macos ]]; then
  row "Vulkan (optional)" "-" "-" SKIP "GPU builds of whisper.cpp only"
fi

for t in clang-format clang-tidy; do
  if have "$t"; then
    row "$t (optional)" "-" "$(tool_version "$t" --version)" OK
  else
    row "$t (optional)" "-" "-" SKIP
  fi
done

if [[ "$OS" == macos ]]; then
  if have xcodebuild && xcodebuild -version >/dev/null 2>&1; then
    row "Xcode (optional)" "-" "$(extract_version "$(xcodebuild -version 2>/dev/null | head -n 1)")" OK
  else
    row "Xcode (optional)" "-" "-" SKIP "needed for iOS/macOS app builds"
  fi
fi

# --- Linux device access ----------------------------------------------------
if [[ "$OS" == linux ]]; then
  udev_rule=""
  for d in /etc/udev/rules.d /run/udev/rules.d /lib/udev/rules.d /usr/lib/udev/rules.d; do
    for f in "$d"/*nrf*.rules; do
      if [[ -e "$f" ]]; then udev_rule="$f"; break 2; fi
    done
  done
  if [[ -n "$udev_rule" ]]; then
    row "nRF udev rules" "-" "$(basename "$udev_rule")" OK
  else
    row "nRF udev rules" "-" "-" WARN "needed to flash without sudo; see docs/dev-setup.md"
  fi

  if id -nG | tr ' ' '\n' | grep -qx dialout; then
    row "dialout group" "-" "member" OK
  elif [[ -e /etc/NIXOS ]]; then
    row "dialout group" "-" "-" SKIP "NixOS: nrf-udev grants access via uaccess"
  else
    row "dialout group" "-" "-" WARN "sudo usermod -aG dialout \"\$USER\", then log in again"
  fi
fi

# --- Report -----------------------------------------------------------------
colour() {
  case "$1" in
    OK) printf '%s' "$C_GREEN" ;;
    MISSING | OLD) printf '%s' "$C_RED" ;;
    WARN) printf '%s' "$C_YELLOW" ;;
    *) printf '%s' "$C_DIM" ;;
  esac
}

w1=4 w2=8 w3=5
for r in "${ROWS[@]}"; do
  IFS='|' read -r t req found _ _ <<<"$r"
  ((${#t} > w1)) && w1=${#t}
  ((${#req} > w2)) && w2=${#req}
  ((${#found} > w3)) && w3=${#found}
done

printed=0
for r in "${ROWS[@]}"; do
  IFS='|' read -r t req found st note <<<"$r"
  if ((QUIET)) && [[ "$st" == OK || "$st" == SKIP ]]; then continue; fi
  if ((!printed)); then
    printf "%-${w1}s  %-${w2}s  %-${w3}s  %s\n" Tool Required Found Status
    printed=1
  fi
  printf "%-${w1}s  %-${w2}s  %-${w3}s  %s%-7s%s %s\n" "$t" "$req" "$found" "$(colour "$st")" "$st" "$C_RESET" \
    "${note:+${C_DIM}${note}${C_RESET}}"
done

if ((FAILED)); then
  err "required tools are missing or too old (pins: tools/versions.env; help: docs/dev-setup.md)"
  exit 1
fi
((QUIET)) || info "${C_GREEN}All required tools found.${C_RESET}"
