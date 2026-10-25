#!/usr/bin/env python3
"""Pure recovery-harness guards; no node process or sockets."""
import copy
from pathlib import Path
import tempfile
import unittest

from parallel_release_recovery import block_hashes, select_sample, recovery_closure_errors
from parallel_execution_test import sha256


class RecoveryHarnessTests(unittest.TestCase):
    def test_closure_preserves_missing_and_changed_evidence_as_failed_report_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);blocks=root/'blocks';blocks.mkdir()
            block=blocks/'blocks.log';block.write_bytes(b'original block')
            source=root/'source.json';source.write_bytes(b'original source')
            original=block_hashes(blocks);frozen={str(source):sha256(source)}
            self.assertEqual(recovery_closure_errors(blocks,original,frozen),[])
            block.write_bytes(b'changed block');source.write_bytes(b'changed source')
            errors=recovery_closure_errors(blocks,original,frozen)
            self.assertIn('source-blocks-changed',errors)
            self.assertIn('frozen-input-changed:'+str(source),errors)
            block.unlink();source.unlink()
            errors=recovery_closure_errors(blocks,original,frozen)
            self.assertIn('source-block-verification-RuntimeError',errors)
            self.assertIn('frozen-input-unreadable:'+str(source)+':FileNotFoundError',errors)

    def test_closure_still_refuses_linked_block_evidence_without_hiding_other_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);block=root/'block';block.write_bytes(b'block')
            blocks=root/'blocks';blocks.symlink_to(root,target_is_directory=True)
            self.assertEqual(recovery_closure_errors(blocks,{},{}),['source-block-verification-RuntimeError'])

    def document(self):
        return {"status": "passed", "samples": [{"workers": 4, "round": 1, "valid": True,
            "state": {"head_block_id": "head", "integrity_hash": "state"},
            "checkpoint": {"block_num": 10, "block_id": "head", "lib": 8, "integrity_hash": "state"}}]}

    def test_select_exact_checked_sample(self):
        self.assertEqual(select_sample(self.document(), 4, 1)["checkpoint"]["lib"], 8)

    def test_reject_failed_invalid_missing_duplicate_and_inconsistent_source(self):
        baseline = self.document()
        changes = [lambda d: d.update(status="running"),
                   lambda d: d["samples"][0].update(valid=False),
                   lambda d: d["samples"][0].pop("checkpoint"),
                   lambda d: d["samples"].append(copy.deepcopy(d["samples"][0])),
                   lambda d: d["samples"][0]["state"].update(integrity_hash="different"),
                   lambda d: d["samples"][0]["checkpoint"].update(lib=10),
                   lambda d: d["samples"][0]["checkpoint"].update(lib=0)]
        for change in changes:
            document = copy.deepcopy(baseline)
            change(document)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                select_sample(document, 4, 1)
        with self.assertRaises(RuntimeError):
            select_sample(baseline, 8, 1)

    def test_hash_copy_is_read_only_and_detects_missing_or_changed_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "blocks.log").write_bytes(b"original")
            original = block_hashes(root)
            self.assertEqual((root / "blocks.log").read_bytes(), b"original")
            (root / "blocks.log").write_bytes(b"different")
            self.assertNotEqual(block_hashes(root), original)
            (root / "blocks.log").unlink()
            with self.assertRaises(RuntimeError):
                block_hashes(root)

    def test_linked_block_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "record").write_bytes(b"test")
            (root / "link").symlink_to(root / "record")
            with self.assertRaisesRegex(RuntimeError, "linked"):
                block_hashes(root)


if __name__ == "__main__":
    unittest.main()
