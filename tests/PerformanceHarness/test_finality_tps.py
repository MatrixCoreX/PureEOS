#!/usr/bin/env python3
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

from finality_tps import analyze, quantiles
from finality_benchmark import Journal, Observer, load_transactions, run

S = 1_000_000_000


def event(kind, at, **kw):
    return {"type": kind, "at_ns": round(at * S), **kw}


def block(bid, parent, num, at, *receipts):
    return event("block", at, id=bid, previous=parent, num=num,
                 receipts=[{"id": tid, "status": status} for tid, status in receipts])


def sample(at, head, lib):
    return event("sample", at, head_id=head, lib_id=lib)


def evidence():
    return [
        {"type": "window", "version": 1, "clock": "monotonic_ns", "start_ns": S, "end_ns": 11*S},
        event("anchor", 0, id="a", num=1), sample(0, "a", "a"),
        event("sent", 1, id="x", actions=3), event("ack", 1.1, id="x", accepted=True),
        block("b", "a", 2, 2, ("x", "executed"), ("setup-not-business", "executed")), sample(2, "b", "a"),
        block("c", "b", 3, 3), sample(3, "c", "b"), sample(11, "c", "b"), event("end", 12)]


class FinalityTests(unittest.TestCase):
    def test_finalized_success_and_actions_are_different(self):
        result = analyze(evidence())
        self.assertEqual(result["finalized_success_tps"], .1)
        self.assertEqual(result["finalized_actions_per_second"], .3)
        self.assertEqual(result["finalization_latency"]["p50_ms"], 2000)
        self.assertEqual(result["inclusion_latency"]["p50_ms"], 1000)
        self.assertEqual(result["cohort_unresolved_at_end"], [])

    def test_no_half_second_block_assumption_and_no_empty_block_pruning(self):
        events = evidence()
        events[0]["end_ns"] = 101*S
        events[-2]["at_ns"] = 101*S
        events[-1]["at_ns"] = 102*S
        self.assertEqual(analyze(events)["finalized_success_tps"], .01)

    def test_acknowledgement_is_not_finality(self):
        events = evidence()
        for e in events:
            if e["type"] == "sample":
                e["lib_id"] = "a"
        result = analyze(events)
        self.assertEqual(result["rpc_accepted_unique_tps_diagnostic"], .1)
        self.assertEqual(result["finalized_success_tps"], 0)
        self.assertEqual(result["cohort_unresolved_at_end"], ["x"])

    def test_failed_receipts_never_pay_tps(self):
        for status in ("soft_fail", "hard_fail", "delayed", "expired"):
            with self.subTest(status=status):
                events = evidence()
                events[5]["receipts"][0]["status"] = status
                result = analyze(events)
                self.assertEqual(result["finalized_success_tps"], 0)
                self.assertEqual(result["canonical_included_unique_tps"], 0)

    def test_duplicate_sends_acks_and_samples_are_unique(self):
        events = evidence()
        events += [event("sent", 1.2, id="x"), event("ack", 1.3, id="x", accepted=True), sample(4, "c", "b")]
        result = analyze(events)
        self.assertEqual(result["sent_unique"], 1)
        self.assertEqual(result["send_attempts_in_window"], 2)
        self.assertEqual(result["duplicate_send_attempts_all"], 1)
        self.assertEqual(result["finalized_success_unique"], 1)

    def test_drain_does_not_inflate_window(self):
        events = evidence()
        events[8]["lib_id"] = "a"
        result = analyze(events)  # first finalization at t=11, excluded by [1,11)
        self.assertEqual(result["finalized_success_tps"], 0)
        self.assertEqual(result["finalized_during_drain"], 1)
        self.assertEqual(result["cohort_finalized_by_window_end"], 0)
        self.assertEqual(result["cohort_finalized_by_evidence_end"], 1)

    def test_warmup_backlog_finalized_in_window_counts_but_is_not_send_cohort(self):
        events = evidence()
        events[3]["at_ns"] = S//2
        self.assertEqual(analyze(events)["sent_unique"], 0)
        self.assertEqual(analyze(events)["finalized_success_unique"], 1)

    def test_orphan_excluded_even_at_same_height(self):
        events = evidence()
        events[8]["lib_id"] = "a"
        events[-2] = sample(11, "fork", "fork")
        events.append(block("fork", "a", 2, 4))
        result = analyze(events)
        self.assertEqual(result["canonical_included_unique_tps"], 0)
        self.assertEqual(result["finalized_success_unique"], 0)

    def test_reincluded_after_fork_gets_new_observation(self):
        events = evidence()
        events[8]["lib_id"] = "a"
        events[-2] = sample(11, "fork", "fork")
        events += [block("fork", "a", 2, 4, ("x", "executed")), sample(5, "fork", "fork")]
        result = analyze(events)
        self.assertEqual(result["finalization_latency"]["p50_ms"], 4000)
        self.assertEqual(result["inclusion_latency"]["p50_ms"], 4000)

    def test_rpc_timeout_can_still_be_finalized(self):
        events = evidence()
        events[4]["accepted"] = False
        result = analyze(events)
        self.assertEqual(result["rpc_error_events"], 1)
        self.assertEqual(result["finalized_success_unique"], 1)

    def test_missing_ancestry_is_fail_closed(self):
        events = evidence()
        events[7]["previous"] = "missing"
        with self.assertRaisesRegex(ValueError, "missing"):
            analyze(events)

    def test_noncontiguous_height_is_rejected(self):
        events = evidence()
        events[7]["num"] = 90
        with self.assertRaisesRegex(ValueError, "noncontiguous"):
            analyze(events)

    def test_conflicting_receipts_for_same_id_rejected(self):
        events = evidence()
        bad = copy.deepcopy(events[5])
        bad["receipts"] = []
        events.append(bad)
        with self.assertRaisesRegex(ValueError, "conflicting"):
            analyze(events)

    def test_finality_regression_and_fork_rejected(self):
        for replacement in ("a", "fork"):
            events = evidence() + [block("fork", "a", 2, 5)]
            events[-3] = sample(11, replacement, replacement)
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                analyze(events)

    def test_lib_not_on_head_branch_rejected(self):
        events = evidence()
        events[-2]["lib_id"] = "fork"
        events.append(block("fork", "a", 2, 5))
        with self.assertRaises(ValueError):
            analyze(events)

    def test_no_send_or_system_activity_not_counted(self):
        events = [e for e in evidence() if e["type"] not in ("sent", "ack")]
        self.assertEqual(analyze(events)["finalized_success_tps"], 0)

    def test_missing_end_truncated_window_and_wrong_clock_rejected(self):
        mutations = [lambda e: e.pop(), lambda e: e[-1].update(at_ns=10*S),
                     lambda e: e[0].update(clock="utc"), lambda e: e[0].update(end_ns=S)]
        for mutation in mutations:
            events = evidence()
            mutation(events)
            with self.assertRaises(ValueError):
                analyze(events)

    def test_missing_bracketing_samples_rejected(self):
        events = evidence()
        events[-2]["at_ns"] = 10*S
        with self.assertRaisesRegex(ValueError, "bracket"):
            analyze(events)

    def test_observer_and_sender_failures_are_not_capacity_success(self):
        events = evidence() + [event("error", 6, stage="observe"), event("error", 7, stage="sender_capacity")]
        result = analyze(events)
        self.assertFalse(result["measurement_complete_without_sampling_or_sender_errors"])
        self.assertEqual(result["observer_error_events"], 1)
        self.assertEqual(result["sender_missed_schedule"], 1)

    def test_clock_inversion_and_unknown_event_rejected(self):
        for extra in (event("sent", 4, id="setup-not-business"), event("bogus", 4)):
            with self.assertRaises(ValueError):
                analyze(evidence() + [extra])

    def test_tail_after_end_rejected(self):
        with self.assertRaisesRegex(ValueError, "after end"):
            analyze(evidence() + [sample(13, "c", "b")])

    def test_latency_empty_and_percentiles(self):
        self.assertIsNone(quantiles([])["p99_ms"])
        result = quantiles(range(1_000_000, 101_000_000, 1_000_000))
        self.assertEqual((result["p50_ms"], result["p95_ms"], result["p99_ms"]), (50, 95, 99))


