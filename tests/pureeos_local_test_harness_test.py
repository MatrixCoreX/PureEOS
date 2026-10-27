#!/usr/bin/env python3
"""Deterministic tests of local-chain RPC pagination and finality observation."""
import json
import io
from pathlib import Path
import unittest
import urllib.error
from unittest.mock import Mock, patch

from pureeos_single_choice_local_test import LocalTest
from pureeos_multinode_local_test import MultiNodeTest
from pureeos_large_multinode_local_test import LargeMultiNodeTest


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.client = object.__new__(LocalTest)
        self.client.node = Mock()
        self.client.node.poll.return_value = None
        self.client.report = {}
        self.client.nonce = 0

    def test_disposable_node_keeps_an_absolute_disk_reserve(self):
        self.client.root = Path("/isolated-test")
        self.client.nodeos = Path("/test/nodeos")
        self.client.spawn = Mock()
        self.client.start_node("genesis", ["--genesis-json", "/isolated-test/genesis.json"])
        command, log_name = self.client.spawn.call_args.args
        flag = "--resource-monitor-space-absolute-gb"
        self.assertEqual(command.count(flag), 1)
        self.assertEqual(command[command.index(flag) + 1], "8")
        self.assertEqual(log_name, "genesis.log")
        self.assertNotIn("--resource-monitor-not-shutdown-on-threshold-exceeded", command)
        self.assertIs(self.client.node, self.client.spawn.return_value)

    def test_explicit_disk_reserve_is_not_duplicated_or_overridden(self):
        self.client.root = Path("/isolated-test")
        self.client.nodeos = Path("/test/nodeos")
        for extra in (["--resource-monitor-space-absolute-gb", "8"],
                      ["--resource-monitor-space-absolute-gb=4"]):
            with self.subTest(extra=extra):
                self.client.spawn = Mock()
                self.client.start_node("restart", extra)
                command = self.client.spawn.call_args.args[0]
                self.assertEqual(command, ["/test/nodeos", "--data-dir", "/isolated-test/data",
                                          "--config-dir", "/isolated-test/config", *extra])

    def test_bootstrap_retries_only_atomic_setcode_cpu_failure(self):
        self.client.contracts = Path("/contracts")
        self.client.redact = lambda value: value
        self.client.rpc = Mock(return_value={"head_block_num": 10})
        self.client.wait_lib = Mock()
        self.client.tx = Mock(side_effect=[RuntimeError("setcode: tx_cpu_usage_exceeded"), {"ok": True}])
        self.assertEqual(self.client.deploy_system(), {"ok": True})
        self.assertEqual(self.client.tx.call_count, 2)
        self.client.wait_lib.assert_called_once_with(12)
        self.assertEqual(len(self.client.report["deployment_cpu_retries"]), 1)

    def test_bootstrap_cpu_retries_are_bounded_and_do_not_hide_other_errors(self):
        self.client.contracts = Path("/contracts")
        self.client.redact = lambda value: value
        self.client.rpc = Mock(return_value={"head_block_num": 10})
        self.client.wait_lib = Mock()
        for message, calls in (("setcode: tx_cpu_usage_exceeded", 3),
                               ("setcode: missing authority", 1), ("unrelated tx_cpu_usage_exceeded", 1)):
            with self.subTest(message=message):
                self.client.tx = Mock(side_effect=RuntimeError(message))
                with self.assertRaisesRegex(RuntimeError, message):
                    self.client.deploy_system()
                self.assertEqual(self.client.tx.call_count, calls)

    def test_independent_node_harness_never_shares_peer_signers(self):
        self.assertEqual(MultiNodeTest.local_signers, ("eosio",))
        self.assertEqual(LargeMultiNodeTest.local_signers, ("eosio",))
        self.assertEqual(len(LargeMultiNodeTest.names), 22)
        self.assertEqual(len(set(LargeMultiNodeTest.names)), 22)

    def test_large_harness_isolates_state_and_retains_resource_guard(self):
        client = object.__new__(LargeMultiNodeTest)
        client.root, client.state_root = Path("/private/evidence"), Path("/dev/shm/private-state")
        client.build = Path("/build")
        client.nodeos = Path("/build/nodeos")
        with patch.object(LocalTest, "spawn") as spawn:
            client.spawn([str(client.nodeos), "--data-dir", "/private/evidence/nodea/data",
                          "--resource-monitor-space-absolute-gb", "8"], "nodea.log")
            command = spawn.call_args.args[0]
            self.assertEqual(command[command.index("--data-dir") + 1], "/dev/shm/private-state/nodea/data")
            self.assertEqual(command.count("--resource-monitor-space-absolute-gb"), 1)
            self.assertEqual(command[command.index("--resource-monitor-space-absolute-gb") + 1], "1")
            self.assertEqual(command[command.index("--p2p-max-nodes-per-host") + 1], "64")
            self.assertEqual(command[command.index("--http-max-response-time-ms") + 1], "200")
            self.assertEqual(command[command.index("--abi-serializer-max-time-ms") + 1], "200")
            self.assertNotIn("--max-transaction-time", command)
            self.assertNotIn("--resource-monitor-not-shutdown-on-threshold-exceeded", command)

    def test_long_test_wallet_timeout_is_explicit_and_only_on_disposable_wallet(self):
        client = object.__new__(LargeMultiNodeTest)
        client.build, client.nodeos = Path("/build"), Path("/build/nodeos")
        with patch.object(LocalTest, "spawn") as spawn:
            client.spawn(["/build/programs/keosd/keosd", "--http-server-address", ""], "wallet.log")
            self.assertEqual(spawn.call_args.args[0], ["/build/programs/keosd/keosd", "--http-server-address", "",
                                                     "--unlock-timeout", "14400"])

    @staticmethod
    def rpc_error(code):
        return urllib.error.HTTPError("http://127.0.0.1", 500, "test", {},
                                      io.BytesIO(json.dumps({"error": {"code": code}}).encode()))

    @patch("pureeos_single_choice_local_test.time.sleep")
    def test_read_only_abi_deadline_is_retried_and_recorded(self, _sleep):
        self.client.redact = lambda value: value
        response = io.BytesIO(b'{"rows": []}')
        with patch("pureeos_single_choice_local_test.urllib.request.urlopen",
                   side_effect=[self.rpc_error(3015010), response]) as open_rpc:
            self.assertEqual(self.client.read_rpc("http://127.0.0.1", "get_table_rows"), {"rows": []})
            self.assertEqual(open_rpc.call_count, 2)
        self.assertEqual(self.client.report["rpc_deadline_retries"][0]["code"], 3015010)

    @patch("pureeos_single_choice_local_test.time.sleep")
    def test_rpc_errors_never_retry_mutations_or_non_deadlines_and_are_bounded(self, _sleep):
        self.client.redact = lambda value: value
        for method, code, calls in (("push_transaction", 3015010, 1), ("get_table_rows", 3050003, 1),
                                    ("get_table_rows", 3015010, 3)):
            with self.subTest(method=method, code=code):
                with patch("pureeos_single_choice_local_test.urllib.request.urlopen",
                           side_effect=[self.rpc_error(code) for _ in range(calls)]) as open_rpc:
                    with self.assertRaisesRegex(RuntimeError, f"RPC {method} HTTP 500"):
                        self.client.read_rpc("http://127.0.0.1", method)
                    self.assertEqual(open_rpc.call_count, calls)

    @staticmethod
    def block(number, trx=None, status="executed"):
        return {"id": f"block-{number}", "transactions": [] if trx is None else [{"trx": trx, "status": status}]}

    @staticmethod
    def page(owners, more="", total="100"):
        return {"rows": [{"owner": owner, "total_votes": "25"} for owner in owners],
                "more": more, "vote_schema": "integer-v2", "vote_precision": 4,
                "total_producer_vote_amount": total}

    @patch("pureeos_single_choice_local_test.time.sleep")
    def test_height_alone_does_not_confirm_transaction(self, _sleep):
        self.client.rpc = Mock(side_effect=[{"last_irreversible_block_num": 11}, self.block(11),
                                           {"last_irreversible_block_num": 12}, self.block(12, {"id": "wanted"})])
        result = self.client.wait_transaction("wanted", 10)
        self.assertEqual(result["block_num"], 12)
        self.assertEqual(result["block_id"], "block-12")

    def test_string_receipt_and_final_status(self):
        self.client.rpc = Mock(side_effect=[{"last_irreversible_block_num": 11}, self.block(11, "wanted")])
        self.assertEqual(self.client.wait_transaction("wanted", 10)["block_num"], 11)
        self.client.rpc = Mock(side_effect=[{"last_irreversible_block_num": 11}, self.block(11, "wanted", "hard_fail")])
        with self.assertRaisesRegex(RuntimeError, "not executed"):
            self.client.wait_transaction("wanted", 10)

    @patch("pureeos_single_choice_local_test.time.sleep")
    def test_rpc_retry_does_not_skip_block(self, _sleep):
        self.client.rpc = Mock(side_effect=[{"last_irreversible_block_num": 11}, OSError("temporary"),
                                           {"last_irreversible_block_num": 11}, self.block(11, "wanted")])
        self.assertEqual(self.client.wait_transaction("wanted", 10)["block_num"], 11)
        self.assertEqual([c.args[1]["block_num_or_id"] for c in self.client.rpc.call_args_list
                          if c.args[0] == "get_block"], [11, 11])

    @patch("pureeos_single_choice_local_test.time.sleep")
    @patch("pureeos_single_choice_local_test.time.monotonic", side_effect=[0, 0, 0, 2])
    def test_missing_transaction_times_out(self, _clock, _sleep):
        self.client.rpc = Mock(side_effect=[{"last_irreversible_block_num": 11}, self.block(11)])
        with self.assertRaisesRegex(RuntimeError, "not found in irreversible"):
            self.client.wait_transaction("wanted", 10, timeout=1)

    def test_submission_records_actual_not_speculative_height(self):
        self.client.rpc = Mock(return_value={"last_irreversible_block_num": 10})
        self.client.run = Mock(return_value=json.dumps({"transaction_id": "wanted",
            "processed": {"block_num": 11, "receipt": {"status": "executed"}}}))
        self.client.wait_transaction = Mock(return_value={"transaction_id": "wanted", "block_num": 13})
        self.client.tx(["push", "action"])
        self.client.wait_transaction.assert_called_once_with("wanted", 10)
        self.assertEqual(self.client.report["confirmed_transactions"][0]["block_num"], 13)
        self.assertEqual(self.client.report["confirmed_transactions"][0]["reported_block_num"], 11)

    def test_pagination_preserves_server_order(self):
        self.client.ranked = Mock(side_effect=[self.page(["c", "a"], "b"), self.page(["b", "d"])])
        result = self.client.ranked_all()
        self.assertEqual([r["owner"] for r in result["rows"]], ["c", "a", "b", "d"])
        self.assertEqual(len(result["pages"]), 2)
        self.assertEqual(self.client.ranked.call_args_list[1].kwargs, {"lower_bound": "b"})

    @patch("pureeos_single_choice_local_test.time.sleep")
    def test_empty_page_can_retry_cursor(self, _sleep):
        self.client.ranked = Mock(side_effect=[self.page([], "a"), self.page([], "a"), self.page(["a"])])
        self.assertEqual(len(self.client.ranked_all()["rows"]), 1)

    def test_changed_total_or_duplicate_producer_rejected(self):
        self.client.ranked = Mock(side_effect=[self.page(["a"], "b"), self.page(["b"], total="101")])
        with self.assertRaisesRegex(RuntimeError, "changed during pagination"):
            self.client.ranked_all()
        self.client.ranked = Mock(side_effect=[self.page(["a"], "b"), self.page(["a"])])
        with self.assertRaisesRegex(RuntimeError, "Duplicate producer"):
            self.client.ranked_all()

    @patch("pureeos_single_choice_local_test.time.sleep")
    @patch("pureeos_single_choice_local_test.time.monotonic", side_effect=[0, 0, 31])
    def test_nonadvancing_empty_pages_time_out(self, _clock, _sleep):
        self.client.ranked = Mock(return_value=self.page([], "a"))
        with self.assertRaisesRegex(RuntimeError, "pagination timed out"):
            self.client.ranked_all()


if __name__ == "__main__":
    unittest.main()
