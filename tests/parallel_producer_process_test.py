#!/usr/bin/env python3
"""Live isolated producers with original serial and new restricted validators.

Creates NEW loopback-only chains and deterministic TEST credentials. Never
opens an existing wallet, data/config directory or nonlocal peer. Does not
claim hardware capacity or physical power-loss safety. Retains all evidence.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

from parallel_execution_test import Node, checked_manifest, require, sha256
sys.path.insert(0, str(Path(__file__).resolve().parent / "PerformanceHarness"))
from finality_benchmark import Journal, Observer
from finality_tps import analyze, read_events
from parallel_replay_benchmark import proc_usage, scrape_stages
from stage_report import compare as compare_stages


def wait_for(predicate, description, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise RuntimeError("Timed out: " + description)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-nodeos", type=Path, required=True)
    parser.add_argument("--new-nodeos", type=Path, required=True)
    parser.add_argument("--fixture-exporter", type=Path, required=True)
    parser.add_argument("--old-sha256", required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--token-fixture", action="store_true", help="Exercise unchanged real token transfers and notification receipts")
    parser.add_argument("--ram-fixture", action="store_true", help="With --token-fixture, first transfer changes the sender row RAM payer")
    parser.add_argument("--lifecycle-fixture", action="store_true", help="With --token-fixture, first transfer creates the recipient balance and table")
    args = parser.parse_args()
    if args.ram_fixture and not args.token_fixture:
        parser.error("--ram-fixture requires --token-fixture")
    if args.lifecycle_fixture and (not args.token_fixture or args.ram_fixture):
        parser.error("--lifecycle-fixture requires --token-fixture and cannot be combined with --ram-fixture")
    old, new, exporter = args.old_nodeos.resolve(), args.new_nodeos.resolve(), args.fixture_exporter.resolve()
    require(old != new and all(path.is_file() for path in (old, new, exporter)), "Distinct valid binaries required")
    require(sha256(old) == args.old_sha256, "Original binary hash changed")
    require(args.output_parent.is_dir(), "Output parent must exist")
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-producer-", dir=args.output_parent.resolve()))
    report = {"status": "running", "root": str(root), "script_sha256": sha256(Path(__file__)),
              "fixture_kind": "real-token-first-balance" if args.lifecycle_fixture else
                              "real-token-payer-change" if args.ram_fixture else "real-token" if args.token_fixture else "existing-row-wat",
              "old_sha256": sha256(old), "new_sha256": sha256(new), "exporter_sha256": sha256(exporter), "phases": []}
    nodes = []
    journals = []
    print("Evidence: " + str(root), flush=True)
    try:
        for workers in (0, 1, 2, 4, None):
            label = "old-producer" if workers is None else f"new-producer-{workers}"
            group = root / label
            group.mkdir()
            corpus = group / "corpus"
            env = {**os.environ, "PUREEOS_PARALLEL_PRODUCER_EXPORT_DIR": str(corpus)}
            if args.token_fixture:
                env["PUREEOS_PARALLEL_PRODUCER_TOKEN"] = "1"
            else:
                env.pop("PUREEOS_PARALLEL_PRODUCER_TOKEN", None)
            if args.ram_fixture:
                env["PUREEOS_PARALLEL_PRODUCER_RAM"] = "1"
            else:
                env.pop("PUREEOS_PARALLEL_PRODUCER_RAM", None)
            if args.lifecycle_fixture:
                env["PUREEOS_PARALLEL_PRODUCER_LIFECYCLE"] = "1"
            else:
                env.pop("PUREEOS_PARALLEL_PRODUCER_LIFECYCLE", None)
            with (group / "export.log").open("x") as log:
                result = subprocess.run([str(exporter), "--run_test=parallel_production_tests/export_fresh_live_producer_fixture",
                    "--report_level=detailed", "--color_output=no"], env=env, cwd=group,
                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
            require(result.returncode == 0, "Fresh fixture export failed: " + str(group))
            manifest = checked_manifest(corpus)
            require(manifest.get("fixture_kind") == report["fixture_kind"], "Exporter returned wrong fixture kind")
            # A new serial observer feeds bootstrap blocks to an OLD producer
            # over P2P, bypassing the documented pre-existing HTTP cache bug.
            feeder = Node(new, group / "new-observer", corpus, manifest, 2, peer="listen", history=True)
            nodes.append(feeder)
            producer = Node(old if workers is None else new, group / "producer", corpus, manifest,
                            workers, peer=feeder.endpoint, history=True)
            nodes.append(producer)
            observer = Node(old, group / "old-observer", corpus, manifest, None, peer=producer.endpoint, history=True)
            nodes.append(observer)
            config_path = producer.root / "config/config.ini"
            lines = ["producer-name = eosio", "enable-stale-production = true", "pause-on-startup = true",
                     "read-mode = head", "max-transaction-time = 100", "contracts-console = false",
                     "performance-metrics = true", "plugin = eosio::prometheus_plugin"]
            if workers:
                lines.append("experimental-parallel-production = true")
            keys = json.loads((corpus / "TEST-ONLY-keys.json").read_text())
            lines.extend("signature-provider = " + key for key in keys)
            config_path.write_text(config_path.read_text() + "\n".join(lines) + "\n")
            for node in (feeder, producer, observer):
                node.start("live", ["--genesis-json", str(corpus / "genesis.json")])
            for point in manifest["checkpoints"]:
                feeder.feed(point)
                producer.check(point)
                observer.check(point)
            entries = json.loads((corpus / "transactions.json").read_text())
            if args.ram_fixture or args.lifecycle_fixture:
                require(len(entries) == 16, "RAM/lifecycle fixtures require the fixed 16-transaction corpus")
            metrics_before = scrape_stages(producer)
            (group / "metrics-before.txt").write_text(metrics_before)
            journal = Journal(group / "finality-evidence.jsonl")
            journals.append(journal)
            finality = Observer(f"http://127.0.0.1:{producer.port}", manifest["chain_id"], journal, 10)
            finality.initialize()
            started = time.monotonic_ns()
            ended = started + 14_000_000_000
            journal.emit("window", version=1, clock="monotonic_ns", start_ns=started, end_ns=ended,
                         scope="Finite queued 16-transaction compatibility load; NOT saturation or capacity")
            responses = []
            payer_change = {}
            for entry in entries:
                journal.emit("sent", id=entry["id"], actions=1)
                response = producer.rpc("push_transaction", entry["packed"])
                require(response["transaction_id"] == entry["id"], "Submission ID changed")
                require(response["processed"]["receipt"]["status"] == "executed", "Speculative submission failed")
                if (args.ram_fixture or args.lifecycle_fixture) and not responses:
                    payer_change = {item["account"]: item["delta"] for item in
                                    response["processed"]["action_traces"][0]["account_ram_deltas"]}
                    if args.ram_fixture:
                        require(payer_change.get("alice", 0) < 0 and payer_change.get("sponsor", 0) > 0 and
                                sum(payer_change.values()) == 0, "First token transfer did not move the sender RAM payer")
                    else:
                        require(payer_change.get("alice", 0) > 0 and sum(payer_change.values()) > 0,
                                "First token transfer did not charge for the new balance/table")
                responses.append(response["transaction_id"])
                journal.emit("ack", id=entry["id"], accepted=True)
            before = producer.rpc("get_info")
            # Default produce-block-offset accumulates across all 12 slots;
            # a 500ms boundary is NOT necessarily a full production window.
            # Resume just after the preceding slot of a six-second round so
            # pending slot index=0 gets its original full local CPU effort.
            # Up to six seconds waiting is included in the fixed 14s window.
            # No consensus or local production limits are relaxed.
            time.sleep((5.53 - time.time() % 6) % 6)
            producer.rpc("resume", namespace="producer")
            target = before["head_block_num"] + 10
            producer.rpc("pause_at_block", {"block_num": target}, namespace="producer")
            def reached_target():
                finality.sample()
                return producer.rpc("get_info")["head_block_num"] >= target
            wait_for(reached_target, "live producer advances to pause point")
            producer.rpc("pause", namespace="producer")
            while time.monotonic_ns() < ended:
                finality.sample()
                time.sleep(0.1)
            finality.sample()
            journal.emit("end")
            journal.close()
            journals.remove(journal)
            finalized = analyze(read_events(group / "finality-evidence.jsonl"))
            require(finalized["finalized_success_unique"] == len(entries) and
                    not finalized["cohort_unresolved_at_end"] and
                    finalized["measurement_complete_without_sampling_or_sender_errors"], "Finality accounting incomplete")
            info = producer.rpc("get_info")
            require(info["head_block_num"] == target, "Producer did not honor the fixed stop height")
            require(info["last_irreversible_block_num"] >= target - 2, "Native Savanna finality stalled")
            state = producer.rpc("get_integrity_hash", namespace="producer")
            metrics_after = scrape_stages(producer)
            (group / "metrics-after.txt").write_text(metrics_after)
            stages = compare_stages(metrics_before, metrics_after)
            usage = proc_usage(producer.process.pid)
            point = {"block_num": target, "block_id": info["head_block_id"], "lib": info["last_irreversible_block_num"],
                     "integrity_hash": state["integrity_hash"]}
            for node in (feeder, observer):
                node.check(point)
            receipts, timestamps = [], []
            for number in range(before["head_block_num"] + 1, target + 1):
                block = producer.rpc("get_block", {"block_num_or_id": number})
                other = observer.rpc("get_block", {"block_num_or_id": number})
                require(block == other, f"Old full validator disagrees on block {number}")
                timestamps.append(block["timestamp"])
                for trx in block["transactions"]:
                    require(trx["status"] == "executed", "Nonexecuted production receipt")
                    require(trx["cpu_usage_us"] >= 100, "Missing original minimum CPU charge")
                    receipts.append(trx["trx"]["id"])
            require(receipts == responses, "Missing, duplicate or reordered production receipt")
            # After the first catch-up slot, empty/full blocks must maintain the
            # normal 500 ms schedule in this explicitly bounded local scenario.
            from datetime import datetime
            gaps = [(datetime.fromisoformat(b) - datetime.fromisoformat(a)).total_seconds()
                    for a, b in zip(timestamps, timestamps[1:])]
            require(all(gap == 0.5 for gap in gaps), "Missed slot in the local producer test")
            for node in (producer, feeder, observer):
                node.stop()
            text = producer.log.read_text(errors="replace")
            totals = re.findall(r"Experimental parallel production totals: batches=(\d+), attempts=(\d+), "
                                r"succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)", text)
            if workers:
                require(len(totals) == 1 and int(totals[0][3]) > 0, "Live producer never adopted a private result")
                if args.ram_fixture or args.lifecycle_fixture:
                    require(int(totals[0][3]) == 6, "RAM/lifecycle transfer was not adopted in the fixed two-batch corpus")
            else:
                require(not totals, "Serial mode unexpectedly ran a production batch")
            histories = [node.history_digests() for node in (producer, feeder, observer)]
            require(histories[0] == histories[1] == histories[2], "Live SHiP trace/state output differs")
            # The immutable old program also reconstructs these NEW produced
            # blocks from disk with full checks, not merely from cached traces.
            observer.start("checked-production-replay", ["--replay-blockchain", "--force-all-checks",
                                                         "--disable-replay-opts"])
            observer.check(point)
            observer.stop()
            require(observer.history_digests() == histories[2], "Old checked replay changed finalized history output")
            item = {"phase": label, "head": target, "lib": info["last_irreversible_block_num"],
                    "code_hashes": manifest["code_hashes"],
                    "first_transfer_ram_deltas": payer_change,
                    "state": state["integrity_hash"], "transactions": len(receipts), "slot_gaps_seconds": gaps,
                    "production_totals": totals, "history_sha256": histories[0], "old_checked_replay": True}
            item["bounded_load_finality"] = finalized
            item["stages"] = stages
            item["process_usage"] = usage
            report["phases"].append(item)
            print(json.dumps({key: value for key, value in item.items() if key != "stages"}), flush=True)
        require(sha256(old) == args.old_sha256 and sha256(new) == report["new_sha256"], "Binary changed during test")
        report["status"] = "passed"
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        for journal in journals:
            journal.close()
        for node in reversed(nodes):
            node.stop(check=False)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print("Report: " + str(root / "report.json"), flush=True)


if __name__ == "__main__":
    main()
