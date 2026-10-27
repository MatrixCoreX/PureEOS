#!/usr/bin/env python3
"""Source-policy tests; no chain process or socket."""
import copy
import unittest
from parallel_complex_replay import replay_source


class ComplexReplayTests(unittest.TestCase):
    def document(self):
        return {"status": "failed", "error": "Enabled eligible mode never adopted any result",
                "samples": [{"workers": 0, "round": 1, "valid": True,
                    "state": {"head_block_id": "head", "integrity_hash": "state"},
                    "checkpoint": {"block_num": 10, "lib": 8, "block_id": "head", "integrity_hash": "state"}}]}

    def test_explicit_diagnostic_does_not_relabel_failed_source(self):
        report = self.document()
        before = copy.deepcopy(report)
        with self.assertRaises(RuntimeError):
            replay_source(report)
        self.assertEqual(replay_source(report, True)["checkpoint"]["lib"], 8)
        self.assertEqual(report, before)

    def test_only_checked_serial_source_and_specific_failure_are_accepted(self):
        changes = [lambda d: d.update(status="running"), lambda d: d.update(status="interrupted"),
                   lambda d: d.update(error="state mismatch"),
                   lambda d: d["samples"][0].update(valid=False),
                   lambda d: d["samples"][0].update(workers=16),
                   lambda d: d["samples"][0].pop("checkpoint"),
                   lambda d: d["samples"][0]["state"].update(integrity_hash="bad"),
                   lambda d: d["samples"].append(copy.deepcopy(d["samples"][0]))]
        for change in changes:
            document = self.document()
            change(document)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                replay_source(document, True)

    def test_passed_source_preserves_strict_sample_validation(self):
        report = self.document()
        report.update(status="passed")
        self.assertEqual(replay_source(report)["workers"], 0)
        report["samples"][0]["valid"] = False
        with self.assertRaises(RuntimeError):
            replay_source(report, True)


if __name__ == "__main__":
    unittest.main()
