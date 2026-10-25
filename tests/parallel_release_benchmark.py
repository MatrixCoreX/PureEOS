#!/usr/bin/env python3
"""Fresh profile-2, SAME-BINARY/SAME-JIT finality throughput comparison.

Only creates private loopback test chains. Keeps all rounds, including invalid
measurements. Offered load is not capacity; sender-limited runs cannot establish
a speedup. The synthetic workload reads and updates real persistent rows.
An explicit --test-block-cpu-us changes ONLY fresh local test genesis capacity;
its results cannot establish a release gain under the production defaults.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace

from parallel_execution_test import Node, checked_manifest, require, sha256
sys.path.insert(0, str(Path(__file__).resolve().parent / "PerformanceHarness"))
from benchmark_environment import preflight, sample, wait_for_quiet_gate, runtime_cpu_evidence, active_runtime_cpu_evidence
from finality_benchmark import run as measure, Journal, Observer
from finality_cpp_benchmark import validate_sends
from finality_tps import read_events, analyze
from parallel_replay_benchmark import scrape_stages, proc_usage


def final_ids(events):
    """IDs on the last observed irreversible branch, never orphaned receipts."""
    events = list(events)
    anchor = next(e["id"] for e in events if e["type"] == "anchor")
    blocks = {e["id"]: e for e in events if e["type"] == "block"}
    head = [e for e in events if e["type"] == "sample"][-1]["lib_id"]
    result, visited = set(), set()
    while head != anchor:
        require(head in blocks and head not in visited, "Incomplete final branch")
        visited.add(head)
        block = blocks[head]
        result.update(r["id"] for r in block["receipts"] if r["status"] == "executed")
        head = block["previous"]
    return result


def summarize(rows, workers, rounds):
    result = {}
    runtime_eligible = all(r.get("runtime_cpu_evidence", {}).get("status") == "eligible" for r in rows)
    for count in workers:
        selected = [r for r in rows if r["workers"] == count]
        if len(selected) != rounds or not all(r.get("valid") for r in selected):
            return {"available": False, "reason": "missing, failed or instrument-limited round; none discarded"}
        values = [r["finality"]["finalized_success_tps"] for r in selected]
        result[str(count)] = {"rounds": rounds, "median_tps": statistics.median(values),
                              "min_tps": min(values), "max_tps": max(values)}
    baseline = result["0"]["median_tps"]
    if baseline == 0:
        # A short fixed window can precede finality even when the complete
        # cohort later finalizes correctly. Keep every mode/sample, but neither
        # divide by zero nor turn this into a chain failure or infinite gain.
        return {"available": False, "modes": result,
                "performance_evidence_status": "inconclusive", "performance_goal_met": False,
                "reason_code": "zero-serial-finality-window",
                "reason": "No positive serial median in the fixed finality window; all samples retained, "
                          "no ratio inferred and no whole-batch TPS substituted"}
    require(baseline > 0, "Invalid serial throughput")
    for count, row in result.items():
        row["ratio_vs_serial_jit"] = row["median_tps"] / baseline
        row["improvement_percent"] = 100 * (row["ratio_vs_serial_jit"] - 1)
        row["repeatable_20_percent_gain"] = (runtime_eligible and count != "0" and rounds >= 3 and
            row["ratio_vs_serial_jit"] >= 1.2 and row["min_tps"] > result["0"]["max_tps"])
    return {"available": True, "modes": result,
            "performance_evidence_status": "eligible" if runtime_eligible else "inconclusive",
            "performance_goal_met": any(row["repeatable_20_percent_gain"] for row in result.values()),
            "criterion": "runtime CPU evidence eligible in every sample; at least 3 rounds, median >=1.2x "
                         "and slowest parallel round exceeds fastest serial round",
            "scope": "observed finalized TPS at this offered rate, not maximum or server capacity"}


def comparison_status(comparison):
    """Valid business execution is not enough to certify a noisy TPS run."""
    return ("passed" if comparison.get("available") is True and
            comparison.get("performance_evidence_status") == "eligible" else "inconclusive")


def performance_release_eligible(comparison, *, report_status, capacity_diagnostic, instrumentation_enabled):
    """Only the performance gate, not approval of an entire node release.

    A larger test CPU allowance or instrumented diagnostic can produce useful
    comparisons but cannot certify the production-default release target.
    """
    return (report_status == 'passed' and comparison.get('available') is True
            and comparison.get('performance_evidence_status') == 'eligible'
            and comparison.get('performance_goal_met') is True
            and capacity_diagnostic is False and instrumentation_enabled is False)


def producer_health_errors(log):
    # Successful receipts alone do not prove a healthy producer lifecycle.
    # Do not reject every future timestamp: early sealing inside a producer's
    # current round is intentional. These are actual invariant failures.
    return [message for message in (
        "called produce_block while not actually producing",
        "pending_block_state does not exist but it should",
        "producing without pending_block_state",
    ) if message in log]


PARALLEL_DIAGNOSTIC_FIELDS = (
    "prepared_successes", "preparation_deadlines", "preparation_budget_fallbacks",
    "preparation_other_fallbacks", "worker_deadlines", "worker_vm_timeouts",
    "worker_other_failures", "discovery_exhaustions", "prefix_refresh_requests",
    "prefix_refresh_successes",
)


def parallel_diagnostics(log, mode="production"):
    """Shutdown counters overlap; never sum them into lost/unique transactions.

    Missing logs stay None, not ten invented zeroes. A malformed or duplicate
    diagnostic record must not silently produce a seemingly valid report.
    """
    require(mode in ("production", "validation"), "Unknown parallel diagnostic mode")
    rows = re.findall(r"Experimental parallel " + mode + r" diagnostics: ([^;\n]+)", log)
    if not rows:
        return None
    require(len(rows) == 1, "Duplicate parallel diagnostic record")
    fields = rows[0].split(", ")
    require(len(fields) == len(PARALLEL_DIAGNOSTIC_FIELDS), "Incomplete parallel diagnostic record")
    result = {}
    for expected, field in zip(PARALLEL_DIAGNOSTIC_FIELDS, fields):
        match = re.fullmatch(re.escape(expected) + r"=(\d+)", field)
        require(match is not None, "Malformed parallel diagnostic field: " + expected)
        result[expected] = int(match.group(1))
    require(result["preparation_deadlines"] <= result["preparation_budget_fallbacks"],
            "Preparation deadlines must be included in budget fallbacks")
    require(result["prefix_refresh_successes"] <= result["prefix_refresh_requests"],
            "Refresh successes exceed requests")
    return result


def validator_workers(producer_workers, fixed_workers):
    require(fixed_workers is None or 0 <= fixed_workers <= 16, "Invalid fixed validator worker count")
    return producer_workers if fixed_workers is None else fixed_workers


def test_block_cpu_limit(value):
    """Deliberately bounded diagnostic, not a production parameter setter."""
    require(value is None or (type(value) is int and 200000 <= value <= 800000),
            "Test block CPU must be an integer in 200000..800000 microseconds")
    return value


def fixture_resource_policy(genesis, manifest, requested_cpu):
    """Reject stale exporters and changes outside the explicitly requested cap."""
    test_block_cpu_limit(requested_cpu)
    defaults = manifest.get("default_initial_configuration")
    actual = genesis.get("initial_configuration")
    require(isinstance(defaults, dict) and isinstance(actual, dict), "Missing fixture resource policy")
    require(defaults.get("max_block_cpu_usage") == 200000 and
            defaults.get("max_transaction_cpu_usage") == 150000 and
            defaults.get("min_transaction_cpu_usage") == 100 and
            defaults.get("max_block_net_usage") == 1048576, "Unexpected default resource policy")
    require(manifest.get("test_capacity_diagnostic") is (requested_cpu is not None),
            "Exporter diagnostic opt-in does not match request")
    expected = dict(defaults)
    if requested_cpu is not None:
        expected["max_block_cpu_usage"] = requested_cpu
    require(actual == expected and all(type(v) is int for v in actual.values()),
            "Fixture resource policy differs from request or changed another parameter")
    return {"diagnostic_only": requested_cpu is not None,
            "default_initial_configuration": defaults, "initial_configuration": actual,
            "comparison_scope": "Same block cap in every worker mode; cap increases are NOT parallel speedup",
            "production_default_performance_evidence": requested_cpu is None}


def check_live_resource_policy(response, expected):
    """Later chain_config versions may add fields, but must preserve this genesis."""
    actual = response.get("chain_config")
    require(isinstance(actual, dict) and all(type(actual.get(k)) is int and actual[k] == v
            for k, v in expected.items()), "Live node resource policy differs from fixture")
    return actual


ADMISSION_DIAGNOSTIC_FIELDS = (
    "capacity_reductions", "capacity_bypasses", "receipt_observations", "ready_unconsumed", "ready_no_receipt",
)


def admission_diagnostics(log):
    """Local query/normal-return counters, NOT a unique transaction adoption rate."""
    rows = re.findall(r"Experimental parallel production admission: ([^;\n]+)", log)
    if not rows:
        return None
    require(len(rows) == 1, "Duplicate production admission diagnostic record")
    fields = rows[0].split(", ")
    require(len(fields) == len(ADMISSION_DIAGNOSTIC_FIELDS), "Incomplete admission diagnostic record")
    result = {}
    for expected, field in zip(ADMISSION_DIAGNOSTIC_FIELDS, fields):
        match = re.fullmatch(re.escape(expected) + r"=(\d+)", field)
        require(match is not None, "Malformed admission diagnostic field: " + expected)
        result[expected] = int(match.group(1))
    return result


INTAKE_DIAGNOSTIC_FIELDS = (
    "staged", "drains", "batch_calls", "batch_offered", "serial_dispatches", "peak_transactions", "peak_bytes",
)


def intake_diagnostics(log):
    """Owner staging/dispatch counts only, never unique adoption or finality."""
    rows = re.findall(r"Experimental parallel intake: ([^;\n]+)", log)
    if not rows:
        return None
    require(len(rows) == 1, "Duplicate live intake diagnostic record")
    fields = rows[0].split(", ")
    # The frozen first intake candidate had no one-item recovery probes. Keep
    # its retained diagnostic logs readable for honest historical comparisons.
    names = INTAKE_DIAGNOSTIC_FIELDS
    if len(fields) == len(names) + 1:
        names = (*names, "single_probe_offers")
    require(len(fields) == len(names), "Incomplete live intake diagnostic record")
    result = {}
    for expected, field in zip(names, fields):
        match = re.fullmatch(re.escape(expected) + r"=(\d+)", field)
        require(match is not None, "Malformed live intake diagnostic field: " + expected)
        result[expected] = int(match.group(1))
    require(0 < result["drains"] <= result["staged"] and
            0 < result["peak_transactions"] <= min(64, result["staged"]) and
            0 < result["peak_bytes"] <= 4 * 1024 * 1024 and
            result.get("single_probe_offers", 0) <= result["batch_calls"] and
            result["batch_offered"] >= 2 * result["batch_calls"] - result.get("single_probe_offers", 0),
            "Invalid live intake bounds")
    return result


def action_delta(iterations, compute, kind="xorshift"):
    """Independent uint64 reference for the fixture's persistent checksum."""
    if not compute:
        return 1
    if kind == "sha256":
        payload = bytearray().join(i.to_bytes(8, "little") for i in range(1, 129))
        for _ in range(iterations):
            payload[:32] = hashlib.sha256(payload).digest()
        return 1 + int.from_bytes(payload[:2], "little")
    require(kind == "xorshift", "Unknown checksum workload")
    value, mask = 1, (1 << 64) - 1
    for _ in range(iterations):
        value ^= value >> 12
        value ^= (value << 25) & mask
        value ^= value >> 27
        value = value * 2685821657736338717 & mask
    return 1 + (value & 65535)


