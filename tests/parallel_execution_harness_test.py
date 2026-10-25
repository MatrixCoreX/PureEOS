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
                               (["--coded-notification-fixture", "--inline-fixture"], "cannot be combined"),
                               (["--token-fixture", "--context-free-fixture"], "cannot be combined"),
                               (["--inline-fixture", "--context-free-fixture"], "cannot be combined"),
                               (["--coded-notification-fixture", "--context-free-fixture"], "cannot be combined"),
                               (["--token-fixture", "--sync-fixture"], "cannot be combined"),
                               (["--inline-fixture", "--sync-fixture"], "cannot be combined"),
                               (["--context-free-fixture", "--sync-fixture"], "cannot be combined"),
                               (["--coded-notification-fixture", "--sync-fixture"], "cannot be combined"),
                               (["--sync-fixture", "--sync-multi-root-fixture"], "cannot be combined"),
                               (["--token-fixture", "--sync-multi-root-fixture"], "cannot be combined"),
                               (["--inline-fixture", "--sync-multi-root-fixture"], "cannot be combined"),
                               (["--coded-notification-fixture", "--sync-multi-root-fixture"], "cannot be combined"),
                               (["--context-free-fixture", "--sync-multi-root-fixture"], "cannot be combined"),
                               *[(["--mixed-fixture", flag], "cannot be combined") for flag in
                                 ("--token-fixture", "--coded-notification-fixture", "--inline-fixture",
                                  "--context-free-fixture", "--sync-fixture", "--sync-multi-root-fixture")]]:
            with self.subTest(flags=flags):
                result = subprocess.run([sys.executable, "-B", str(script), *required, *flags],
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)
                self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def test_sync_production_checks_require_exact_adoption_and_conflict_boundaries(self):
        from parallel_producer_process_test import check_sync_production_totals
        valid = [["2", "80", "16", "4", "12"]]
        check_sync_production_totals(valid)
        check_sync_production_totals([["2", "40", "16", "4", "12"]])
        for totals in ([], valid + valid, [["2", "80"]]):
            with self.subTest(totals=totals), self.assertRaisesRegex(RuntimeError, "totals"):
                check_sync_production_totals(totals)
        for counts, message in (((2, 80, 16, 3, 13), "four sync"), ((2, 80, 16, 5, 11), "four sync"),
                                ((1, 80, 16, 4, 12), "four sync"), ((2, 80, 16, 4, 11), "four sync"),
                                ((2, 3, 4, 4, 12), "counters"), ((2, 8, 3, 4, 12), "counters")):
            with self.subTest(counts=counts), self.assertRaisesRegex(RuntimeError, message):
                check_sync_production_totals([list(map(str, counts))])

    def test_mixed_production_totals_bind_two_shared_row_batches(self):
        from parallel_producer_process_test import check_mixed_production_totals
        valid = [["2", "160", "16", "2", "14"]]
        check_mixed_production_totals(valid)
        for totals in ([], valid + valid, [["2", "160"]]):
            with self.subTest(totals=totals), self.assertRaisesRegex(RuntimeError, "totals"):
                check_mixed_production_totals(totals)
        for row in ((2, 160, 16, 4, 12), (1, 160, 16, 2, 14), (2, 160, 16, 2, 13),
                    (2, 1, 16, 2, 14), (2, 160, 1, 2, 14)):
            with self.subTest(row=row), self.assertRaises(RuntimeError):
                check_mixed_production_totals([list(map(str, row))])

    def test_mixed_submission_compares_each_nested_frame_and_original_execution_order(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_mixed_response
        common = {"except": None, "error_code": None, "console": "", "console_markers": []}
        expected, traces = [], []
        order = [1, 3, 4, 6, 5, 2, 7, 8, 10, 9]
        for ordinal in range(1, 11):
            root = ordinal <= 2
            pure = ordinal in (6, 10)
            receiver = "test" if root else "bob" if ordinal in (5, 9) else "empty" if pure else "carol"
            children = []
            for index in range(3 if root else 1 if ordinal in (5, 9) else 0):
                children.append({"call_ordinal": index + 1, "sender_ordinal": index, "receiver": "bob",
                                 "read_only": False, "hex_data": "00" * (2 - index) if root else "",
                                 "return_value_hex_data": "0b00000000000000000000000000000000"})
            item = {"action_ordinal": ordinal, "creator_action_ordinal": 0 if root else 1 if ordinal < 7 else 2,
                    "closest_unnotified_ancestor_action_ordinal": 0 if root else 1 if ordinal < 7 else 2,
                    "receiver": receiver, "context_free": pure,
                    "act": {"account": "test" if root else receiver, "name": "run" if root else "pure" if pure else "normal",
                            "authorization": [{"actor": "test", "permission": "active"}] if root else [],
                            "data": ordinal.to_bytes(8, "little").hex()},
                    "return_value_hex_data": children[0]["return_value_hex_data"] if children else "",
                    "calls": children}
            expected.append(item)
            trace = {**common, **{k: deepcopy(v) for k, v in item.items() if k not in ("calls", "receiver")},
                     "receipt": {"receiver": receiver, "global_sequence": 100 + order.index(ordinal)},
                     "account_ram_deltas": [],
                     "call_traces": [{**common, **child, "error_id": None} for child in children]}
            traces.append(trace)
        entry = {"actions": 2, "expected": {"actions": expected, "execution_order": order}}
        response = {"processed": {"action_traces": traces}}
        original = deepcopy((entry, response))
        check_mixed_response(entry, response)
        self.assertEqual((entry, response), original)
        nested = deepcopy(response)
        nested["processed"]["action_traces"] = nested["processed"]["action_traces"][:2]
        nested["processed"]["action_traces"][0]["inline_traces"] = deepcopy(traces[2:6])
        nested["processed"]["action_traces"][1]["inline_traces"] = deepcopy(traces[6:])
        for trace in nested["processed"]["action_traces"]:
            trace["act"]["hex_data"] = trace["act"]["data"]
            trace["act"]["data"] = {"abi_decoded": "not the original bytes"}
        check_mixed_response(entry, nested)
        for ordinal in range(10):
            for field, value in (("action_ordinal", 99), ("creator_action_ordinal", 99),
                                 ("closest_unnotified_ancestor_action_ordinal", 99),
                                 ("context_free", not traces[ordinal]["context_free"]),
                                 ("return_value_hex_data", "ffff"), ("account_ram_deltas", [{"account": "bob", "delta": 8}]),
                                 ("except", {"code": 1}), ("error_code", 1), ("console", "bad"), ("console_markers", [1])):
                altered = deepcopy(response)
                altered["processed"]["action_traces"][ordinal][field] = value
                with self.subTest(ordinal=ordinal, field=field), self.assertRaises(RuntimeError):
                    check_mixed_response(entry, altered)
        for ordinal in (0, 1, 4, 8):
            for index in range(len(traces[ordinal]["call_traces"])):
                for field, value in (("call_ordinal", 99), ("sender_ordinal", 99), ("receiver", "wrong"),
                                     ("read_only", True), ("hex_data", "ffff"), ("return_value_hex_data", "ffff"),
                                     ("except", {"code": 1}), ("error_code", 1), ("error_id", -1),
                                     ("console", "bad"), ("console_markers", [1])):
                    altered = deepcopy(response)
                    altered["processed"]["action_traces"][ordinal]["call_traces"][index][field] = value
                    with self.subTest(ordinal=ordinal, index=index, field=field), self.assertRaises(RuntimeError):
                        check_mixed_response(entry, altered)
        for fault in range(9):
            altered_entry, altered = deepcopy((entry, response))
            items = altered["processed"]["action_traces"]
            if fault == 0:
                items.pop()
            elif fault == 1:
                items.append(deepcopy(items[0]))
            elif fault == 2:
                items[0]["receipt"]["global_sequence"] = items[1]["receipt"]["global_sequence"]
            elif fault == 3:
                items[0]["receipt"]["global_sequence"], items[1]["receipt"]["global_sequence"] = \
                    items[1]["receipt"]["global_sequence"], items[0]["receipt"]["global_sequence"]
            elif fault == 4:
                items[0]["call_traces"].pop()
            elif fault == 5:
                items[0]["receipt"]["receiver"] = "wrong"
            elif fault == 6:
                items[0]["act"]["data"] = "ffff"
            elif fault == 7:
                altered_entry["actions"] = 1
            else:
                altered_entry["expected"]["actions"][0]["calls"].pop()
            with self.subTest(fault=fault), self.assertRaises(RuntimeError):
                check_mixed_response(altered_entry, altered)

    def test_sync_submission_trace_checks_bind_root_child_payload_and_original_status(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_sync_response
        data = (7).to_bytes(8, "little").hex()
        returned = (22).to_bytes(8, "little").hex()
        entry = {"actions": 1, "root_data": data + (10000).to_bytes(8, "little").hex(),
                 "call_data": data, "return_data": returned}
        common = {"except": None, "error_code": None, "console": "", "console_markers": []}
        child = {**common, "call_ordinal": 1, "sender_ordinal": 0, "receiver": "bob", "read_only": False,
                 "hex_data": data, "return_value_hex_data": returned, "error_id": None}
        root = {**common, "action_ordinal": 1, "creator_action_ordinal": 0,
                "closest_unnotified_ancestor_action_ordinal": 0, "context_free": False,
                "receipt": {"receiver": "test"}, "account_ram_deltas": [],
                "act": {"account": "test", "name": "run", "data": entry["root_data"],
                        "authorization": [{"actor": "test", "permission": "active"}]},
                "return_value_hex_data": returned, "call_traces": [child]}
        response = {"processed": {"action_traces": [root]}}
        check_sync_response(entry, response)
        modern = deepcopy(response)
        modern["processed"]["action_traces"][0]["act"]["hex_data"] = entry["root_data"]
        modern["processed"]["action_traces"][0]["act"]["data"] = {"decoded": "ignored"}
        check_sync_response(entry, modern)
        for fault in ("different child input", "wrong root length"):
            changed_entry, changed = deepcopy(entry), deepcopy(response)
            changed_root = changed["processed"]["action_traces"][0]
            if fault == "different child input":
                changed_entry["call_data"] = (9).to_bytes(8, "little").hex()
                changed_root["call_traces"][0]["hex_data"] = changed_entry["call_data"]
            else:
                changed_entry["root_data"] += "00"
                changed_root["act"]["data"] = changed_entry["root_data"]
            with self.subTest(fault=fault), self.assertRaisesRegex(RuntimeError, "payload prefix"):
                check_sync_response(changed_entry, changed)
        mutations = [
            ((), "actions", 2, "sole root"),
            (("root",), "action_ordinal", 2, "ancestry"),
            (("root",), "creator_action_ordinal", 1, "ancestry"),
            (("root",), "closest_unnotified_ancestor_action_ordinal", 1, "ancestry"),
            (("root", "receipt"), "receiver", "bob", "receiver or type"),
            (("root",), "context_free", True, "receiver or type"),
            (("root", "act"), "account", "bob", "target or authorization"),
            (("root", "act"), "authorization", [], "target or authorization"),
            (("root", "act"), "data", "", "root payload"),
            (("root",), "inline_traces", [{}], "inline action or RAM"),
            (("root",), "account_ram_deltas", [{"account": "bob", "delta": 1}], "inline action or RAM"),
            (("root",), "return_value_hex_data", "", "root return"),
            (("root",), "call_traces", [], "lost or duplicated"),
            (("call",), "call_ordinal", 2, "identity or read-only"),
            (("call",), "sender_ordinal", 1, "identity or read-only"),
            (("call",), "receiver", "test", "identity or read-only"),
            (("call",), "read_only", True, "identity or read-only"),
            (("call",), "hex_data", "", "child input or return"),
            (("call",), "return_value_hex_data", "", "child input or return"),
            (("call",), "error_id", -10000, "error status"),
        ]
        mutations += [((kind,), field, value, message) for kind in ("root", "call")
                      for field, value, message in (("except", {"code": 1}, "exception"),
                          ("error_code", 1, "exception"), ("console", "unexpected", "output"),
                          ("console_markers", [0], "output"))]
        for path, key, value, message in mutations:
            changed_entry, changed = deepcopy(entry), deepcopy(response)
            changed_root = changed["processed"]["action_traces"][0]
            target = changed_entry if not path else changed_root if path[0] == "root" else changed_root["call_traces"][0]
            for segment in path[1:]:
                target = target[segment]
            target[key] = value
            with self.subTest(path=path, key=key), self.assertRaisesRegex(RuntimeError, message):
                check_sync_response(changed_entry, changed)

    def test_multi_sync_trace_checks_each_original_root_and_its_own_child_without_mutating_input(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_multi_sync_response
        data = (7).to_bytes(8, "little").hex()
        returned = (22).to_bytes(8, "little").hex()
        entry = {"actions": 2, "roots": []}
        traces = []
        for index in range(2):
            root_data = data + (10000 + index).to_bytes(8, "little").hex()
            entry["roots"].append({"root_data": root_data, "call_data": data, "return_data": returned})
            common = {"except": None, "error_code": None, "console": "", "console_markers": []}
            child = {**common, "call_ordinal": 1, "sender_ordinal": 0, "receiver": "bob",
                     "read_only": False, "hex_data": data, "return_value_hex_data": returned, "error_id": None}
            traces.append({**common, "action_ordinal": index + 1, "creator_action_ordinal": 0,
                "closest_unnotified_ancestor_action_ordinal": 0, "context_free": False,
                "receipt": {"receiver": "test", "global_sequence": 100 + index}, "account_ram_deltas": [],
                "act": {"account": "test", "name": "run", "data": root_data,
                        "authorization": [{"actor": "test", "permission": "active"}]},
                "return_value_hex_data": returned, "call_traces": [child]})
        response = {"processed": {"action_traces": traces}}
        before = deepcopy((entry, response))
        check_multi_sync_response(entry, response)
        self.assertEqual((entry, response), before)
        for index in range(2):
            for fault in range(12):
                altered_entry, altered = deepcopy((entry, response))
                root = altered["processed"]["action_traces"][index]
                call = root["call_traces"][0]
                if fault == 0:
                    root["action_ordinal"] = 3
                elif fault == 1:
                    root["creator_action_ordinal"] = 1
                elif fault == 2:
                    root["act"]["data"] = entry["roots"][1 - index]["root_data"]
                elif fault == 3:
                    root["call_traces"].append(deepcopy(call))
                elif fault == 4:
                    call["call_ordinal"] = 2
                elif fault == 5:
                    call["sender_ordinal"] = 1
                elif fault == 6:
                    call["read_only"] = True
                elif fault == 7:
                    call["return_value_hex_data"] = ""
                elif fault == 8:
                    root["except"] = {"code": 1}
                elif fault == 9:
                    call["error_id"] = -10000
                elif fault == 10:
                    root["console_markers"] = [0]
                else:
                    root["receipt"]["global_sequence"] = 100 + (1 - index)
                with self.subTest(index=index, fault=fault), self.assertRaises(RuntimeError):
                    check_multi_sync_response(altered_entry, altered)
        for fault in range(5):
            altered_entry, altered = deepcopy((entry, response))
            if fault == 0:
                altered_entry["actions"] = 1
            elif fault == 1:
                altered_entry["roots"].pop()
            elif fault == 2:
                altered["processed"]["action_traces"].pop()
            elif fault == 3:
                altered_entry["roots"][1]["root_data"] = altered_entry["roots"][0]["root_data"]
            else:
                altered_entry["roots"][1]["call_data"] = (9).to_bytes(8, "little").hex()
            with self.subTest(fault=fault), self.assertRaises(RuntimeError):
                check_multi_sync_response(altered_entry, altered)

    def test_context_free_trace_checks_bind_type_data_queues_and_original_receipts(self):
        from copy import deepcopy
        from parallel_producer_process_test import check_context_free_response
        receivers = ["test", "test", "bob", "carol", "dave", "dave", "bob", "carol", "dave", "carol",
                     "bob", "carol", "dave", "dave", "bob", "carol", "dave", "carol"]
        creators = [0, 0, 1, 1, 1, 1, 1, 5, 3, 3, 2, 2, 2, 2, 2, 13, 11, 11]
        ancestors = [0, 0, 1, 1, 1, 1, 1, 1, 3, 3, 2, 2, 2, 2, 2, 2, 11, 11]
        methods = ["run", "run", "normal", "pure", "run", "normal", "pure", "pure", "pure", "normal",
                   "normal", "pure", "run", "normal", "pure", "pure", "pure", "normal"]
        order = {ordinal: i + 100 for i, ordinal in enumerate((1, 5, 4, 7, 8, 3, 9, 10, 6, 2, 13, 12, 15, 16, 11, 17, 18, 14))}
        traces = []
        for ordinal, receiver, creator, ancestor, method in zip(range(1, 19), receivers, creators, ancestors, methods):
            root = 2 if ordinal == 2 or ordinal >= 11 else 1
            traces.append({"action_ordinal": ordinal, "creator_action_ordinal": creator,
                "closest_unnotified_ancestor_action_ordinal": ancestor, "context_free": method == "pure",
                "receipt": {"receiver": receiver, "global_sequence": order[ordinal]}, "account_ram_deltas": [],
                "act": {"account": "test" if method == "run" else receiver, "name": method,
                        "data": str(root) if method == "run" else "78797a",
                        "authorization": [{"actor": "test", "permission": "active"}] if method == "run" else []}})
        response = {"processed": {"action_traces": traces}}
        check_context_free_response({"actions": 2}, response)
        nested = deepcopy(traces)
        for trace in nested:
            if trace["creator_action_ordinal"]:
                nested[trace["creator_action_ordinal"] - 1].setdefault("inline_traces", []).append(trace)
        tree = {"processed": {"action_traces": nested[:2]}}
        check_context_free_response({"actions": 2}, tree)
        cycle = deepcopy(tree)
        cycle["processed"]["action_traces"][0]["inline_traces"].append(cycle["processed"]["action_traces"][0])
        with self.assertRaisesRegex(RuntimeError, "bounds"):
            check_context_free_response({"actions": 2}, cycle)
        for fault, message in enumerate(("lost", "ordinals", "receiver", "ancestry", "type", "target", "authorization", "payload", "RAM", "execution order")):
            changed = deepcopy(response); entries = changed["processed"]["action_traces"]
            if fault == 0:
                entries.pop()
            elif fault == 1:
                entries[3]["action_ordinal"] = 1
            elif fault == 2:
                entries[3]["receipt"]["receiver"] = "bob"
            elif fault == 3:
                entries[7]["closest_unnotified_ancestor_action_ordinal"] = 5
            elif fault == 4:
                entries[3]["context_free"] = False
            elif fault == 5:
                entries[3]["act"]["name"] = "normal"
            elif fault == 6:
                entries[3]["act"]["authorization"] = [{"actor": "test", "permission": "active"}]
            elif fault == 7:
                entries[3]["act"]["data"] = ""
            elif fault == 8:
                entries[3]["account_ram_deltas"] = [{"account": "carol", "delta": 1}]
            else:
                entries[3]["receipt"]["global_sequence"] = entries[4]["receipt"]["global_sequence"]
            with self.subTest(fault=fault), self.assertRaisesRegex(RuntimeError, message):
                check_context_free_response({"actions": 2}, changed)

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
