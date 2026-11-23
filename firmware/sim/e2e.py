#!/usr/bin/env python3
"""End-to-end check: micnote-cli against the puck simulator.

Boots firmware/sim's zephyr.exe on an empty flash file, then drives it with
the host CLI over the simulator's pseudo-terminal:

1. pair: host_a pairs with the fresh puck (the simulator's pairing window
   is open while no host is paired, and it presses its own button). The demo
   memos were made before any pairing, on the device fallback key; pairing
   moves them to host_a.
2. a second host can't get in: its Hello is refused (no pairing window now).
3. status: host_a authenticates and the puck reports its two demo memos.
4. sync --no-ack: both decrypt with host_a's key and nothing is deleted.
5. restart the simulator on the same flash: the pairing, the memos and the
   link key survive, and the boot count has gone up.
6. sync: both are fetched, written, acked and deleted from the puck.
7. list: the puck is empty.
8. each memo came out as <id>.mnr and as <id>.opus, an Ogg Opus stream
   (the demo memos are real Opus from the firmware's encoder thread).

Standard library only. Exits non-zero with the failing step on stderr.

    python3 firmware/sim/e2e.py --sim build/sim/sim/zephyr/zephyr.exe \\
        --cli build/dev/core/bin/Debug/micnote-cli
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "proto" / "testvectors" / "vectors.json"
PTY_LINE = re.compile(r"uart_1 connected to pseudotty: (\S+)")
SERVING = "sim: serving the Micnote protocol"
BOOT_TIMEOUT_S = 30
CLI_TIMEOUT_S = 60

# What sync prints for the two demo memos firmware/sim/src/main.c records:
# 250 and 600 20 ms packets, with a marker every 100 packets.
DEMO = [
    ("250 packets, 5.0 s, 2 markers", "0000000100000001"),
    ("600 packets, 12.0 s, 5 markers", "0000000100000002"),
]


class Failure(Exception):
    pass


class Sim:
    """zephyr.exe running in `workdir`, with its stdout collected."""

    def __init__(self, exe: Path, workdir: Path, *, erase: bool) -> None:
        args = [str(exe), "-flash=flash.bin"]
        if erase:
            args.append("-flash_erase")
        self.proc = subprocess.Popen(
            args,
            cwd=workdir,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.lines: list[str] = []
        self.pty: str | None = None
        self._ready = threading.Event()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        if not self._ready.wait(BOOT_TIMEOUT_S) or self.pty is None:
            self.stop()
            raise Failure("simulator didn't start serving:\n" + self.log())

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self.lines.append(line.rstrip("\n"))
            if m := PTY_LINE.search(line):
                self.pty = m.group(1)
            if SERVING in line:
                self._ready.set()
        self._ready.set()  # exited: wake the waiter, which sees pty or not

    def log(self) -> str:
        return "\n".join("  | " + line for line in self.lines)

    def stop(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._reader.join(5)


def run_cli(cli: Path, sim: Sim, key: Path, *args: str, fails: bool = False) -> str:
    """Runs micnote-cli against the simulator; returns stdout (stdout and
    stderr if `fails`, which expects a non-zero exit)."""
    assert sim.pty is not None
    cmd = [str(cli), args[0], "--usb", sim.pty, "--key", str(key), *args[1:]]
    try:
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=CLI_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        raise Failure(f"{' '.join(cmd)}: timed out\nsimulator:\n{sim.log()}") from e
    if fails:
        if done.returncode == 0:
            raise Failure(f"{' '.join(cmd)}: should have failed\n{done.stdout}")
        print(f"$ micnote-cli {' '.join(args)}  (fails, as it should)\n{done.stderr}", end="")
        return done.stdout + done.stderr
    if done.returncode != 0:
        raise Failure(
            f"{' '.join(cmd)}: exit {done.returncode}\n{done.stdout}{done.stderr}"
            f"simulator:\n{sim.log()}"
        )
    print(f"$ micnote-cli {' '.join(args)}\n{done.stdout}", end="")
    return done.stdout


def expect(output: str, *needles: str) -> None:
    for needle in needles:
        if needle not in output:
            raise Failure(f"expected {needle!r} in:\n{output}")


def check(sim_exe: Path, cli: Path, work: Path) -> None:
    keys = json.loads(VECTORS.read_text())["keys"]
    key = work / "host_a.key"
    key.write_bytes(bytes.fromhex(keys["host_a"]["priv"]))
    out = work / "out"

    other = work / "other.key"
    other.write_bytes(bytes(range(32)))

    sim = Sim(sim_exe, work, erase=True)
    try:
        expect(
            run_cli(cli, sim, key, "pair", "--name", "e2e"), "the puck is showing:", "paired with"
        )
        if not (work / "pucks").is_dir():
            raise Failure("pair didn't store a link key")
        expect(run_cli(cli, sim, other, "status", fails=True), "hello failed")
        expect(run_cli(cli, sim, key, "hosts"), "e2e  (this host)")
        expect(run_cli(cli, sim, key, "status"), "2 recordings", "boot 1")
        listed = run_cli(cli, sim, key, "list")
        expect(listed, *(rec_id for _, rec_id in DEMO))
        synced = run_cli(cli, sim, key, "sync", "--out", str(out), "--no-ack")
        expect(synced, *(summary for summary, _ in DEMO), "kept on the puck (--no-ack)")
    finally:
        sim.stop()

    # Same flash file, no erase: the store must have survived the restart.
    sim = Sim(sim_exe, work, erase=False)
    try:
        if any("store empty" in line for line in sim.lines):
            raise Failure("restart lost the recordings:\n" + sim.log())
        expect(run_cli(cli, sim, key, "status"), "2 recordings", "boot 2")
        expect(run_cli(cli, sim, key, "list"), *(rec_id for _, rec_id in DEMO))
        expect(run_cli(cli, sim, key, "sync", "--out", str(out)), *(s for s, _ in DEMO))
        expect(run_cli(cli, sim, key, "list"), "no recordings")
    finally:
        sim.stop()

    for _, rec_id in DEMO:
        if not (out / f"{rec_id}.mnr").is_file():
            raise Failure(f"sync didn't write {rec_id}.mnr")
        opus = out / f"{rec_id}.opus"
        if not opus.is_file():
            raise Failure(f"sync didn't write {rec_id}.opus")
        head = opus.read_bytes()[:36]
        # The first page: "OggS", BOS, then the OpusHead packet.
        if head[:4] != b"OggS" or head[5] != 0x02 or head[28:36] != b"OpusHead":
            raise Failure(f"{opus.name} isn't an Ogg Opus stream")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--sim", type=Path, required=True, help="firmware/sim's zephyr.exe")
    p.add_argument("--cli", type=Path, required=True, help="micnote-cli")
    p.add_argument("--keep", type=Path, help="copy the synced files here, to listen to them")
    args = p.parse_args(argv)
    with tempfile.TemporaryDirectory(prefix="micnote-e2e-") as tmp:
        try:
            check(args.sim.resolve(), args.cli.resolve(), Path(tmp))
        except Failure as e:
            print(f"e2e: FAILED: {e}", file=sys.stderr)
            return 1
        if args.keep:
            shutil.copytree(Path(tmp) / "out", args.keep, dirs_exist_ok=True)
    print("e2e: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
