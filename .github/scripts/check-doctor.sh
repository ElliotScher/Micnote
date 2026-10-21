#!/usr/bin/env bash
# Run doctor and check that it *worked*, not that the machine passes: CI
# runners lack most of Micnote's toolchain, so doctor is expected to exit 1.
#
#   check-doctor.sh SHELL [EXPECT_REGEX...]
#
# SHELL runs scripts/doctor.sh (e.g. /bin/bash for macOS's bash 3.2). Fails
# unless doctor exits 0 or 1, printed its header and its closing verdict
# (so it ran to the end), hit no shell errors, and every EXPECT_REGEX
# matches a line of its output.
set -uo pipefail

sh="$1"
shift
out="$(mktemp)"

"$sh" scripts/doctor.sh >"$out" 2>&1
code=$?
cat "$out"
# shellcheck disable=SC2016 # $BASH_VERSION is for the child shell to expand
echo "--- doctor exited $code under $("$sh" -c 'echo "$BASH_VERSION"')"

fail() { echo "::error::$*"; exit 1; }

[[ $code == 0 || $code == 1 ]] || fail "doctor exited $code (expected 0 or 1)"
grep -q '^Tool  *Required  *Found  *Status' "$out" || fail "no table header"
grep -qE 'All required tools found\.|required tools are missing or too old' "$out" ||
  fail "no closing verdict: doctor stopped early"
if grep -nE 'line [0-9]+: |unbound variable|syntax error|command not found|bad substitution' "$out"; then
  fail "shell errors in doctor output"
fi
for re in "$@"; do
  grep -qE "$re" "$out" || fail "expected a line matching: $re"
done

# --quiet must print no OK rows.
"$sh" scripts/doctor.sh --quiet >"$out" 2>&1
if grep -E '  OK ' "$out"; then fail "--quiet printed OK rows"; fi
echo "doctor check passed"
