#!/usr/bin/env python3
"""Regenerate proto/gen/ from micnote.proto with the pinned nanopb generator.

The generator and its protoc (grpcio-tools) come from tools/versions.env and
run from a throwaway venv in build/.nanopb-venv; nothing is installed
system-wide. Standard library only, so any Python >= PYTHON_MIN runs it:

    python3 proto/regen.py          regenerate proto/gen/
    python3 proto/regen.py --check  regenerate, then fail if proto/gen/ changed

On Windows, use `py proto\\regen.py`. Tests: proto/test_regen.py.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent


def fail(message: str) -> NoReturn:
    sys.exit(f"error: {message}")


def read_pins(versions_env: Path) -> dict[str, str]:
    """KEY=value pairs from tools/versions.env, as the shell scripts read it."""
    pins = {}
    for line in versions_env.read_text().splitlines():
        if m := re.match(r"\s*([A-Z0-9_]+)=([^\s#]*)", line):
            pins[m[1]] = m[2]
    return pins


def venv_python(venv: Path, windows: bool = os.name == "nt") -> Path:
    if windows:
        return venv / "Scripts" / "python.exe"
    return venv / "bin" / "python"


def ensure_venv(venv: Path, requirements: list[str], stamp_name: str) -> Path:
    """Create `venv` with `requirements` unless it already has exactly them.

    `stamp_name` names the pinned versions; a venv without that stamp file is
    stale (or half-built) and is recreated from scratch.
    """
    python = venv_python(venv)
    stamp = venv / stamp_name
    if stamp.exists():
        return python

    print(f"Creating {venv} with {' '.join(requirements)}")
    shutil.rmtree(venv, ignore_errors=True)
    if uv := shutil.which("uv"):
        subprocess.run([uv, "venv", "--quiet", str(venv)], check=True)
        install = [uv, "pip", "install", "--quiet", "--python", str(python)]
    else:
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
        install = [str(python), "-m", "pip", "install", "--quiet", "--disable-pip-version-check"]
    subprocess.run([*install, *requirements], check=True)
    stamp.touch()
    return python


def generate(python: Path, proto_dir: Path, gen_dir: Path) -> None:
    """Run nanopb into a scratch directory, then swap the result into gen_dir.

    A failed run leaves gen_dir as it was. Running from proto_dir keeps the
    paths nanopb writes into the generated files relative, so output is
    identical on every machine.
    """
    with tempfile.TemporaryDirectory() as out:
        generator = [str(python), "-m", "nanopb.generator.nanopb_generator"]
        result = subprocess.run(
            [*generator, "-q", "-I", ".", "-D", out, "micnote.proto"], cwd=proto_dir
        )
        if result.returncode != 0:
            fail(f"nanopb_generator exited {result.returncode}")
        for old in gen_dir.glob("*.pb.[ch]"):
            old.unlink()
        for new in Path(out).glob("*.pb.[ch]"):
            # Write bytes as-is: nanopb emits LF, and .gitattributes keeps
            # the checkout LF too, so --check compares like with like.
            (gen_dir / new.name).write_bytes(new.read_bytes())


def check_clean(root: Path) -> None:
    """Fail unless proto/gen in the git checkout at `root` matches HEAD."""
    git = ["git", "-C", str(root)]
    changed = subprocess.run([*git, "diff", "--exit-code", "--", "proto/gen"]).returncode != 0
    untracked = subprocess.run(
        [*git, "ls-files", "--others", "--exclude-standard", "--", "proto/gen"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    if changed or untracked:
        fail("proto/gen is out of date: run proto/regen.py and commit the result")
    print("proto/gen is up to date")


def main(argv: list[str] | None = None, root: Path = ROOT) -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check", action="store_true", help="fail if regenerating changes proto/gen/"
    )
    args = parser.parse_args(argv)

    pins = read_pins(root / "tools" / "versions.env")
    nanopb = pins.get("NANOPB_VERSION")
    grpcio_tools = pins.get("GRPCIO_TOOLS_VERSION")
    if not nanopb or not grpcio_tools:
        fail("NANOPB_VERSION or GRPCIO_TOOLS_VERSION missing from tools/versions.env")

    # nanopb's grpcio-tools extra brings its own protoc. Where that wheel
    # can't load (NixOS: no libstdc++ on the default library path), nanopb
    # falls back to a protoc on PATH; both produce the same output.
    python = ensure_venv(
        root / "build" / ".nanopb-venv",
        [f"nanopb[grpcio-tools]=={nanopb}", f"grpcio-tools=={grpcio_tools}"],
        stamp_name=f".nanopb-{nanopb}-grpcio-tools-{grpcio_tools}",
    )
    generate(python, root / "proto", root / "proto" / "gen")
    print(f"Regenerated proto/gen with nanopb {nanopb}")
    if args.check:
        check_clean(root)


if __name__ == "__main__":
    main()
