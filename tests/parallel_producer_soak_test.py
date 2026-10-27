#!/usr/bin/env python3
"""Bounded long-lived producer/validator soak, not maximum-capacity testing.

Keeps one experimental producer and old/new full validators alive over many
real block windows, repeatedly exercises queued adoption, and verifies exact
receipts/state/LIB. Only a fresh private loopback test chain is modified.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

from parallel_execution_test import Node, checked_manifest, require, sha256
from parallel_producer_process_test import wait_for, proc_usage, scrape_stages, compare_stages


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-nodeos", required=True, type=Path)
    parser.add_argument("--new-nodeos", required=True, type=Path)
    parser.add_argument("--fixture-exporter", required=True, type=Path)
    parser.add_argument("--old-sha256", required=True)
    parser.add_argument("--output-parent", required=True, type=Path)
    parser.add_argument("--workers", type=int, choices=(0, 1, 2, 4), default=4)
    parser.add_argument("--old-producer", action="store_true", help="Diagnose the immutable reference without new options")
    parser.add_argument("--rounds", type=int, default=180)
    args = parser.parse_args()
    old, new, exporter = (path.resolve() for path in (args.old_nodeos, args.new_nodeos, args.fixture_exporter))
    require(old != new and all(path.is_file() for path in (old, new, exporter)), "Distinct binaries required")
    require(sha256(old) == args.old_sha256, "Old binary hash changed")
    require(args.output_parent.is_dir() and 1 <= args.rounds <= 240, "Invalid directory or bounded round count")
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-producer-soak-", dir=args.output_parent.resolve()))
    report = {"status": "running", "scope": __doc__, "script_sha256": sha256(Path(__file__)),
              "old_sha256": sha256(old), "new_sha256": sha256(new), "exporter_sha256": sha256(exporter),
              "workers": None if args.old_producer else args.workers, "rounds": [], "start_loadavg": os.getloadavg()}
    nodes = []
    started = time.monotonic()
    print("Evidence: " + str(root), flush=True)
    try:
        corpus = root / "corpus"
        env = {**os.environ, "PUREEOS_PARALLEL_PRODUCER_EXPORT_DIR": str(corpus),
               "PUREEOS_PARALLEL_PRODUCER_TRANSACTION_COUNT": str(args.rounds * 16)}
        with (root / "export.log").open("x") as log:
            result = subprocess.run([str(exporter), "--run_test=parallel_production_tests/export_fresh_live_producer_fixture",
                "--report_level=detailed", "--color_output=no"], env=env, cwd=root,
                stdout=log, stderr=subprocess.STDOUT, timeout=120)
        require(result.returncode == 0, "Fresh fixture export failed")
        manifest = checked_manifest(corpus)
        feeder = Node(new, root / "new-validator", corpus, manifest, 2, peer="listen", history=True)
        nodes.append(feeder)
        producer = Node(old if args.old_producer else new, root / "producer", corpus, manifest,
                        None if args.old_producer else args.workers, peer=feeder.endpoint, history=True)
        nodes.append(producer)
        observer = Node(old, root / "old-validator", corpus, manifest, None, peer=producer.endpoint, history=True)
        nodes.append(observer)
        config = producer.root / "config/config.ini"
        lines = ["producer-name = eosio", "enable-stale-production = true", "pause-on-startup = true",
                 "read-mode = head", "max-transaction-time = 100",
                 "performance-metrics = true", "plugin = eosio::prometheus_plugin"]
        if not args.old_producer and args.workers:
            lines.append("experimental-parallel-production = true")
        lines.extend("signature-provider = " + key for key in json.loads((corpus / "TEST-ONLY-keys.json").read_text()))
        config.write_text(config.read_text() + "\n".join(lines) + "\n")
        for node in nodes:
            node.start("soak", ["--genesis-json", str(corpus / "genesis.json")])
        for point in manifest["checkpoints"]:
            feeder.feed(point)
            producer.check(point)
            observer.check(point)
        entries = json.loads((corpus / "transactions.json").read_text())
        require(len(entries) == args.rounds * 16, "Exporter count was not honored")
        metrics_before = scrape_stages(producer)
        (root / "metrics-before.txt").write_text(metrics_before)
        all_receipts = []
        active_started = time.monotonic()
        for round_number in range(args.rounds):
            batch = entries[round_number * 16:(round_number + 1) * 16]
            before = producer.rpc("get_info")
            target = before["head_block_num"] + 6
            # At pause_at_block there is no speculative pending block. RPC
            # completion correctly waits for resume; do not deadlock this test
            # by awaiting it before resume. Observe each actual queue insertion
            # before sending the next request, retaining controlled order.
            with ThreadPoolExecutor(max_workers=16) as sender:
                futures = []
                for entry in batch:
                    future = sender.submit(producer.rpc, "push_transaction", entry["packed"])
                    futures.append(future)
                    def queued():
                        queued_ids = {row["trx_id"] for row in producer.rpc("get_unapplied_transactions",
                                      {"limit": 4096, "time_limit_ms": 100}, namespace="producer")["trxs"]}
                        return entry["id"] in queued_ids
                    wait_for(queued, "ordered actual queue insertion", timeout=5)
                producer.rpc("resume", namespace="producer")
                producer.rpc("pause_at_block", {"block_num": target}, namespace="producer")
                for entry, future in zip(batch, futures):
                    response = future.result(timeout=15)
                    require(response["transaction_id"] == entry["id"] and
                            response["processed"]["receipt"]["status"] == "executed", "Soak submission failed")
            wait_for(lambda: producer.rpc("get_info")["head_block_num"] >= target, "soak advances", timeout=30)
            producer.rpc("pause", namespace="producer")
            info = producer.rpc("get_info")
            require(info["head_block_num"] == target and info["last_irreversible_block_num"] == target - 2,
                    "Soak height/finality mismatch")
            state = producer.rpc("get_integrity_hash", namespace="producer")
            point = {"block_num": target, "block_id": info["head_block_id"], "lib": info["last_irreversible_block_num"],
                     "integrity_hash": state["integrity_hash"]}
            for node in (feeder, observer):
                node.check(point)
            receipts, times = [], []
            last_business_block = 0
            for number in range(before["head_block_num"] + 1, target + 1):
                block = producer.rpc("get_block", {"block_num_or_id": number})
                require(block == observer.rpc("get_block", {"block_num_or_id": number}), "Old validator block mismatch")
                times.append(datetime.fromisoformat(block["timestamp"]))
                for trx in block["transactions"]:
                    require(trx["status"] == "executed" and trx["cpu_usage_us"] >= 100, "Invalid receipt")
                    receipts.append(trx["trx"]["id"])
                    last_business_block = number
            require(receipts == [entry["id"] for entry in batch], "Lost, repeated or reordered receipt")
            require(last_business_block <= info["last_irreversible_block_num"], "Business cohort not yet finalized")
            # Deliberate pause/resume between rounds is not counted as a missed
            # production slot; every active six-block interval is checked.
            gaps = [(b - a).total_seconds() for a, b in zip(times, times[1:])]
            require(all(gap == .5 for gap in gaps), "Missed active block slot")
            all_receipts.extend(receipts)
            row = {"round": round_number, "head": target, "lib": point["lib"], "state": point["integrity_hash"],
                   "transactions": len(receipts), "active_slot_gaps_seconds": gaps,
                   "usage": {node.root.name: proc_usage(node.process.pid) for node in nodes}}
            report["rounds"].append(row)
            if round_number % 10 == 0 or round_number + 1 == args.rounds:
                print(json.dumps(row), flush=True)
        report["active_scenario_seconds"] = time.monotonic() - active_started
        require(len(set(all_receipts)) == len(entries), "Duplicate canonical cohort")
        metrics_after = scrape_stages(producer)
        (root / "metrics-after.txt").write_text(metrics_after)
        report["stages"] = compare_stages(metrics_before, metrics_after)
        for node in nodes:
            node.stop()
        totals = re.findall(r"Experimental parallel production totals: batches=(\d+), attempts=(\d+), "
                            r"succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)", producer.log.read_text())
        if args.old_producer or not args.workers:
            require(not totals, "Serial producer unexpectedly ran experimental batches")
        else:
            require(len(totals) == 1 and int(totals[0][3]) >= args.rounds, "Soak did not repeatedly adopt results")
        histories = [node.history_digests() for node in nodes]
        require(histories[0] == histories[1] == histories[2], "Soak full trace/state-history mismatch")
        require(sha256(old) == report["old_sha256"] and sha256(new) == report["new_sha256"], "Binary changed")
        report.update(status="passed", production_totals=totals, history_sha256=histories[0],
                      finalized_transactions=len(all_receipts))
    except BaseException as error:
        report["status"], report["error"] = "failed", str(error)
        raise
    finally:
        for node in reversed(nodes):
            node.stop(check=False)
        report["total_seconds"] = time.monotonic() - started
        report["end_loadavg"] = os.getloadavg()
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print("Report: " + str(root / "report.json"), flush=True)


if __name__ == "__main__":
    main()