def expected_row_totals(entries, executed, delta):
    totals = Counter()
    for entry in entries:
        if entry["id"] in executed:
            require(1 <= entry["actions"] <= 100, "Invalid action count in corpus")
            updates = entry.get("row_updates", entry["actions"])
            require(1 <= updates <= 100, "Invalid row update count in corpus")
            totals[entry["scope"]] += updates * delta
    return totals


def check_adoption(workers, actions, totals):
    if not workers:
        return
    if actions <= 16:
        require(totals is not None, "Missing parallel production counters")
        require(totals["adopted"] > 0, "Enabled eligible mode never adopted any result")
    else:
        # Current implementation rejects >16 roots BEFORE attempting private
        # execution. An expected bypass is not successful parallel execution.
        # The node emits no totals line when there were zero batches. Keep
        # the raw report null in that case instead of inventing counters.
        require(totals is None or (totals["adopted"] == 0 and totals["attempts"] == 0),
                "Expected >16-root serial bypass no longer matches this binary")


def head_business_ids(node, head, anchor, capacity=None):
    result, visited = set(), set()
    while head != anchor:
        require(head not in visited and len(visited) < 10000, "Invalid or unbounded produced ancestry")
        visited.add(head)
        block = node.rpc("get_block", {"block_num_or_id": head})
        require(block["id"] == head, "Wrong block in final state check")
        if capacity is not None:
            capacity.append({"id": head, "transactions": len(block["transactions"]),
                "receipt_cpu_us": sum(int(r["cpu_usage_us"]) for r in block["transactions"]),
                "receipt_net_bytes": 8 * sum(int(r["net_usage_words"]) for r in block["transactions"])})
        for receipt in block["transactions"]:
            if receipt["status"] == "executed":
                trx = receipt["trx"]
                txid = trx if isinstance(trx, str) else trx["id"]
                require(txid not in result, "Duplicate executed transaction")
                result.add(txid)
        head = block["previous"]
    return result


