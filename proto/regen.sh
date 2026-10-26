#!/usr/bin/env bash
# Regenerates proto/gen/ from micnote.proto with the nanopb generator pinned
# in tools/versions.env. The generator runs in a throwaway venv under
# build/.nanopb-venv; nothing is installed system-wide.
#
#   proto/regen.sh          regenerate proto/gen/
#   proto/regen.sh --check  regenerate, then fail if proto/gen/ changed
set -euo pipefail

# shellcheck source-path=SCRIPTDIR source=../scripts/lib/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/../scripts/lib/common.sh"

check=0
case "${1:-}" in
  --check) check=1 ;;
  "") ;;
  -h | --help) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) die "unknown argument: $1" ;;
esac

nanopb_version="$(sed -n 's/^NANOPB_VERSION=\([^[:space:]#]*\).*/\1/p' "$MICNOTE_ROOT/tools/versions.env")"
[[ -n "$nanopb_version" ]] || die "NANOPB_VERSION missing from tools/versions.env"

venv="$MICNOTE_ROOT/build/.nanopb-venv"
stamp="$venv/.nanopb-$nanopb_version"
if [[ ! -f "$stamp" ]]; then
  info "Creating $venv with nanopb $nanopb_version"
  rm -rf "$venv"
  if have uv; then
    uv venv --quiet "$venv"
    uv pip install --quiet --python "$venv/bin/python" "nanopb==$nanopb_version"
  else
    python3 -m venv "$venv"
    "$venv/bin/python" -m pip install --quiet --disable-pip-version-check "nanopb==$nanopb_version"
  fi
  touch "$stamp"
fi

proto_dir="$MICNOTE_ROOT/proto"
gen_dir="$proto_dir/gen"
find "$gen_dir" -name '*.pb.[ch]' -delete
# Run from proto/ so the paths nanopb writes into the generated files are
# relative and identical on every machine.
(cd "$proto_dir" && "$venv/bin/nanopb_generator" -q -I . -D gen micnote.proto)
info "Regenerated proto/gen with nanopb $nanopb_version"

if ((check)); then
  if ! git -C "$MICNOTE_ROOT" diff --exit-code -- proto/gen ||
    [[ -n "$(git -C "$MICNOTE_ROOT" ls-files --others --exclude-standard -- proto/gen)" ]]; then
    die "proto/gen is out of date: run proto/regen.sh and commit the result"
  fi
  info "proto/gen is up to date"
fi
