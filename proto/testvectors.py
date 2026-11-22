#!/usr/bin/env python3
"""Extract proto/testvectors/ from the firmware test-vector generator's output.

firmware/tests/testvectors writes recordings with the real recstore and
prints each file between `MNTV-FILE <name>` and `MNTV-END` lines as hex.
This script runs that test (or reads a log it already wrote) and writes the
files into proto/testvectors/. The core's tests decrypt them, so CI proves
the firmware and the host agree on the format and the crypto.

    python3 proto/testvectors.py                  run the generator, extract
    python3 proto/testvectors.py --log PATH       extract from a handler.log, or
                                                  a twister output dir holding one
    python3 proto/testvectors.py --check [...]    then fail if proto/testvectors/
                                                  changed

Running the generator needs the firmware environment (west, Zephyr SDK).
Standard library only. Tests: proto/test_testvectors.py.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from pathlib import Path

import regen
from regen import fail

ROOT = Path(__file__).resolve().parent.parent
OUT = "proto/testvectors"
GENERATOR = "firmware/tests/testvectors"
LOG_PARENT = "micnote.testvectors"

# Only plain names of the kinds the generator writes: nothing that could
# escape the output directory or overwrite something else.
NAME = re.compile(r"[a-z0-9_]+\.(mnr|json)")
KINDS = ("*.mnr", "*.json")


def parse_log(text: str) -> dict[str, bytes]:
    """The files in a generator log, by name."""
    files: dict[str, bytes] = {}
    name: str | None = None
    hex_lines: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if name is None:
            if line.startswith("MNTV-FILE "):
                name = line.removeprefix("MNTV-FILE ")
                if not NAME.fullmatch(name):
                    fail(f"unexpected test vector name {name!r}")
                if name in files:
                    fail(f"{name} appears twice in the log")
                hex_lines = []
        elif line == "MNTV-END":
            try:
                files[name] = bytes.fromhex("".join(hex_lines))
            except ValueError:
                fail(f"{name}: not hex")
            name = None
        else:
            hex_lines.append(line)
    if name is not None:
        fail(f"{name}: no MNTV-END (did the generator crash?)")
    if not files:
        fail("no test vectors in the log (did the generator test run?)")
    return files


def find_log(path: Path) -> Path:
    """`path` itself, or the generator's handler.log somewhere under it."""
    if path.is_file():
        return path
    if path.is_dir():
        for log in sorted(path.rglob(f"{LOG_PARENT}/handler.log")):
            return log
    fail(f"no {LOG_PARENT}/handler.log at or under {path}")


def run_generator(root: Path, out: Path) -> Path:
    """Run the generator test with twister and return its log."""
    command = ["west", "twister", "-T", GENERATOR, "-p", "native_sim", "-O", str(out)]
    print(" ".join(command))
    result = subprocess.run(command, cwd=root)
    if result.returncode != 0:
        fail(f"twister exited {result.returncode}")
    return find_log(out)


def write_vectors(files: dict[str, bytes], out_dir: Path) -> None:
    """Replace the generated files in out_dir with `files`; others stay."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for kind in KINDS:
        for old in out_dir.glob(kind):
            if old.name not in files:
                old.unlink()
    for name, data in files.items():
        (out_dir / name).write_bytes(data)


def main(argv: list[str] | None = None, root: Path = ROOT) -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--log", type=Path, help="a handler.log, or a twister output dir")
    parser.add_argument("--check", action="store_true", help=f"fail if extracting changes {OUT}/")
    args = parser.parse_args(argv)

    log = find_log(args.log) if args.log else run_generator(root, root / "build" / "testvectors")
    files = parse_log(log.read_text())
    write_vectors(files, root / OUT)
    print(f"Wrote {len(files)} files to {OUT}: {', '.join(sorted(files))}")
    if args.check:
        regen.check_clean(root, OUT, "proto/testvectors.py")


if __name__ == "__main__":
    main()
