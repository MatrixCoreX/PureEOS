#!/usr/bin/env python3
"""Offline contract checks for the isolated process harness; no nodeos spawned."""
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch
from parallel_execution_test import Node, checked_manifest, require, sha256


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

    def test_producer_fixture_flags_reject_invalid_combinations_before_creating_a_chain(self):
        script = Path(__file__).with_name("parallel_producer_process_test.py")
        required = ["--old-nodeos", str(self.root / "absent-old"), "--new-nodeos", str(self.root / "absent-new"),
                    "--fixture-exporter", str(self.root / "absent-exporter"), "--old-sha256", "0" * 64,
                    "--output-parent", str(self.root)]
        before = sorted(path.name for path in self.root.iterdir())
        for flags, message in [(["--ram-fixture"], "requires --token-fixture"),
                               (["--lifecycle-fixture"], "requires --token-fixture"),
                               (["--token-fixture", "--ram-fixture", "--lifecycle-fixture"], "cannot be combined")]:
            with self.subTest(flags=flags):
                result = subprocess.run([sys.executable, "-B", str(script), *required, *flags],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
                self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def checkpoint_node(self):
        node = object.__new__(Node)
        node.root, node.manifest = self.root, self.manifest
        point = {"block_num": 6, "block_id": "head", "lib": 4, "integrity_hash": "state"}
        info = {"head_block_num": 6, "head_block_id": "head", "last_irreversible_block_num": 4,
                "chain_id": self.manifest["chain_id"]}
        state = {"head_block_id": "head", "integrity_hash": "state"}
        return node, point, info, state

    def test_checkpoint_waits_for_exact_finality_after_head_cache(self):
        node, point, info, state = self.checkpoint_node()
        node.rpc = Mock(side_effect=[{**info, "last_irreversible_block_num": 3}, info, state])
        with patch("parallel_execution_test.time.sleep") as sleep:
            self.assertEqual(node.check(point)["lib"], 4)
            sleep.assert_called_once_with(0.1)

    def test_checkpoint_wrong_finality_still_fails_at_deadline(self):
        node, point, info, _ = self.checkpoint_node()
        for incorrect in (3, 5):
            node.rpc = Mock(return_value={**info, "last_irreversible_block_num": incorrect})
            with patch("parallel_execution_test.time.monotonic", side_effect=[0, 31]):
                with self.assertRaisesRegex(RuntimeError, f"actual={incorrect}, expected=4"):
                    node.check(point)


if __name__ == "__main__":
    unittest.main()
