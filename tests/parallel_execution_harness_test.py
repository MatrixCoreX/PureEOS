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
                               (["--token-fixture", "--ram-fixture", "--lifecycle-fixture"], "cannot be combined"),
                               (["--multi-action-fixture"], "requires --token-fixture"),
                               (["--token-fixture", "--ram-fixture", "--multi-action-fixture"], "cannot be combined"),
                               (["--token-fixture", "--lifecycle-fixture", "--multi-action-fixture"], "cannot be combined"),
                               (["--token-fixture", "--coded-notification-fixture"], "cannot be combined"),
                               (["--token-fixture", "--inline-fixture"], "cannot be combined"),
                               (["--coded-notification-fixture", "--inline-fixture"], "cannot be combined")]:
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

    def test_multi_action_trace_checks_require_all_roots_and_exact_ram_refund(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_multi_action_response
        names = ["transfer", "transfer", "close"]
        deltas = [{"alice": 240}, {"alice": -128, "blank": 128}, {"alice": -112, "blank": -128}]
        roots = [{"creator_action_ordinal": 0, "act": {"name": name},
                  "account_ram_deltas": [{"account": account, "delta": delta} for account, delta in ram.items()]}
                 for name, ram in zip(names, deltas)]
        response = {"processed": {"action_traces": roots}}
        self.assertEqual(check_multi_action_response({"actions": 3}, response, True), {"alice": 0, "blank": 0})
        response["processed"]["action_traces"].append({"creator_action_ordinal": 1})
        self.assertEqual(check_multi_action_response({"actions": 3}, response, True), {"alice": 0, "blank": 0})
        for mutation, message in [(0, "Lost a top-level"), (1, "action sequence"), (2, "did not create"), (3, "refund")]:
            changed = deepcopy(response)
            traces = changed["processed"]["action_traces"]
            if mutation == 0:
                traces[1]["creator_action_ordinal"] = 1
            elif mutation == 1:
                traces[2]["act"]["name"] = "open"
            elif mutation == 2:
                traces[0]["account_ram_deltas"] = []
            else:
                traces[2]["account_ram_deltas"][0]["delta"] += 1
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, message):
                check_multi_action_response({"actions": 3}, changed, True)

    def test_checkpoint_waits_for_exact_finality_after_head_cache(self):
        node, point, info, state = self.checkpoint_node()
        node.rpc = Mock(side_effect=[{**info, "last_irreversible_block_num": 3}, info, state])
        with patch("parallel_execution_test.time.sleep") as sleep:
            self.assertEqual(node.check(point)["lib"], 4)
            sleep.assert_called_once_with(0.1)

    def test_coded_notification_trace_checks_bind_receivers_ancestry_and_execution_order(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_coded_notification_response
        receivers = ["test", "test", "alice", "bob", "carol", "alice", "bob", "carol"]
        creators = [0, 0, 1, 1, 3, 2, 2, 6]
        ancestors = [0, 0, 1, 1, 1, 2, 2, 2]
        order = {ordinal: i + 10 for i, ordinal in enumerate((1, 3, 4, 5, 2, 6, 7, 8))}
        traces = [{"action_ordinal": ordinal, "creator_action_ordinal": creator,
                   "closest_unnotified_ancestor_action_ordinal": ancestor,
                   "receipt": {"receiver": receiver, "global_sequence": order[ordinal]},
                   "act": {"account": "test", "name": "run", "data": ancestor or ordinal}}
                  for ordinal, receiver, creator, ancestor in zip(range(1, 9), receivers, creators, ancestors)]
        response = {"processed": {"action_traces": traces}}
        check_coded_notification_response({"actions": 2}, response)
        nested = deepcopy(traces)
        for index in (0, 1):
            start = 2 + 3 * index
            nested[index]["inline_traces"] = nested[start:start + 2]
            nested[start]["inline_traces"] = [nested[start + 2]]
        tree_response = {"processed": {"action_traces": nested[:2]}}
        check_coded_notification_response({"actions": 2}, tree_response)
        missing_child = deepcopy(tree_response)
        missing_child["processed"]["action_traces"][0]["inline_traces"][0]["inline_traces"].clear()
        with self.assertRaisesRegex(RuntimeError, "lost"):
            check_coded_notification_response({"actions": 2}, missing_child)
        cycle = deepcopy(tree_response)
        cycle["processed"]["action_traces"][0]["inline_traces"].append(cycle["processed"]["action_traces"][0])
        with self.assertRaisesRegex(RuntimeError, "bounds"):
            check_coded_notification_response({"actions": 2}, cycle)
        for mutation, message in [(0, "lost"), (1, "ordinals"), (2, "receiver"), (3, "ancestry"),
                                  (4, "original action"), (5, "execution order")]:
            changed = deepcopy(response)
            entries = changed["processed"]["action_traces"]
            if mutation == 0:
                entries.pop()
            elif mutation == 1:
                entries[2]["action_ordinal"] = 1
            elif mutation == 2:
                entries[2]["receipt"]["receiver"] = "bob"
            elif mutation == 3:
                entries[4]["creator_action_ordinal"] = 1
            elif mutation == 4:
                entries[4]["act"]["data"] = 99
            else:
                entries[1]["receipt"]["global_sequence"] = entries[0]["receipt"]["global_sequence"]
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, message):
                check_coded_notification_response({"actions": 2}, changed)

    def test_inline_trace_checks_bind_original_sends_authority_and_nonordinal_execution(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_inline_response
        receivers = ["test", "test", "carol", "bob", "blank", "bob", "carol", "carol", "blank",
                     "carol", "bob", "blank", "bob", "carol", "carol", "blank"]
        creators = [0, 0, 1, 1, 1, 1, 4, 4, 8, 2, 2, 2, 2, 11, 11, 15]
        methods = ["run", "run", "run", "child", "run", "sibling", "child", "grand", "grand",
                   "run", "child", "run", "sibling", "child", "grand", "grand"]
        order = {ordinal: i + 100 for i, ordinal in enumerate((1, 3, 5, 4, 7, 8, 9, 6, 2, 10, 12, 11, 14, 15, 16, 13))}
        traces = []
        for ordinal, receiver, creator, method in zip(range(1, 17), receivers, creators, methods):
            account = "test" if method == "run" else "carol" if method == "grand" else "bob"
            actor = "alice" if method == "run" else account
            traces.append({"action_ordinal": ordinal, "creator_action_ordinal": creator,
                "closest_unnotified_ancestor_action_ordinal": creator,
                "receipt": {"receiver": receiver, "global_sequence": order[ordinal]},
                "act": {"account": account, "name": method, "data": 2 if ordinal == 2 or ordinal >= 10 else 1,
                        "authorization": [{"actor": actor, "permission": "active"}]}})
        response = {"processed": {"action_traces": traces}}
        check_inline_response({"actions": 2}, response)
        nested = deepcopy(traces)
        for trace in nested:
            creator = trace["creator_action_ordinal"]
            if creator:
                nested[creator - 1].setdefault("inline_traces", []).append(trace)
        tree = {"processed": {"action_traces": nested[:2]}}
        check_inline_response({"actions": 2}, tree)
        cycle = deepcopy(tree)
        cycle["processed"]["action_traces"][0]["inline_traces"].append(cycle["processed"]["action_traces"][0])
        with self.assertRaisesRegex(RuntimeError, "bounds"):
            check_inline_response({"actions": 2}, cycle)
        for mutation, message in enumerate(("lost", "ordinals", "receiver", "ancestry", "target", "authorization", "payload", "execution order")):
            changed = deepcopy(response); entries = changed["processed"]["action_traces"]
            if mutation == 0:
                entries.pop()
            elif mutation == 1:
                entries[3]["action_ordinal"] = 1
            elif mutation == 2:
                entries[3]["receipt"]["receiver"] = "alice"
            elif mutation == 3:
                entries[7]["closest_unnotified_ancestor_action_ordinal"] = 1
            elif mutation == 4:
                entries[3]["act"]["account"] = "test"
            elif mutation == 5:
                entries[3]["act"]["authorization"][0]["actor"] = "alice"
            elif mutation == 6:
                entries[3]["act"]["data"] = 2
            else:
                entries[5]["receipt"]["global_sequence"] = entries[3]["receipt"]["global_sequence"]
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, message):
                check_inline_response({"actions": 2}, changed)

    def test_checkpoint_wrong_finality_still_fails_at_deadline(self):
        node, point, info, _ = self.checkpoint_node()
        for incorrect in (3, 5):
            node.rpc = Mock(return_value={**info, "last_irreversible_block_num": incorrect})
            with patch("parallel_execution_test.time.monotonic", side_effect=[0, 31]):
                with self.assertRaisesRegex(RuntimeError, f"actual={incorrect}, expected=4"):
                    node.check(point)


if __name__ == "__main__":
    unittest.main()
