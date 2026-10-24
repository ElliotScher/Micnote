# shellcheck shell=bash source-path=SCRIPTDIR
# Shared helpers for scripts/*.sh. Source it; don't execute it.
# Safe under `set -euo pipefail`. Don't end a pipeline with `grep -q`: it exits
# at the first match, the writer gets SIGPIPE, and pipefail reports failure.

MICNOTE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export MICNOTE_ROOT

# Colour only when writing to a terminal and NO_COLOR is unset.
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
  C_RED=$'\e[31m' C_GREEN=$'\e[32m' C_YELLOW=$'\e[33m' C_DIM=$'\e[2m' C_RESET=$'\e[0m'
else
  C_RED='' C_GREEN='' C_YELLOW='' C_DIM='' C_RESET=''
fi
export C_RED C_GREEN C_YELLOW C_DIM C_RESET

info() { printf '%s\n' "$*"; }
warn() { printf '%swarning:%s %s\n' "$C_YELLOW" "$C_RESET" "$*" >&2; }
err()  { printf '%serror:%s %s\n' "$C_RED" "$C_RESET" "$*" >&2; }
die()  { err "$*"; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# version_ge A B: true if dotted version A >= B. Missing parts count as 0,
# and any non-numeric suffix on a part ("1.0.1-rc1", "18git") is ignored.
version_ge() {
  local -a a b
  local i x y
  IFS=. read -ra a <<<"$1"
  IFS=. read -ra b <<<"$2"
  for ((i = 0; i < ${#a[@]} || i < ${#b[@]}; i++)); do
    x="${a[i]:-0}" y="${b[i]:-0}"
    x="${x%%[!0-9]*}" y="${y%%[!0-9]*}"
    x=$((10#${x:-0})) y=$((10#${y:-0}))
    ((x > y)) && return 0
    ((x < y)) && return 1
  done
  return 0
}

# extract_version TEXT: print the first N.N[.N...] in TEXT, or nothing.
extract_version() {
  [[ "$1" =~ ([0-9]+(\.[0-9]+)+) ]] && printf '%s\n' "${BASH_REMATCH[1]}"
  return 0
}

# confirm PROMPT: yes/no prompt. Always yes when ASSUME_YES=1 (--yes);
# always no when stdin isn't a terminal and --yes wasn't given.
confirm() {
  [[ "${ASSUME_YES:-0}" == 1 ]] && return 0
  [[ -t 0 ]] || return 1
  local reply
  read -rp "$1 [y/N] " reply
  [[ "$reply" == [yY] || "$reply" == [yY][eE][sS] ]]
}

# load_versions: source tools/versions.env into the environment.
load_versions() {
  local f="$MICNOTE_ROOT/tools/versions.env"
  [[ -f "$f" ]] || die "missing $f"
  set -a
  # shellcheck source=../../tools/versions.env
  . "$f"
  set +a
}

# cxx23_works CXX: CXX builds and runs a program using <expected> and <print>.
cxx23_works() {
  local d rc=1
  d="$(mktemp -d)"
  printf '#include <expected>\n#include <print>\nint main(){std::expected<int,int> e{1}; std::println("{}", *e);}\n' >"$d/p.cpp"
  if "$1" -std=c++23 "$d/p.cpp" -o "$d/p" >/dev/null 2>&1 && "$d/p" >/dev/null 2>&1; then rc=0; fi
  rm -rf "$d"
  return "$rc"
}

# find_cxx23: print the first C++23-capable compiler, trying $CXX, then the
# defaults CMake picks (c++, g++, clang++), then versioned names such as
# Ubuntu's g++-14. Prints nothing (and fails) if there is none.
find_cxx23() {
  local c
  for c in ${CXX:-} c++ g++ clang++ g++-14 g++-15 g++-16 g++-17 clang++-19 clang++-20 clang++-21 clang++-22 clang++-23; do
    if have "$c" && cxx23_works "$c"; then
      printf '%s\n' "$c"
      return 0
    fi
  done
  return 1
}

# os_id: linux | macos | windows | unknown
os_id() {
  case "$(uname -s)" in
    Linux) echo linux ;;
    Darwin) echo macos ;;
    MINGW* | MSYS* | CYGWIN*) echo windows ;;
    *) echo unknown ;;
  esac
}
