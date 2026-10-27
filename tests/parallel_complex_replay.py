#!/usr/bin/env python3
"""Same-binary/same-JIT complex-workload replay, NOT live/finality TPS.

Uses fresh copies of one stopped and fully checked live benchmark's block log.
Keeps CPU/NET validation and all normal checks. No production clock or second
node competes with the measured process. Every rotated-order round is retained.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from parallel_execution_test import Node, checked_manifest, require, sha256
from parallel_release_recovery import block_hashes, select_sample, checked_sample
from parallel_release_benchmark import check_adoption, parallel_diagnostics
sys.path.insert(0, str(Path(__file__).resolve().parent / "PerformanceHarness"))
from benchmark_environment import preflight, sample, wait_for_quiet_gate, runtime_cpu_evidence
from parallel_replay_benchmark import proc_usage, replay_timing, scrape_stages, stages_since_process_start
from statistics import median


def power_profile():
    if not shutil.which("powerprofilesctl"):
        return None  # headless servers need not run a desktop power service
    try:
        return subprocess.check_output(["powerprofilesctl", "get"], text=True,
                                       stderr=subprocess.DEVNULL, timeout=10).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def replay_source(report, allow_failed_adoption=False):
    if report.get("status") == "passed":
        return select_sample(report, 0, 1)
    # Diagnostic escape hatch for a CHECKED SERIAL checkpoint only. Never
    # relabel the live result, permit an invalid row, or allow general failures.
    require(allow_failed_adoption and report.get("status") == "failed" and
            report.get("error") == "Enabled eligible mode never adopted any result",
            "Need passed source or explicit zero-adoption diagnostic source")
    return checked_sample(report, 0, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--quiet-gate", type=Path, required=True)
    parser.add_argument("--workers", default="0,2,4")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--metrics", action="store_true")
    parser.add_argument("--allow-checked-serial-from-failed-adoption", action="store_true",
                        help="Only reuse an independently valid SERIAL checkpoint after live zero-adoption failure; keep source status")
    args = parser.parse_args()
    workers = [int(v) for v in args.workers.split(",")]
    require(len(set(workers)) == len(workers) and 0 in workers and len(workers) > 1 and
            all(0 <= v <= 16 for v in workers) and 1 <= args.repetitions <= 10, "Invalid repetitions/modes")
    source = args.report.resolve(strict=True)
    original = json.loads(source.read_text())
    selected = replay_source(original, args.allow_checked_serial_from_failed_adoption)
    require(Path(original["root"]).resolve() == source.parent, "Source root moved")
    binary = Path(original["parameters"]["nodeos"]).resolve(strict=True)
    require(sha256(binary) == original["inputs"][str(binary)], "Source binary changed")
    corpus = source.parent / "round-1/corpus"
    manifest = checked_manifest(corpus)
    blocks = source.parent / "round-1/workers-0/validator/data/blocks"
    original_blocks = block_hashes(blocks)
    storage = preflight(args.output_parent)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-complex-replay-", dir=args.output_parent.resolve()))
    frozen_paths = [source, binary, Path(__file__), Path(__file__).with_name("parallel_execution_test.py"),
                    Path(__file__).with_name("parallel_release_recovery.py"),
                    Path(__file__).with_name("parallel_release_benchmark.py"),
                    Path(__file__).parent / "PerformanceHarness/parallel_replay_benchmark.py",
                    Path(__file__).parent / "PerformanceHarness/benchmark_environment.py", args.quiet_gate]
    frozen = {str(p.resolve()): sha256(p) for p in frozen_paths}
    report = {"status": "running", "root": str(root), "source_report": str(source), "inputs": frozen,
              "source_status": original["status"],
              "source_policy": "explicit checked serial checkpoint only" if args.allow_checked_serial_from_failed_adoption else "whole live report passed",
              "source_blocks": original_blocks, "storage": storage, "workload": original["workload"],
              "parameters": {"workers": workers, "repetitions": args.repetitions, "batch_size": 32,
                             "engine": "jit", "metrics": args.metrics}, "samples": [],
              "scope": "checked replay throughput, not production/finality TPS; no changes to resource limits"}
    count = len(json.loads((corpus / "transactions.json").read_text()))
    report["business_transactions"] = count
    point = selected["checkpoint"]
    node = None
    print("Evidence: " + str(root), flush=True)
    try:
        for repeat in range(args.repetitions):
            order = workers[repeat % len(workers):] + workers[:repeat % len(workers)]
            for mode in order:
                case = root / f"round-{repeat + 1}-workers-{mode}"
                case.mkdir()
                gate = wait_for_quiet_gate(args.quiet_gate, case / "quiet-gate")
                profile_before = power_profile()
                preflight(case)
                require(all(sha256(Path(p)) == h for p, h in frozen.items()), "Frozen input changed")
                node = Node(binary, case / "node", corpus, manifest, mode, engine="jit", batch_size=32)
                shutil.copytree(blocks, node.root / "data/blocks")
                require(block_hashes(node.root / "data/blocks") == original_blocks, "Block copy mismatch")
                stop = threading.Event()
                errors = []
                def monitor():
                    with (case / "host.jsonl").open("x") as output:
                        while not stop.is_set():
                            try:
                                process = node.process
                                state = sample([process.pid] if process else [])
                                output.write(json.dumps(state) + "\n"); output.flush()
                                if state["memory_kib"].get("MemAvailable", 0) < 1024 * 1024:
                                    errors.append("low-memory")
                                if shutil.disk_usage(case).free < storage["reserve_bytes"]:
                                    errors.append("disk-reserve-exhausted")
                                for proc in Path("/proc").iterdir():
                                    if proc.name.isdigit():
                                        try:
                                            if (proc / "comm").read_text().strip() in {"cc1", "cc1plus", "ninja", "cmake", "make", "rustc", "cargo"}:
                                                errors.append("concurrent-compiler")
                                        except OSError:
                                            pass
                            except Exception as error:
                                errors.append(type(error).__name__)
                            stop.wait(1)
                thread = threading.Thread(target=monitor)
                thread.start()
                try:
                    started = time.monotonic()
                    extra = ["--replay-blockchain", "--force-all-checks", "--disable-replay-opts"]
                    if args.metrics:
                        extra.extend(["--performance-metrics", "--plugin", "eosio::prometheus_plugin"])
                    node.start("checked-replay", extra)
                    startup = time.monotonic() - started
                    usage = proc_usage(node.process.pid)
                    state = node.check(point)
                    stages = None
                    if args.metrics:
                        metrics = scrape_stages(node)
                        (case / "metrics.txt").write_text(metrics)
                        stages = stages_since_process_start(metrics)
                    node.stop()
                finally:
                    stop.set(); thread.join()
                blocks_count, seconds = replay_timing(node.log.read_text())
                totals = node.totals("checked-replay", require_adoption=report["workload"]["parallel_eligible"])
                check_adoption(mode, report["workload"]["actions_per_transaction"], totals[0] if totals else None)
                profile_after = power_profile()
                if profile_before != profile_after:
                    errors.append("power-profile-changed")
                if report["workload"].get("unique_scopes") and stages:
                    require(stages["stages"]["parallel_conflict"]["attempt_count"] == 0,
                            "Unexpected dependency refresh despite unique-scope workload")
                row = {"round": repeat + 1, "workers": mode, "replay_seconds": seconds,
                       "business_transactions_per_second": count / seconds,
                       "actions_per_second": count * report["workload"].get("executed_actions_per_transaction",
                           report["workload"]["actions_per_transaction"]) / seconds,
                       "state": state, "replayed_blocks": blocks_count, "startup_seconds": startup,
                       "totals": totals, "usage": usage, "monitor_errors": errors,
                       "runtime_cpu_evidence": runtime_cpu_evidence(
                           json.loads(line) for line in (case / "host.jsonl").read_text().splitlines()),
                       "validation_diagnostics_including_rolled_back_blocks": parallel_diagnostics(node.log.read_text(), "validation"),
                       "stages": stages, "power_profile_before": profile_before, "power_profile_after": profile_after,
                       "quiet_gate": gate, "valid": not errors}
                report["samples"].append(row)
                print(json.dumps({k: v for k, v in row.items() if k != "stages"}), flush=True)
        report["comparison"] = {}
        for mode in workers:
            rows = [r for r in report["samples"] if r["workers"] == mode]
            values = [r["replay_seconds"] for r in rows]
            report["comparison"][str(mode)] = {"median_seconds": median(values),
                "min_seconds": min(values), "max_seconds": max(values), "rounds": len(rows)}
        serial = report["comparison"]["0"]
        report["performance_evidence_status"] = "eligible" if all(
            r["runtime_cpu_evidence"]["status"] == "eligible" for r in report["samples"]) else "inconclusive"
        for mode, row in report["comparison"].items():
            row["speed_ratio_vs_serial"] = serial["median_seconds"] / row["median_seconds"]
            row["every_parallel_faster_than_every_serial"] = mode != "0" and row["max_seconds"] < serial["min_seconds"]
        report["status"] = "passed" if all(r["valid"] for r in report["samples"]) else "inconclusive"
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["error"] = "Operator changed test matrix; retain all samples, do not claim completed comparison"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        if node:
            node.stop(check=False)
        if block_hashes(blocks) != original_blocks or not all(sha256(Path(p)) == h for p, h in frozen.items()):
            report["status"] = "failed"
            report["input_integrity_failure"] = True
        with (root / "report.json").open("x") as output:
            json.dump(report, output, indent=2)
    print(json.dumps({"status": report["status"], "report": str(root / "report.json")}))
    return 0 if report["status"] == "passed" else 130 if report["status"] == "interrupted" else 2


if __name__ == "__main__":
    raise SystemExit(main())
