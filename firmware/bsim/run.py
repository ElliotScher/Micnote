#!/usr/bin/env python3
"""BLE end to end in BabbleSim: the puck, a host and a stranger on one
simulated 2.4 GHz channel.

Runs the image firmware/bsim/link builds (one executable, three roles chosen
by -testid) as three devices against BabbleSim's PHY, and fails unless every
device's test passes. See README.md for what each role checks.

    python3 firmware/bsim/run.py                build, then run
    python3 firmware/bsim/run.py --no-build     run the last build
    python3 firmware/bsim/run.py --keep DIR     also copy the device logs to DIR

Needs BSIM_OUT_PATH and BSIM_COMPONENTS_PATH (README.md), and the firmware
environment (west) to build. Standard library only.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "firmware" / "bsim" / "link"
BUILD = ROOT / "build" / "bsim-link"
EXE = BUILD / "link" / "zephyr" / "zephyr.exe"
BOARD = "nrf52_bsim"
ROLES = ("puck", "host", "stranger")
# Simulated time; the whole scenario takes about 45 s of it.
SIM_LENGTH_US = 120_000_000
WALL_TIMEOUT_S = 600


class Failure(Exception):
    pass


def bsim_out() -> Path:
    out = os.environ.get("BSIM_OUT_PATH")
    if not out or not os.environ.get("BSIM_COMPONENTS_PATH"):
        raise Failure("set BSIM_OUT_PATH and BSIM_COMPONENTS_PATH (see firmware/bsim/README.md)")
    phy = Path(out) / "bin" / "bs_2G4_phy_v1"
    if not phy.is_file():
        raise Failure(f"{phy} missing: build BabbleSim (firmware/bsim/README.md)")
    return Path(out)


def build() -> None:
    cmd = ["west", "build", "-b", BOARD, str(APP), "-d", str(BUILD)]
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        raise Failure("build failed")


def run(logs: Path) -> None:
    out = bsim_out()
    if not EXE.is_file():
        raise Failure(f"{EXE} missing: build first")
    sim_id = f"micnote_{os.getpid()}"
    bin_dir = out / "bin"
    procs = []
    for d, role in enumerate(ROLES):
        log = (logs / f"{role}.log").open("w")
        procs.append(
            (
                role,
                log,
                subprocess.Popen(
                    [str(EXE), f"-s={sim_id}", f"-d={d}", f"-testid={role}", f"-rs={23 + d * 7}"],
                    cwd=bin_dir,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                ),
            )
        )
    phy = subprocess.Popen(
        ["./bs_2G4_phy_v1", f"-s={sim_id}", f"-D={len(ROLES)}", f"-sim_length={SIM_LENGTH_US}"],
        cwd=bin_dir,
        stdout=(logs / "phy.log").open("w"),
        stderr=subprocess.STDOUT,
    )
    failed = []
    try:
        for role, log, proc in procs:
            code = proc.wait(timeout=WALL_TIMEOUT_S)
            log.close()
            text = (logs / f"{role}.log").read_text(errors="replace")
            verdict = next((ln for ln in text.splitlines() if "Test end:" in ln), "")
            print(f"{role:9} exit {code}  {verdict.split('INFO: ')[-1] or '(no verdict)'}")
            # bst exits 0 only when the role called TEST_PASS.
            if code != 0:
                failed.append(role)
        phy.wait(timeout=WALL_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        raise Failure("the simulation didn't finish in time") from e
    finally:
        for _, _, proc in procs:
            proc.kill()
        phy.kill()
    if failed:
        for role in failed:
            text = (logs / f"{role}.log").read_text(errors="replace")
            print(f"\n--- {role} (last 40 lines) ---\n" + "\n".join(text.splitlines()[-40:]))
        raise Failure(f"failed: {', '.join(failed)}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--no-build", action="store_true", help="run the existing build")
    p.add_argument("--keep", type=Path, help="copy the device logs here")
    args = p.parse_args(argv)
    try:
        if not args.no_build:
            build()
        with tempfile.TemporaryDirectory(prefix="micnote-bsim-") as tmp:
            try:
                run(Path(tmp))
            finally:
                if args.keep:
                    shutil.copytree(tmp, args.keep, dirs_exist_ok=True)
    except Failure as e:
        print(f"bsim: FAILED: {e}", file=sys.stderr)
        return 1
    print("bsim: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
