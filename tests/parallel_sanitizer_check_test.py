#!/usr/bin/env python3
"""Offline negative checks; no inspected executable is ever launched."""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from parallel_sanitizer_check import check_dependencies, inspect


def dynamic(*libraries):
    return "\n".join(f" 0x0000000000000001 (NEEDED) Shared library: [{name}]" for name in libraries)


class SanitizerCheckTest(unittest.TestCase):
    def test_exact_supported_runtime_sets(self):
        self.assertEqual(check_dependencies(dynamic("libasan.so.8", "libubsan.so.1", "libc.so.6"),
                                            "address,undefined"), ["asan", "ubsan"])
        self.assertEqual(check_dependencies(dynamic("libtsan.so.2", "libstdc++.so.6"), "thread"), ["tsan"])

    def test_stale_other_mode_is_rejected_in_both_directions(self):
        for text, mode in ((dynamic("libtsan.so.2"), "address,undefined"),
                           (dynamic("libasan.so.8", "libubsan.so.1"), "thread")):
            with self.subTest(mode=mode), self.assertRaisesRegex(RuntimeError, "mismatch"):
                check_dependencies(text, mode)

    def test_missing_partial_or_mixed_runtime_sets_are_rejected(self):
        for libraries in ((), ("libc.so.6",), ("libasan.so.8",), ("libubsan.so.1",),
                          ("libasan.so.8", "libubsan.so.1", "libtsan.so.2")):
            with self.subTest(libraries=libraries), self.assertRaisesRegex(RuntimeError, "mismatch"):
                check_dependencies(dynamic(*libraries), "address,undefined")

    def test_runtime_names_outside_needed_fields_do_not_count(self):
        for text in ("libasan.so.8 libubsan.so.1", "(RUNPATH) [libasan.so.8] (RUNPATH) [libubsan.so.1]",
                     dynamic("notlibasan.so.8", "libubsan.so.1"), dynamic("libasan.so.8.fake", "libubsan.so.1")):
            with self.subTest(text=text), self.assertRaisesRegex(RuntimeError, "mismatch"):
                check_dependencies(text, "address,undefined")

    def test_unknown_mode_is_not_silently_treated_as_thread(self):
        with self.assertRaises(ValueError):
            check_dependencies(dynamic("libtsan.so.2"), "unknown")

    def test_inspection_calls_readelf_not_the_program_and_hashes_the_exact_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "program with spaces"
            path.write_bytes(b"test only; never execute")
            result = Mock(stdout=dynamic("libasan.so.8", "libubsan.so.1"))
            with patch("parallel_sanitizer_check.subprocess.run", return_value=result) as run:
                report = inspect(path, "address,undefined")
            self.assertEqual(run.call_args.args[0], ["readelf", "-d", str(path.resolve())])
            self.assertTrue(run.call_args.kwargs["check"])
            self.assertEqual(report["runtimes"], ["asan", "ubsan"])
            self.assertEqual(report["sha256"], hashlib.sha256(b"test only; never execute").hexdigest())

    def test_non_elf_failure_is_not_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "not-elf"
            path.write_bytes(b"not ELF")
            with patch("parallel_sanitizer_check.subprocess.run", side_effect=subprocess.CalledProcessError(1, "readelf")):
                with self.assertRaises(subprocess.CalledProcessError):
                    inspect(path, "thread")

    def test_replacement_during_inspection_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "changing"
            path.write_bytes(b"first")
            def replace(*args, **kwargs):
                path.write_bytes(b"second")
                return Mock(stdout=dynamic("libtsan.so.2"))
            with patch("parallel_sanitizer_check.subprocess.run", side_effect=replace):
                with self.assertRaisesRegex(RuntimeError, "changed"):
                    inspect(path, "thread")

    def test_missing_or_directory_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                inspect(Path(directory) / "absent", "thread")
            with self.assertRaisesRegex(RuntimeError, "file"):
                inspect(directory, "thread")


if __name__ == "__main__":
    unittest.main()