class CollectorTests(unittest.TestCase):
    def test_end_to_end_http_sender_observer_and_recalculation(self):
        # HTTP fixture, NOT a blockchain performance result. Exercise real sockets,
        # concurrency, the open-loop sender, journaling and independent recalculation.
        lock = threading.Lock()
        chain = [{"id": "a", "block_num": 1, "previous": "genesis", "transactions": []}]
        pending = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with lock:
                    if self.path.endswith("get_info"):
                        chain.append({"id": f"block-{len(chain)+1}", "block_num": len(chain)+1,
                                      "previous": chain[-1]["id"], "transactions": list(pending)})
                        pending.clear()
                        result = {"chain_id": "test-chain", "head_block_id": chain[-1]["id"],
                                  "last_irreversible_block_id": chain[-2]["id"],
                                  "last_irreversible_block_num": chain[-2]["block_num"]}
                    elif self.path.endswith("get_block"):
                        result = next(b for b in chain if b["id"] == payload["block_num_or_id"])
                    else:
                        txid = hashlib.sha256(bytes.fromhex(payload["packed_trx"])).hexdigest()
                        pending.append({"trx": {"id": txid}, "status": "executed"})
                        result = {"transaction_id": txid, "processed": {"receipt": {"status": "executed"}}}
                encoded = json.dumps(result).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                packets = Path(directory) / "packets.jsonl"
                packets.write_text("\n".join(json.dumps({"signatures": [], "compression": 0,
                    "packed_context_free_data": "", "packed_trx": f"fixture-{i}".encode().hex()}) for i in range(4)))
                args = SimpleNamespace(endpoint=f"http://127.0.0.1:{server.server_port}", chain_id="test-chain",
                    transactions=packets, evidence=Path(directory)/"evidence.jsonl", rate=5, seconds=.5,
                    warmup=.1, drain=.2, poll=.02, timeout=2, inflight=2)
                result = run(args)
                self.assertGreater(result["finalized_success_unique"], 0)
                self.assertEqual(result["cohort_unresolved_at_end"], [])
                raw = [json.loads(line) for line in args.evidence.read_text().splitlines()]
                self.assertEqual(result, analyze(raw))
                self.assertNotIn("packed_trx", args.evidence.read_text())
                self.assertNotIn("signatures", args.evidence.read_text())
                # A short prepared input is rejected before contacting the chain.
                args.rate = 100
                with patch("finality_benchmark.rpc") as request:
                    with self.assertRaisesRegex(ValueError, "unique transactions"):
                        run(args)
                    request.assert_not_called()
                args.rate = 5
                args.evidence = Path(directory) / "failed-evidence.jsonl"
                original_emit = Journal.emit

                def fail_ack_write(journal, kind, **fields):
                    if kind == "ack":
                        raise OSError("injected evidence write failure")
                    return original_emit(journal, kind, **fields)

                with patch.object(Journal, "emit", fail_ack_write):
                    with self.assertRaisesRegex(ValueError, "collector worker failed"):
                        run(args)
                self.assertNotIn('"type":"end"', args.evidence.read_text())
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_prepared_ids_compression_and_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "packets.jsonl"
            raw = b"test fixture, not a signed blockchain transaction"
            packet = {"signatures": [], "compression": "zlib", "packed_context_free_data": "",
                      "packed_trx": zlib.compress(raw).hex()}
            path.write_text(json.dumps(packet) + "\n")
            self.assertEqual(load_transactions(path)[0][0], hashlib.sha256(raw).hexdigest())
            path.write_text((json.dumps(packet) + "\n") * 2)
            with self.assertRaisesRegex(ValueError, "duplicate"):
                load_transactions(path)

    def test_journal_never_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence"
            journal = Journal(path)
            journal.emit("end")
            journal.close()
            with self.assertRaises(FileExistsError):
                Journal(path)

    def test_observer_pins_block_ids_and_uses_one_clock(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "evidence"
            journal = Journal(path)
            observer = Observer("http://test", "chain", journal, 1)
            info = {"chain_id": "chain", "last_irreversible_block_id": "a", "last_irreversible_block_num": 1,
                    "head_block_id": "b"}
            with patch("finality_benchmark.rpc", side_effect=[info, info, {
                    "id": "b", "block_num": 2, "previous": "a",
                    "transactions": [{"trx": {"id": "x"}, "status": "executed"}]}]) as request:
                observer.initialize()
                self.assertEqual(request.call_args.args[2], {"block_num_or_id": "b"})
            journal.close()
            records = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual([e["type"] for e in records], ["anchor", "block", "sample"])
            self.assertLessEqual(records[-1]["started_ns"], records[-1]["at_ns"])

    def test_observer_wrong_chain_rejected_before_send(self):
        observer = Observer("http://test", "expected", None, 1)
        with patch("finality_benchmark.rpc", return_value={"chain_id": "wrong"}):
            with self.assertRaisesRegex(ValueError, "wrong chain"):
                observer.initialize()


if __name__ == "__main__":
    unittest.main()
