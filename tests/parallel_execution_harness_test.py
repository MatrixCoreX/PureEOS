#!/usr/bin/env python3
"""Offline contract checks for the isolated process harness; no nodeos spawned."""
import json
from pathlib import Path
import tempfile
import unittest
from parallel_execution_test import checked_manifest, require, sha256


class HarnessTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="parallel-harness-unit-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = {"format": 1, "chain_id": "0" * 64, "code_hashes": ["a" * 64],
                         "checkpoints": [{"block_num": 2, "file": "block-2.json"}]}
        (self.root / "block-2.json").write_text("{}")

    def load(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest))
        return checked_manifest(self.root)

    def test_contiguous_input_and_streamed_hash(self):
        self.assertEqual(self.load(), self.manifest)
        self.assertEqual(sha256(self.root / "block-2.json"),
                         "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a")

    def test_path_traversal_and_absolute_path_rejected(self):
        for name in ("../genesis.json", "/tmp/block-2.json", "blocks/../block-2.json"):
            with self.subTest(name=name):
                self.manifest["checkpoints"][0]["file"] = name
                with self.assertRaisesRegex(RuntimeError, "filename"):
                    self.load()

    def test_gap_rejected(self):
        self.manifest["checkpoints"][0]["block_num"] = 3
        with self.assertRaisesRegex(RuntimeError, "contiguous"):
            self.load()

    def test_empty_rejected(self):
        self.manifest["checkpoints"] = []
        with self.assertRaisesRegex(RuntimeError, "Empty"):
            self.load()

    def test_missing_block_rejected(self):
        (self.root / "block-2.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing"):
            self.load()

    def test_unknown_format_rejected(self):
        self.manifest["format"] = 2
        with self.assertRaisesRegex(RuntimeError, "format"):
            self.load()

    def test_bad_digest_rejected(self):
        self.manifest["code_hashes"] = ["g" * 64]
        with self.assertRaisesRegex(RuntimeError, "digest"):
            self.load()

    def test_fork_path_is_confined(self):
        self.manifest["forks"] = [{"file": "../fork-0.json"}]
        with self.assertRaisesRegex(RuntimeError, "fork filename"):
            self.load()

    def test_fork_requires_a_real_block_file(self):
        self.manifest["forks"] = [{"file": "fork-0.json"}]
        with self.assertRaisesRegex(RuntimeError, "Missing fork"):
            self.load()
        (self.root / "fork-0.json").write_text("{}")
        self.assertEqual(self.load(), self.manifest)

    def test_checks_cannot_be_optimized_away(self):
        with self.assertRaisesRegex(RuntimeError, "must fail"):
            require(False, "must fail")


if __name__ == "__main__":
    unittest.main()