def receipt_capacity(rows, configuration):
    """Receipt totals are a lower bound, not the full pending block resource bill."""
    cpu_limit = int(configuration["max_block_cpu_usage"])
    net_limit = int(configuration["max_block_net_usage"])
    require(cpu_limit > 0 and net_limit > 0, "Invalid genesis resource limits")
    total_transactions = sum(r["transactions"] for r in rows)
    total_cpu = sum(r["receipt_cpu_us"] for r in rows)
    nonempty = [r for r in rows if r["transactions"]]
    near_cpu_limit = sum(100 * r["receipt_cpu_us"] >= 95 * cpu_limit for r in nonempty)
    return {"blocks": len(rows), "transactions": total_transactions,
        "max_receipt_cpu_fraction": max((r["receipt_cpu_us"] / cpu_limit for r in rows), default=0),
        "max_receipt_net_fraction": max((r["receipt_net_bytes"] / net_limit for r in rows), default=0),
        "mean_receipt_cpu_us_per_transaction": total_cpu / total_transactions if total_transactions else None,
        "nonempty_blocks": len(nonempty),
        "mean_nonempty_receipt_cpu_fraction": sum(r["receipt_cpu_us"] for r in nonempty) / (len(nonempty) * cpu_limit) if nonempty else None,
        "nonempty_blocks_at_least_95_percent_receipt_cpu": near_cpu_limit,
        "fraction_nonempty_blocks_at_least_95_percent_receipt_cpu": near_cpu_limit / len(nonempty) if nonempty else None,
        "max_block_cpu_usage": cpu_limit, "max_block_net_usage": net_limit,
        "scope": "all produced blocks including warmup/drain; receipts only, excluding implicit/pending overhead"}


def wait_for(predicate, description, timeout=60):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise RuntimeError("Timed out: " + description)


def pin_paused_head(node):
    # pause() only sets a flag; an already scheduled block may still finish.
    # get_integrity_hash aborts/reschedules the pending block under that flag.
    # Pin ITS head first, then wait for get_info's asynchronous cache to match.
    state = node.rpc("get_integrity_hash", namespace="producer")
    def current():
        info = node.rpc("get_info")
        return info if info["head_block_id"] == state["head_block_id"] else None
    return wait_for(current, "paused producer info matches pinned state"), state


def validate_sender_ready(record, count, before_ns, after_ns):
    require(isinstance(record, dict) and record.get("status") == "ready" and
            type(record.get("protocol")) is int and record["protocol"] == 1 and
            type(record.get("count")) is int and record["count"] == count and
            type(record.get("ready_ns")) is int and before_ns <= record["ready_ns"] <= after_ns,
            "Malformed, stale or wrong-corpus sender readiness")
    return record


