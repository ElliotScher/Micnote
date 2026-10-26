"""Tests for proto/regen.py, one class per step. Standard library only:

    python3 -m unittest discover -s proto -p "test_*.py"

CI runs them on Linux and Windows under coverage.py with a 100% line and
branch gate (pyproject.toml). Only pip/uv installs are mocked: generation
runs a real subprocess (a stand-in nanopb module) and the git checks run
against a real throwaway repository.
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

import regen

FAKE_GENERATOR = textwrap.dedent(
    """\
    # Stand-in for nanopb_generator: writes micnote.pb.{c,h} into the -D
    # directory, recording the working directory it ran in, or fails when
    # FAKE_NANOPB_EXIT is set.
    import os, sys
    if os.environ.get("FAKE_NANOPB_EXIT"):
        sys.exit(int(os.environ["FAKE_NANOPB_EXIT"]))
    out = sys.argv[sys.argv.index("-D") + 1]
    for name in ("micnote.pb.c", "micnote.pb.h"):
        with open(os.path.join(out, name), "wb") as f:
            f.write(("generated in " + os.getcwd() + "\\n").encode())
    """
)


class TempDirTestCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)


class ReadPinsTest(TempDirTestCase):
    def test_reads_pins_and_skips_comments(self) -> None:
        env = self.tmp / "versions.env"
        env.write_text(
            "# a comment line\n"
            "\n"
            "NANOPB_VERSION=0.4.9.1\n"
            "  GRPCIO_TOOLS_VERSION=1.84.0   # trailing comment\n"
            "EMPTY=\n"
            "not a pin\n"
        )
        self.assertEqual(
            regen.read_pins(env),
            {"NANOPB_VERSION": "0.4.9.1", "GRPCIO_TOOLS_VERSION": "1.84.0", "EMPTY": ""},
        )

    def test_reads_the_real_file(self) -> None:
        pins = regen.read_pins(regen.ROOT / "tools" / "versions.env")
        self.assertIn("NANOPB_VERSION", pins)
        self.assertIn("GRPCIO_TOOLS_VERSION", pins)


class VenvPythonTest(unittest.TestCase):
    def test_posix_layout(self) -> None:
        self.assertEqual(regen.venv_python(Path("v"), windows=False), Path("v") / "bin" / "python")

    def test_windows_layout(self) -> None:
        self.assertEqual(
            regen.venv_python(Path("v"), windows=True), Path("v") / "Scripts" / "python.exe"
        )


class EnsureVenvTest(TempDirTestCase):
    REQS = ("nanopb[grpcio-tools]==1", "grpcio-tools==2")

    def setUp(self) -> None:
        super().setUp()
        self.venv = self.tmp / "venv"
        self.calls: list[list[str]] = []

    def fake_run(self, cmd: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Records each command; "creates" the venv when asked to."""
        self.calls.append([str(c) for c in cmd])
        if "venv" in cmd and "pip" not in cmd:
            self.venv.mkdir(parents=True)
        return subprocess.CompletedProcess(cmd, 0)

    def ensure(self, uv: str | None) -> Path:
        with (
            mock.patch.object(regen.shutil, "which", return_value=uv),
            mock.patch.object(regen.subprocess, "run", side_effect=self.fake_run),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            return regen.ensure_venv(self.venv, list(self.REQS), ".stamp-1-2")

    def test_reuses_venv_with_matching_stamp(self) -> None:
        self.venv.mkdir()
        (self.venv / ".stamp-1-2").touch()
        self.assertEqual(self.ensure(uv="/bin/uv"), regen.venv_python(self.venv))
        self.assertEqual(self.calls, [])

    def test_creates_with_uv(self) -> None:
        python = str(regen.venv_python(self.venv))
        self.ensure(uv="/bin/uv")
        self.assertEqual(
            self.calls,
            [
                ["/bin/uv", "venv", "--quiet", str(self.venv)],
                ["/bin/uv", "pip", "install", "--quiet", "--python", python, *self.REQS],
            ],
        )
        self.assertTrue((self.venv / ".stamp-1-2").exists())

    def test_creates_with_venv_and_pip_without_uv(self) -> None:
        python = str(regen.venv_python(self.venv))
        self.ensure(uv=None)
        self.assertEqual(
            self.calls,
            [
                [sys.executable, "-m", "venv", str(self.venv)],
                [
                    python,
                    *("-m", "pip", "install", "--quiet", "--disable-pip-version-check"),
                    *self.REQS,
                ],
            ],
        )
        self.assertTrue((self.venv / ".stamp-1-2").exists())

    def test_rebuilds_stale_venv_from_scratch(self) -> None:
        self.venv.mkdir()
        (self.venv / ".stamp-old").touch()
        (self.venv / "leftover").touch()
        self.ensure(uv="/bin/uv")
        self.assertFalse((self.venv / "leftover").exists())
        self.assertFalse((self.venv / ".stamp-old").exists())
        self.assertTrue((self.venv / ".stamp-1-2").exists())

    def test_failed_install_leaves_no_stamp(self) -> None:
        def run(cmd: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            if "install" in cmd:
                raise subprocess.CalledProcessError(1, cmd)
            return self.fake_run(cmd, **kwargs)

        with (
            mock.patch.object(regen.shutil, "which", return_value="/bin/uv"),
            mock.patch.object(regen.subprocess, "run", side_effect=run),
            contextlib.redirect_stdout(io.StringIO()),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            regen.ensure_venv(self.venv, list(self.REQS), ".stamp-1-2")
        # No stamp, so the next run retries instead of trusting a broken venv.
        self.assertFalse((self.venv / ".stamp-1-2").exists())


class GenerateTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        fake = self.tmp / "fake" / "nanopb" / "generator"
        fake.mkdir(parents=True)
        (fake.parent / "__init__.py").touch()
        (fake / "__init__.py").touch()
        (fake / "nanopb_generator.py").write_text(FAKE_GENERATOR)
        env = mock.patch.dict(os.environ, {"PYTHONPATH": str(self.tmp / "fake")})
        env.start()
        self.addCleanup(env.stop)

        self.proto = self.tmp / "proto"
        self.gen = self.proto / "gen"
        self.gen.mkdir(parents=True)
        (self.gen / "micnote.pb.h").write_bytes(b"old header\n")
        (self.gen / "stale.pb.c").write_bytes(b"from a removed .proto\n")
        (self.gen / ".clang-format").write_bytes(b"DisableFormat: true\n")

    def generate(self) -> None:
        regen.generate(Path(sys.executable), self.proto, self.gen)

    def test_replaces_generated_files(self) -> None:
        self.generate()
        names = sorted(p.name for p in self.gen.iterdir())
        self.assertEqual(names, [".clang-format", "micnote.pb.c", "micnote.pb.h"])
        for name in ("micnote.pb.c", "micnote.pb.h"):
            data = (self.gen / name).read_bytes()
            # Bytes kept as written (LF, no CRLF conversion on Windows).
            self.assertTrue(data.startswith(b"generated in ") and data.endswith(b"\n"))
            self.assertNotIn(b"\r", data)
            # Ran from proto/, so nanopb writes relative paths. samefile, not
            # string equality: Windows may report the temp dir in 8.3 form.
            cwd = data.removeprefix(b"generated in ").removesuffix(b"\n").decode()
            self.assertTrue(Path(cwd).samefile(self.proto))

    def test_failure_leaves_gen_untouched(self) -> None:
        before = {p.name: p.read_bytes() for p in self.gen.iterdir()}
        with (
            mock.patch.dict(os.environ, {"FAKE_NANOPB_EXIT": "3"}),
            self.assertRaises(SystemExit) as raised,
        ):
            self.generate()
        self.assertEqual(raised.exception.code, "error: nanopb_generator exited 3")
        self.assertEqual({p.name: p.read_bytes() for p in self.gen.iterdir()}, before)


class CheckCleanTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.git("init", "-q")
        (self.tmp / "proto" / "gen").mkdir(parents=True)
        (self.tmp / "proto" / "gen" / "micnote.pb.h").write_text("v1\n")
        (self.tmp / "README").write_text("unrelated\n")
        self.git("add", "-A")
        # Explicit identity and no signing: the runner's (or your) git config
        # may lack one or require the other.
        self.git(
            *("-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"),
            *("commit", "-qm", "init"),
        )

    def git(self, *args: str) -> None:
        subprocess.run(["git", "-C", str(self.tmp), *args], check=True)

    def check(self) -> str:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            regen.check_clean(self.tmp)
        return out.getvalue()

    def assert_out_of_date(self) -> None:
        with (
            self.assertRaises(SystemExit) as raised,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            regen.check_clean(self.tmp)
        self.assertIn("proto/gen is out of date", str(raised.exception.code))

    def test_clean_checkout_passes(self) -> None:
        self.assertEqual(self.check(), "proto/gen is up to date\n")

    def test_changes_outside_gen_are_ignored(self) -> None:
        (self.tmp / "README").write_text("edited\n")
        self.assertEqual(self.check(), "proto/gen is up to date\n")

    def test_modified_file_fails(self) -> None:
        (self.tmp / "proto" / "gen" / "micnote.pb.h").write_text("v2\n")
        self.assert_out_of_date()

    def test_new_untracked_file_fails(self) -> None:
        (self.tmp / "proto" / "gen" / "extra.pb.c").write_text("new\n")
        self.assert_out_of_date()


class MainTest(TempDirTestCase):
    def setUp(self) -> None:
        super().setUp()
        (self.tmp / "tools").mkdir()
        self.write_pins("NANOPB_VERSION=0.4.9.1\nGRPCIO_TOOLS_VERSION=1.84.0\n")
        self.python = Path("venv-python")
        patches = {
            "ensure_venv": mock.patch.object(regen, "ensure_venv", return_value=self.python),
            "generate": mock.patch.object(regen, "generate"),
            "check_clean": mock.patch.object(regen, "check_clean"),
        }
        self.mocks = {name: p.start() for name, p in patches.items()}
        for p in patches.values():
            self.addCleanup(p.stop)

    def write_pins(self, text: str) -> None:
        (self.tmp / "tools" / "versions.env").write_text(text)

    def main(self, *argv: str) -> str:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            regen.main(list(argv), root=self.tmp)
        return out.getvalue()

    def test_regenerates_with_pinned_versions(self) -> None:
        self.assertEqual(self.main(), "Regenerated proto/gen with nanopb 0.4.9.1\n")
        self.mocks["ensure_venv"].assert_called_once_with(
            self.tmp / "build" / ".nanopb-venv",
            ["nanopb[grpcio-tools]==0.4.9.1", "grpcio-tools==1.84.0"],
            stamp_name=".nanopb-0.4.9.1-grpcio-tools-1.84.0",
        )
        self.mocks["generate"].assert_called_once_with(
            self.python, self.tmp / "proto", self.tmp / "proto" / "gen"
        )
        self.mocks["check_clean"].assert_not_called()

    def test_check_runs_git_check_after_generating(self) -> None:
        order = mock.Mock()
        order.attach_mock(self.mocks["generate"], "generate")
        order.attach_mock(self.mocks["check_clean"], "check_clean")
        self.main("--check")
        self.assertEqual([c[0] for c in order.mock_calls], ["generate", "check_clean"])
        self.mocks["check_clean"].assert_called_once_with(self.tmp)

    def test_missing_pin_fails_before_installing(self) -> None:
        for pins in ("NANOPB_VERSION=0.4.9.1\n", "GRPCIO_TOOLS_VERSION=1.84.0\n"):
            with self.subTest(pins=pins):
                self.write_pins(pins)
                with self.assertRaises(SystemExit) as raised:
                    self.main()
                self.assertIn("missing from tools/versions.env", str(raised.exception.code))
        self.mocks["ensure_venv"].assert_not_called()

    def test_rejects_unknown_arguments(self) -> None:
        with (
            self.assertRaises(SystemExit) as raised,
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.main("--bogus")
        self.assertEqual(raised.exception.code, 2)


class ScriptEntryPointTest(unittest.TestCase):
    def test_runs_as_a_script(self) -> None:
        script = regen.ROOT / "proto" / "regen.py"
        result = subprocess.run(
            [sys.executable, str(script), "--help"], capture_output=True, text=True, check=True
        )
        self.assertIn("--check", result.stdout)


if __name__ == "__main__":
    unittest.main()
