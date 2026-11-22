"""Tests for proto/testvectors.py. Standard library only:

    python3 -m unittest discover -s proto -p "test_*.py"

CI runs them under coverage.py with a 100% line and branch gate
(pyproject.toml). Twister is mocked: the generator itself is tested by
running it, in the firmware workflow.
"""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import testvectors

LOG = """\
*** Booting Zephyr ***
Running TESTSUITE testvectors
MNTV-FILE rec_a.mnr
4d4e5231
0102
MNTV-END
MNTV-FILE vectors.json
7b7d0a
MNTV-END
 PASS - test_generate
"""


class TempDirTestCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)


def failure(test: unittest.TestCase, fn, *args) -> str:
    """Run fn, expecting regen.fail(); return its message."""
    with test.assertRaises(SystemExit) as raised:
        fn(*args)
    return str(raised.exception.code)


class ParseLogTest(unittest.TestCase):
    def test_extracts_files_and_ignores_other_output(self) -> None:
        self.assertEqual(
            testvectors.parse_log(LOG),
            {"rec_a.mnr": b"MNR1\x01\x02", "vectors.json": b"{}\n"},
        )

    def test_tolerates_crlf_and_indentation(self) -> None:
        log = "  MNTV-FILE a.mnr\r\n  ab\r\n MNTV-END\r\n"
        self.assertEqual(testvectors.parse_log(log), {"a.mnr": b"\xab"})

    def test_rejects_unsafe_names(self) -> None:
        for name in ("../x.mnr", "x.bin", "X.mnr", "a/b.json", ".mnr"):
            with self.subTest(name=name):
                log = f"MNTV-FILE {name}\nMNTV-END\n"
                self.assertIn("unexpected", failure(self, testvectors.parse_log, log))

    def test_rejects_duplicates(self) -> None:
        log = "MNTV-FILE a.mnr\n00\nMNTV-END\nMNTV-FILE a.mnr\n00\nMNTV-END\n"
        self.assertIn("twice", failure(self, testvectors.parse_log, log))

    def test_rejects_bad_hex(self) -> None:
        log = "MNTV-FILE a.mnr\nzz\nMNTV-END\n"
        self.assertIn("not hex", failure(self, testvectors.parse_log, log))

    def test_rejects_unterminated_file(self) -> None:
        log = "MNTV-FILE a.mnr\n00\n"
        self.assertIn("no MNTV-END", failure(self, testvectors.parse_log, log))

    def test_rejects_empty_log(self) -> None:
        self.assertIn("no test vectors", failure(self, testvectors.parse_log, "booted\n"))


class FindLogTest(TempDirTestCase):
    def test_file_is_used_as_is(self) -> None:
        log = self.tmp / "anything.log"
        log.write_text(LOG)
        self.assertEqual(testvectors.find_log(log), log)

    def test_finds_log_under_twister_output(self) -> None:
        log = self.tmp / "native_sim_native" / "x" / "micnote.testvectors" / "handler.log"
        log.parent.mkdir(parents=True)
        log.write_text(LOG)
        # Another suite's log must not be picked up.
        other = self.tmp / "native_sim_native" / "micnote.crypto" / "handler.log"
        other.parent.mkdir(parents=True)
        other.write_text("")
        self.assertEqual(testvectors.find_log(self.tmp), log)

    def test_directory_without_log_fails(self) -> None:
        self.assertIn("no micnote.testvectors", failure(self, testvectors.find_log, self.tmp))

    def test_missing_path_fails(self) -> None:
        missing = self.tmp / "nope"
        self.assertIn("no micnote.testvectors", failure(self, testvectors.find_log, missing))


class RunGeneratorTest(TempDirTestCase):
    def run_with(self, returncode: int, write_log: bool = True):
        def fake_run(command, cwd):
            if write_log:
                out = Path(command[command.index("-O") + 1])
                log = out / "micnote.testvectors" / "handler.log"
                log.parent.mkdir(parents=True)
                log.write_text(LOG)
            return subprocess.CompletedProcess(command, returncode)

        out = self.tmp / "tv"
        with (
            mock.patch.object(testvectors.subprocess, "run", side_effect=fake_run) as run,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = testvectors.run_generator(self.tmp, out)
        return run, out, result

    def test_runs_twister_on_the_generator(self) -> None:
        run, out, log = self.run_with(0)
        command = run.call_args.args[0]
        self.assertEqual(command[:2], ["west", "twister"])
        self.assertIn("firmware/tests/testvectors", command)
        self.assertIn("native_sim", command)
        self.assertEqual(run.call_args.kwargs["cwd"], self.tmp)
        self.assertEqual(log, out / "micnote.testvectors" / "handler.log")

    def test_twister_failure_fails(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            self.run_with(1, write_log=False)
        self.assertIn("twister exited 1", str(raised.exception.code))


class WriteVectorsTest(TempDirTestCase):
    def test_writes_and_removes_stale_vectors_only(self) -> None:
        out = self.tmp / "proto" / "testvectors"
        out.mkdir(parents=True)
        (out / "old.mnr").write_bytes(b"stale")
        (out / "old.json").write_bytes(b"stale")
        (out / "a.mnr").write_bytes(b"previous")
        (out / "README.md").write_text("kept\n")
        testvectors.write_vectors({"a.mnr": b"\x00\x01", "vectors.json": b"{}"}, out)
        self.assertEqual(
            sorted(p.name for p in out.iterdir()), ["README.md", "a.mnr", "vectors.json"]
        )
        self.assertEqual((out / "a.mnr").read_bytes(), b"\x00\x01")

    def test_creates_the_directory(self) -> None:
        out = self.tmp / "new"
        testvectors.write_vectors({"a.mnr": b"x"}, out)
        self.assertEqual((out / "a.mnr").read_bytes(), b"x")


class MainTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.log = self.tmp / "handler.log"
        self.log.write_text(LOG)
        patcher = mock.patch.object(testvectors.regen, "check_clean")
        self.check_clean = patcher.start()
        self.addCleanup(patcher.stop)

    def main(self, *argv: str) -> str:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            testvectors.main(list(argv), root=self.tmp)
        return out.getvalue()

    def test_extracts_from_log(self) -> None:
        printed = self.main("--log", str(self.log))
        self.assertIn("Wrote 2 files to proto/testvectors: rec_a.mnr, vectors.json", printed)
        written = self.tmp / "proto" / "testvectors" / "rec_a.mnr"
        self.assertEqual(written.read_bytes(), b"MNR1\x01\x02")
        self.check_clean.assert_not_called()

    def test_check_runs_after_writing(self) -> None:
        self.main("--log", str(self.log), "--check")
        self.check_clean.assert_called_once_with(
            self.tmp, "proto/testvectors", "proto/testvectors.py"
        )

    def test_without_log_runs_the_generator(self) -> None:
        with mock.patch.object(testvectors, "run_generator", return_value=self.log) as gen:
            self.main()
        gen.assert_called_once_with(self.tmp, self.tmp / "build" / "testvectors")


class ScriptEntryPointTest(unittest.TestCase):
    def test_runs_as_a_script(self) -> None:
        script = Path(testvectors.__file__)
        result = subprocess.run(
            [sys.executable, str(script), "--help"], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("--check", result.stdout)


if __name__ == "__main__":
    unittest.main()