def wait_sender_ready(child, count, observer, journal, before_ns, timeout=60):
    """Bounded stdio handshake, outside the single immutable measurement window."""
    data = bytearray()
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        selector.register(child.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            require(child.poll() is None, "Sender exited before readiness")
            if selector.select(min(.1, max(0, deadline - time.monotonic()))):
                part = os.read(child.stdout.fileno(), 4097 - len(data))
                require(part, "Sender closed readiness channel")
                data.extend(part)
                require(len(data) <= 4096, "Oversized sender readiness")
                if b"\n" in data:
                    line, remainder = bytes(data).split(b"\n", 1)
                    require(not remainder, "Unexpected output before sender start")
                    return validate_sender_ready(json.loads(line), count, before_ns, time.monotonic_ns())
            try:
                observer.sample()
            except Exception as error:
                journal.emit("error", stage="observe", category=type(error).__name__)
    raise RuntimeError("Sender readiness timed out; sample invalid, not rescheduled")


def append_runtime_end_sample(case, pids, storage, monitor_errors, scheduler=False, logical_ticks=None):
    """AFTER monitor join, BEFORE postprocessing; no concurrent file writer."""
    try:
        final = sample(pids, include_scheduler=scheduler)
        if logical_ticks is not None:
            final['logical_cpu_ticks'] = logical_ticks()
        with (case / 'host.jsonl').open('a') as output:
            output.write(json.dumps(final) + '\n')
        if final['memory_kib'].get('MemAvailable', 0) < 1024 * 1024:
            monitor_errors.append('low-memory-final-sample')
        if shutil.disk_usage(case).free < storage['reserve_bytes']:
            monitor_errors.append('disk-reserve-exhausted-final-sample')
        if any(p.get('unavailable') for p in final['processes'].values()):
            monitor_errors.append('node-exited-final-sample')
        return True
    except Exception as error:
        monitor_errors.append('final-sample:' + type(error).__name__)
        return False


def end_runtime_observation(stop, thread, case, pids, storage, monitor_errors, scheduler=False, logical_ticks=None):
    stop.set()
    thread.join()
    return append_runtime_end_sample(case, pids, storage, monitor_errors, scheduler, logical_ticks)


def finish_runtime_cpu_evidence(row, case, pids, storage, monitor_errors, scheduler=False, logical_ticks=None):
    """Analyze retained phases; NEVER extend ready-v1 active time with JSON postprocessing."""
    start = row['finality'].get('sender_start')
    if start is None:
        # The HTTP-only instrument has no readiness/end callback protocol.
        append_runtime_end_sample(case, pids, storage, monitor_errors, scheduler, logical_ticks)
    host = [json.loads(line) for line in (case / 'host.jsonl').read_text().splitlines()]
    row['runtime_cpu_evidence_full_lifecycle'] = runtime_cpu_evidence(host)
    if start is not None:
        require(type(start.get('protocol')) is int and start['protocol'] == 1 and
                start.get('window_fixed_once_before_sending') is True,
                'Unknown phase policy; do not reinterpret old evidence')
        row['runtime_cpu_evidence'] = active_runtime_cpu_evidence(host, start['origin_ns'], start['observation_end_ns'])
        if start.get('cpu_boundary_recorded') is not True:
            monitor_errors.append('missing-active-end-sample')
            row['runtime_cpu_evidence'].update(status='inconclusive', reason='active-end capture missing or failed')
        row['runtime_cpu_policy'] = 'ready-v1: full warmup/send/drain with complete bracketing CPU intervals; lifecycle retained'
    else:
        row['runtime_cpu_evidence'] = row['runtime_cpu_evidence_full_lifecycle']
        row['runtime_cpu_policy'] = 'whole lifecycle; no ready protocol'
    return host


def measure_presigned(args, sender, corpus, producer_port):
    """Pre-signed C++ P2P load; observer is a separate node, same Linux clock."""
    root = args.evidence.parent
    observer_path = root / "observer.jsonl"
    sends_path = root / "p2p-sends.jsonl"
    boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    journal = Journal(observer_path)
    observer = Observer(args.observer_endpoint, args.chain_id, journal, args.timeout)
    child = None
    try:
        observer.initialize()
        count = args.rate * (args.seconds + args.warmup)
        command = [str(sender), "--transactions", str(corpus / "transactions.json"),
                   "--evidence", str(sends_path), "--port", str(producer_port),
                   "--rate", str(args.rate), "--count", str(count), "--start-stdin", "1"]
        with (root / "p2p-sender.log").open("x") as output:
            preload_started = time.monotonic_ns()
            child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=output)
            ready = wait_sender_ready(child, count, observer, journal, preload_started)
            with (root / "sender-ready.json").open("x") as ready_output:
                json.dump(ready, ready_output)
            # Both modes receive the SAME five-second post-ready lead. This
            # origin is fixed once, before sending; never shift it or retry a
            # late/failed sample. Preload duration is recorded, not throughput.
            origin = time.monotonic_ns() + 5_000_000_000
            start = origin + args.warmup * 1_000_000_000
            end = start + args.seconds * 1_000_000_000
            journal.emit("window", version=1, clock="monotonic_ns", start_ns=start, end_ns=end,
                         requested_tps=args.rate, chain_id=args.chain_id, sender="cpp-p2p", boot_id=boot,
                         sender_start_protocol=1, sender_preload_started_ns=preload_started,
                         sender_ready_ns=ready["ready_ns"], sender_post_ready_lead_seconds=5)
            child.stdin.write((str(origin) + "\n").encode()); child.stdin.flush()
            while time.monotonic_ns() < end + args.drain * 1_000_000_000:
                require(child.poll() in (None, 0), "Pre-signed P2P sender failed; retain the invalid sample")
                try:
                    observer.sample()
                except Exception as error:
                    journal.emit("error", stage="observe", category=type(error).__name__)
                time.sleep(args.poll)
            require(child.poll() == 0, "Sender did not complete inside fixed window plus drain")
            output.write(child.stdout.read(4096).decode())
        # Close observations and host sampling BEFORE expensive journal/corpus
        # JSON validation. Otherwise postprocessing silently extends the drain
        # and contaminates active CPU evidence with a single-core decoder.
        observer.sample()
        journal.emit("end")
        observation_end = time.monotonic_ns()
        end_callback = getattr(args, 'on_observation_end', None)
        cpu_boundary_recorded = bool(end_callback()) if end_callback is not None else False
        sends = validate_sends(read_events(sends_path), boot, count)
        entries = json.loads((corpus / "transactions.json").read_text())
        require([e["id"] for e in sends] == [e["id"] for e in entries[:count]], "Sender changed corpus or order")
        require([e["actions"] for e in sends] == [e["actions"] for e in entries[:count]], "Sender action counts differ")
        sends = [{**e, "due_ns": origin + i * 1_000_000_000 // args.rate} for i, e in enumerate(sends)]
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait(timeout=5)
        if child is not None:
            try:
                child.stdin.close()
            except OSError:
                pass  # Do not mask the original failure with cleanup EPIPE.
            child.stdout.close()
        journal.close()
    events = [*read_events(observer_path), *sends]
    with args.evidence.open("x") as output:
        for event in events:
            output.write(json.dumps(event) + "\n")
    return {**analyze(events), "sender_start": {
        "protocol": 1, "preload_started_ns": preload_started, "ready_ns": ready["ready_ns"],
        "preload_seconds": (ready["ready_ns"] - preload_started) / 1e9,
        "origin_ns": origin, "observation_end_ns": observation_end, "post_ready_lead_seconds": 5,
        "window_fixed_once_before_sending": True, "cpu_boundary_recorded": cpu_boundary_recorded}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodeos", type=Path, required=True)
    parser.add_argument("--fixture-exporter", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--quiet-gate", type=Path, required=True)
    parser.add_argument("--presigned-sender", type=Path, help="Optional bounded C++ P2P sender; avoids Python/RPC load bottleneck")
    parser.add_argument("--workers", default="0,4,8")
    parser.add_argument("--validator-workers", type=int, default=None,
                        help="Fix validator workers independently; omitted preserves matched-mode comparison")
    parser.add_argument("--scheduler-diagnostics", action="store_true",
                        help="Record process CPU and node thread scheduler counters; diagnostic overhead in EVERY mode")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--rate", type=int, default=500)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--drain", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--metrics", action="store_true", help="Enable stage instrumentation in BOTH modes; default measures without it")
    parser.add_argument("--iterations", type=int, default=0)
    parser.add_argument("--actions-per-transaction", type=int, default=1,
                        help="Actually invoke the contract 1..100 times in each atomic transaction")
    parser.add_argument("--compute-checksum", action="store_true",
                        help="Use arithmetic whose result affects persistent state, instead of the legacy empty loop")
    parser.add_argument("--unique-scopes", action="store_true",
                        help="Give EVERY transaction a unique table scope, not merely each 32-tx batch")
    parser.add_argument("--cdt-wasm", type=Path, help="Deploy the bounded complex_benchmark CDT fixture")
    parser.add_argument("--checksum-kind", choices=("xorshift", "sha256"), default="xorshift")
    parser.add_argument("--inline-actions", type=int, choices=(0, 100), default=0,
                        help="CDT batch root sends 100 inline work actions; requires one root per tx")
    parser.add_argument("--hot-percent", type=int, default=0)
    parser.add_argument("--test-block-cpu-us", type=int, default=None,
                        help="TEST ONLY fresh-genesis CPU cap, 200000..800000 us; identical for all modes. "
                             "Does not change production defaults, transaction caps, NET or billing")
    args = parser.parse_args()
    test_block_cpu_limit(args.test_block_cpu_us)
    workers = [int(v) for v in args.workers.split(",")]
    validator_workers(0, args.validator_workers)
    require(len(set(workers)) == len(workers) and 0 in workers and len(workers) > 1
            and all(0 <= w <= 16 for w in workers), "Need serial plus unique supported worker counts")
    require(1 <= args.rounds <= 10 and args.rate > 0 and args.seconds > 0 and args.warmup >= 0
            and args.drain > 0 and 1 <= args.batch_size <= 64 and 0 <= args.iterations <= 10000000
            and 0 <= args.hot_percent <= 100 and 1 <= args.actions_per_transaction <= 100, "Invalid limits")
    count = max(64, (args.seconds + args.warmup) * args.rate)
    require(count <= 262144, "Fixture transaction bound exceeded")
    require(not args.unique_scopes or (count <= 8192 and args.hot_percent == 0), "Unique-scope fixture bounds/hot conflict")
    require(not args.cdt_wasm or args.compute_checksum, "CDT contract requires checksum validation")
    require(args.checksum_kind != "sha256" or (args.cdt_wasm and args.compute_checksum and not args.inline_actions
            and args.iterations <= 100000), "SHA-256 fixture requires its CDT contract and bounded work")
    require(not args.inline_actions or (args.cdt_wasm and args.actions_per_transaction == 1), "Inline fixture needs CDT and one root")
    executed_actions = args.actions_per_transaction * (args.inline_actions + 1 if args.inline_actions else 1)
    require(count * executed_actions <= 2000000, "Fixture total action bound exceeded")
    delta = action_delta(args.iterations, args.compute_checksum, args.checksum_kind)
    require((args.seconds + args.warmup + args.drain + 30) * len(workers) < 1200,
            "Comparison would exceed the fixed fixture lifetime")
    nodeos, exporter, gate = (p.resolve(strict=True) for p in
                             (args.nodeos, args.fixture_exporter, args.quiet_gate))
    sender = args.presigned_sender.resolve(strict=True) if args.presigned_sender else None
    storage = preflight(args.output_parent)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-release-tps-", dir=args.output_parent.resolve()))
    frozen_paths = [nodeos, exporter, gate, Path(__file__),
        Path(__file__).with_name("parallel_execution_test.py"),
        Path(__file__).parent / "PerformanceHarness/finality_benchmark.py",
        Path(__file__).parent / "PerformanceHarness/finality_tps.py",
        Path(__file__).parent / "PerformanceHarness/finality_cpp_benchmark.py",
        Path(__file__).parent / "PerformanceHarness/benchmark_environment.py",
        Path(__file__).parent / "PerformanceHarness/parallel_replay_benchmark.py"]
    if sender:
        frozen_paths.append(sender)
    if args.cdt_wasm:
        args.cdt_wasm = args.cdt_wasm.resolve(strict=True)
        frozen_paths.append(args.cdt_wasm)
    frozen = {str(p): sha256(p) for p in frozen_paths}
    report = {"status": "running", "root": str(root), "inputs": frozen, "samples": [],
              "engine": "jit for producer, canonical fallback and validator",
              "parameters": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "storage": storage, "platform": platform.platform(), "logical_cpus": os.cpu_count(),
              "harness_pid": os.getpid(),
              "validator_mode": "matched" if args.validator_workers is None else "fixed",
              "workload": {"actions_per_transaction": args.actions_per_transaction,
                           "iterations_per_action": args.iterations, "compute_checksum": args.compute_checksum,
                           "checksum_kind": args.checksum_kind,
                           "row_delta_per_action": delta,
                           "executed_actions_per_transaction": executed_actions,
                           "inline_actions": args.inline_actions, "cdt_contract": bool(args.cdt_wasm),
                           "unique_scopes": args.unique_scopes,
                           "parallel_eligible": args.actions_per_transaction <= 16,
                           "action_layout": "same row within a tx; " + ("UNIQUE table scope for every tx" if args.unique_scopes else "64 round-robin scopes across txs")},
              "sender": "cpp-p2p" if sender else "python-http", "minimum_measured_send_fraction": .98,
              "finality_policy": "one genesis finalizer; independent producer and validator on the SAME host; not a multi-finalizer network capacity test",
              "policy": ("TEST ONLY block CPU capacity diagnostic; per-tx CPU/NET/billing unchanged"
                         if args.test_block_cpu_us is not None else "unchanged genesis CPU/NET") +
                        "; synthetic rows; no system-contract account quota",
              "production_default_performance_evidence": args.test_block_cpu_us is None}
    nodes = []
    print("Evidence: " + str(root), flush=True)
    try:
        for round_index in range(args.rounds):
            group = root / f"round-{round_index + 1}"
            group.mkdir()
            corpus = group / "corpus"
            env = {k: v for k, v in os.environ.items() if not k.startswith("PUREEOS_PARALLEL_")}
            env.update(PUREEOS_PARALLEL_RELEASE_EXPORT_DIR=str(corpus),
                       PUREEOS_PARALLEL_RELEASE_TRANSACTIONS=str(count),
                       PUREEOS_PARALLEL_RELEASE_WORK=str(args.iterations),
                       PUREEOS_PARALLEL_RELEASE_ACTIONS=str(args.actions_per_transaction),
                       PUREEOS_PARALLEL_RELEASE_COMPUTE=str(int(args.compute_checksum)),
                       PUREEOS_PARALLEL_RELEASE_UNIQUE_SCOPES=str(int(args.unique_scopes)),
                       PUREEOS_PARALLEL_RELEASE_INLINE_ACTIONS=str(args.inline_actions),
                       PUREEOS_PARALLEL_RELEASE_HOT_PERCENT=str(args.hot_percent))
            if args.cdt_wasm:
                env["PUREEOS_PARALLEL_RELEASE_CDT_WASM"] = str(args.cdt_wasm)
            if args.test_block_cpu_us is not None:
                env["PUREEOS_PARALLEL_RELEASE_TEST_BLOCK_CPU_US"] = str(args.test_block_cpu_us)
            with (group / "export.log").open("x") as output:
                exported = subprocess.run([str(exporter),
                    "--run_test=parallel_release_fixtures/export_fresh_jit_throughput_fixture",
                    "--report_level=detailed", "--color_output=no"], env=env, cwd=group,
                    stdout=output, stderr=subprocess.STDOUT, timeout=300)
            require(exported.returncode == 0, "Fresh release export failed")
            manifest = checked_manifest(corpus)
            require(manifest["fixture_kind"] == "fresh-profile2-jit-row-update", "Wrong fixture kind")
            require(manifest["actions_per_transaction"] == args.actions_per_transaction and
                    manifest["compute_checksum"] == args.compute_checksum and
                    manifest["loop_iterations"] == args.iterations and
                    manifest["unique_scopes"] == args.unique_scopes and
                    manifest["inline_actions"] == args.inline_actions and
                    manifest["cdt_contract"] == bool(args.cdt_wasm), "Exporter workload does not match request")
            genesis = json.loads((corpus / "genesis.json").read_text())
            resource_policy = fixture_resource_policy(genesis, manifest, args.test_block_cpu_us)
            require(genesis["initial_savanna"]["version"] == 2,
                    "Release test must use fresh profile 2")
            entries = json.loads((corpus / "transactions.json").read_text())
            require(len(entries) == count and len({e["id"] for e in entries}) == count, "Invalid corpus size/IDs")
            require(all(e["actions"] == args.actions_per_transaction for e in entries), "Wrong corpus action counts")
            require(all(e["executed_actions"] == executed_actions for e in entries), "Wrong executed action counts")
            if args.unique_scopes:
                require(len({e["scope"] for e in entries}) == count and len(manifest["scopes"]) == count,
                        "Transactions share business state despite unique-scope request")
            packets = group / "packets.jsonl"
            with packets.open("x") as output:
                for entry in entries:
                    output.write(json.dumps({**entry["packed"], "id": entry["id"], "action_count": entry["actions"]}) + "\n")
            # Rotate order, retaining every result; no fastest-run selection.
            order = workers[round_index % len(workers):] + workers[:round_index % len(workers)]
            for count_workers in order:
                case = group / f"workers-{count_workers}"
                case.mkdir()
                row = {"round": round_index + 1, "workers": count_workers,
                       "validator_workers": validator_workers(count_workers, args.validator_workers),
                       "resource_policy": resource_policy,
                       "corpus_sha256": sha256(corpus / "transactions.json"), "valid": False}
                report["samples"].append(row)
                require(all(sha256(Path(p)) == digest for p, digest in frozen.items()), "Frozen input changed")
                row["quiet_gate"] = wait_for_quiet_gate(gate, case / "quiet-gate")
                preflight(case)
                validator = Node(nodeos, case / "validator", corpus, manifest, row["validator_workers"],
                                 peer="listen", engine="jit", batch_size=args.batch_size)
                producer = Node(nodeos, case / "producer", corpus, manifest, count_workers,
                                peer=validator.endpoint, engine="jit", batch_size=args.batch_size)
                nodes.extend((validator, producer))
                config = producer.root / "config/config.ini"
                lines = ["producer-name = eosio", "enable-stale-production = true", "pause-on-startup = true",
                         "read-mode = head", "max-transaction-time = 100",
                         "performance-metrics = " + str(args.metrics).lower(),
                         "plugin = eosio::prometheus_plugin"]
                if count_workers:
                    lines.append("experimental-parallel-production = true")
                lines.extend("signature-provider = " + k for k in json.loads((corpus / "TEST-ONLY-keys.json").read_text()))
                with config.open("a") as output:
                    output.write("\n".join(lines) + "\n")
                for node in (validator, producer):
                    node.start("live", ["--genesis-json", str(corpus / "genesis.json")])
                row["node_pids"] = {"producer": producer.process.pid, "validator": validator.process.pid}
                for point in manifest["checkpoints"]:
                    validator.feed(point)
                    producer.check(point)
                row["live_resource_policy_before"] = {n.root.name: check_live_resource_policy(
                    n.rpc("get_consensus_parameters"), genesis["initial_configuration"])
                    for n in (producer, validator)}
                (case / "metrics-before.txt").write_text(scrape_stages(producer))
                stop_monitor = threading.Event()
                monitor_errors = []
                def monitor():
                    with (case / "host.jsonl").open("x") as output:
                        while not stop_monitor.is_set():
                            try:
                                state = sample([producer.process.pid, validator.process.pid],
                                               include_scheduler=args.scheduler_diagnostics)
                                output.write(json.dumps(state) + "\n"); output.flush()
                                if state["memory_kib"].get("MemAvailable", 0) < 1024 * 1024:
                                    monitor_errors.append("low-memory")
                                if shutil.disk_usage(case).free < storage["reserve_bytes"]:
                                    monitor_errors.append("disk-reserve-exhausted")
                                if any(p.get("unavailable") for p in state["processes"].values()):
                                    monitor_errors.append("observed-node-exited")
                                for proc in Path("/proc").iterdir():
                                    if not proc.name.isdigit():
                                        continue
                                    try:
                                        comm = (proc / "comm").read_text().strip()
                                        if re.fullmatch(r"cc1(?:plus)?|clang(?:\+\+)?(?:-\d+)?|rustc|cargo|cmake|g?make|ninja|lto1", comm):
                                            monitor_errors.append("concurrent-compiler:" + comm)
                                    except OSError:
                                        pass
                            except Exception as error:
                                monitor_errors.append(type(error).__name__)
                            stop_monitor.wait(1)
                monitor_thread = threading.Thread(target=monitor)
                monitor_thread.start()
                try:
                    producer.rpc("resume", namespace="producer")
                    measurement = SimpleNamespace(endpoint=f"http://127.0.0.1:{producer.port}",
                        observer_endpoint=f"http://127.0.0.1:{validator.port}", chain_id=manifest["chain_id"],
                        transactions=packets, evidence=case / "finality.jsonl", rate=args.rate,
                        seconds=args.seconds, warmup=args.warmup, drain=args.drain, poll=.1, timeout=10, inflight=32)
                    measurement.on_observation_end = lambda: end_runtime_observation(stop_monitor, monitor_thread,
                        case, [producer.process.pid, validator.process.pid], storage, monitor_errors, args.scheduler_diagnostics)
                    row["finality"] = (measure_presigned(measurement, sender, corpus, producer.p2p_port)
                                       if sender else measure(measurement))
                    producer.rpc("pause", namespace="producer")
                finally:
                    stop_monitor.set(); monitor_thread.join()
                finish_runtime_cpu_evidence(row, case, [producer.process.pid, validator.process.pid],
                    storage, monitor_errors, args.scheduler_diagnostics)
                events = list(read_events(case / "finality.jsonl"))
                sent = {e["id"] for e in events if e["type"] == "sent"}
                finalized = final_ids(events)
                info, producer_hash = pin_paused_head(producer)
                wait_for(lambda: validator.rpc("get_info")["head_block_id"] == info["head_block_id"], "validator catches head")
                validator_hash = validator.rpc("get_integrity_hash", namespace="producer")
                require(producer_hash == validator_hash, "Serial/parallel replicated state diverged")
                row["state"] = producer_hash
                row["checkpoint"] = {"block_num": info["head_block_num"], "block_id": info["head_block_id"],
                                     "lib": info["last_irreversible_block_num"],
                                     "integrity_hash": producer_hash["integrity_hash"]}
                # Ledger equality uses the pinned HEAD; finality throughput
                # still uses only the irreversible branch inside the window.
                capacity = []
                executed = head_business_ids(validator, info["head_block_id"], manifest["checkpoints"][-1]["block_id"], capacity)
                row["receipt_capacity"] = receipt_capacity(capacity,
                    json.loads((corpus / "genesis.json").read_text())["initial_configuration"])
                with (case / "block-receipts.json").open("x") as output:
                    json.dump(capacity, output)
                require(executed <= sent, "Unexpected business transaction in produced chain")
                totals = expected_row_totals(entries, executed, delta)
                for scope in manifest["scopes"]:
                    rows = validator.rpc("get_table_rows", {"code": "test", "scope": scope, "table": "rows",
                                                            "json": True, "limit": 2})["rows"]
                    require(len(rows) == 1 and int(rows[0]["value"]) == 10 + totals[scope], "Final row sum mismatch")
                (case / "metrics-after.txt").write_text(scrape_stages(producer))
                row["process_usage"] = {n.root.name: proc_usage(n.process.pid) for n in (producer, validator)}
                row["live_resource_policy_after"] = {n.root.name: check_live_resource_policy(
                    n.rpc("get_consensus_parameters"), genesis["initial_configuration"])
                    for n in (producer, validator)}
                for node in (producer, validator):
                    node.stop()
                row["producer_health_errors"] = producer_health_errors(producer.log.read_text())
                row["production_diagnostics_including_aborted_blocks"] = parallel_diagnostics(producer.log.read_text())
                row["production_admission_including_aborted_blocks"] = admission_diagnostics(producer.log.read_text())
                row["live_intake_dispatch_counts"] = intake_diagnostics(producer.log.read_text())
                match = re.search(r"Experimental parallel production totals: batches=(\d+), attempts=(\d+), "
                    r"succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)", producer.log.read_text())
                row["production_attempts_including_aborted_blocks"] = dict(zip(
                    ("batches", "attempts", "succeeded", "adopted", "serial_fallbacks"),
                    map(int, match.groups()))) if match else None
                check_adoption(count_workers, args.actions_per_transaction,
                               row["production_attempts_including_aborted_blocks"])
                row["finalized_action_tps"] = row["finality"]["finalized_success_tps"] * executed_actions
                row["valid"] = (not monitor_errors and not row["producer_health_errors"] and sent <= finalized and
                    row["finality"]["measurement_complete_without_sampling_or_sender_errors"] and
                    row["finality"]["sent_unique_tps"] >= .98 * args.rate and
                    not row["finality"]["rpc_error_events"] and not row["finality"]["cohort_unresolved_at_end"])
                row["monitor_errors"] = monitor_errors
                print(json.dumps({"round": round_index + 1, "workers": count_workers, "valid": row["valid"],
                                  "finalized_tps": row["finality"]["finalized_success_tps"]}), flush=True)
                require(not row["producer_health_errors"], "Producer lifecycle invariant failed; inspect retained log")
        report["comparison"] = summarize(report["samples"], workers, args.rounds)
        report["status"] = comparison_status(report["comparison"])
    except KeyboardInterrupt:
        report["status"] = "interrupted"
        report["error"] = "Operator interrupted; partial samples retained, no TPS comparison"
    except Exception as error:
        report["status"] = "failed"
        report["error_type"] = type(error).__name__
        report["error"] = str(error) if isinstance(error, RuntimeError) else "see retained logs"
    finally:
        cleanup_errors = []
        for node in reversed(nodes):
            try:
                node.stop(check=False)
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
        if cleanup_errors or not all(sha256(Path(p)) == digest for p, digest in frozen.items()):
            report["status"] = "failed"
            report["cleanup_errors"] = cleanup_errors
            report["input_integrity_or_cleanup_failure"] = True
        report['performance_release_eligible'] = performance_release_eligible(
            report.get('comparison', {}), report_status=report['status'],
            capacity_diagnostic=args.test_block_cpu_us is not None,
            instrumentation_enabled=args.metrics or args.scheduler_diagnostics)
        with (root / "report.json").open("x") as output:
            json.dump(report, output, indent=2); output.write("\n")
    print(json.dumps({"status": report["status"], "report": str(root / "report.json")}))
    return 0 if report["status"] == "passed" else 130 if report["status"] == "interrupted" else 2


if __name__ == "__main__":
    raise SystemExit(main())
