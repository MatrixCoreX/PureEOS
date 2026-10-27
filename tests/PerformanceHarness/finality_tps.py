#!/usr/bin/env python3
"""PureEOS v1 evidence reader. No third-party dependencies and no RPC side effects.

The JSONL stream comes from one observer's monotonic clock. Block IDs and parent
links, not block heights or producer timestamps, determine the canonical branch.
Only explicitly sent business IDs with executed receipts count as transactions.
"""
import argparse
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def quantiles(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "p50_ms": None, "p95_ms": None, "p99_ms": None}
    return {"count": len(values), **{
        f"p{p}_ms": values[max(0, math.ceil(len(values) * p / 100) - 1)] / 1e6
        for p in (50, 95, 99)}}


def analyze(events):
    events = list(events)
    windows = [e for e in events if e.get("type") == "window"]
    require(len(windows) == 1, "exactly one measurement window required")
    window = windows[0]
    require(window.get("version") == 1 and window.get("clock") == "monotonic_ns",
            "unsupported evidence version or clock")
    start, stop = window["start_ns"], window["end_ns"]
    require(isinstance(start, int) and isinstance(stop, int) and 0 <= start < stop,
            "invalid fixed observation window")
    data = [e for e in events if e["type"] != "window"]
    require(all(isinstance(e.get("at_ns"), int) and e["at_ns"] >= 0 for e in data),
            "events require nonnegative monotonic nanoseconds")
    data.sort(key=lambda e: e["at_ns"])
    anchors = [e for e in data if e["type"] == "anchor"]
    require(len(anchors) == 1 and anchors[0]["at_ns"] <= start, "one pre-window anchor required")
    anchor = anchors[0]
    blocks = {anchor["id"]: {"id": anchor["id"], "num": anchor["num"], "receipts": []}}
    lib = anchor["id"]
    sends, acks, finalized, inclusions = {}, {}, {}, {}
    finalized_blocks, branch = set(), set()
    samples, errors, lags = [], [], []
    duplicate_sends = 0
    sent_attempts = 0
    final_end = None

    def path(head, ancestor):
        result, visited = [], set()
        while head != ancestor:
            require(head in blocks and head not in visited, "missing or cyclic block ancestry")
            visited.add(head)
            block = blocks[head]
            require("previous" in block and block["previous"] in blocks, "missing parent block")
            require(block["num"] == blocks[block["previous"]]["num"] + 1, "noncontiguous block numbers")
            result.append(head)
            head = block["previous"]
        return list(reversed(result))

    for event in data:
        kind, now = event["type"], event["at_ns"]
        if kind == "anchor":
            continue
        if kind == "sent":
            require(isinstance(event.get("actions", 1), int) and event.get("actions", 1) > 0,
                    "invalid action count")
            sent_attempts += start <= now < stop
            if event["id"] in sends:
                duplicate_sends += 1
            else:
                sends[event["id"]] = event
            if "due_ns" in event:
                lags.append(max(0, now - event["due_ns"]))
        elif kind == "ack":
            if event.get("accepted"):
                acks.setdefault(event["id"], now)
            else:
                errors.append(event)
        elif kind == "block":
            content = {key: event[key] for key in ("id", "num", "previous", "receipts")}
            require(all(r.get("status") in ("executed", "soft_fail", "hard_fail", "delayed", "expired")
                        and isinstance(r.get("id"), str) for r in content["receipts"]), "invalid receipt")
            if content["id"] in blocks:
                require(blocks[content["id"]] == content, "conflicting block contents")
            blocks[content["id"]] = content
        elif kind == "sample":
            require(now >= anchor["at_ns"], "sample precedes anchor")
            # The new LIB must descend from the previous one and lie on this head's branch.
            newly_final = path(event["lib_id"], lib)
            reversible = path(event["head_id"], event["lib_id"])
            current = finalized_blocks | set(newly_final) | set(reversible)
            branch = current
            for bid in newly_final + reversible:
                for receipt in blocks[bid]["receipts"]:
                    if receipt["status"] == "executed":
                        inclusions.setdefault((receipt["id"], bid), now)
            for bid in newly_final:
                for receipt in blocks[bid]["receipts"]:
                    if receipt["status"] == "executed":
                        finalized.setdefault(receipt["id"], (now, bid))
            finalized_blocks.update(newly_final)
            lib = event["lib_id"]
            samples.append(event)
        elif kind == "error":
            errors.append(event)
        elif kind == "end":
            require(final_end is None, "multiple end markers")
            final_end = now
        else:
            raise ValueError(f"unknown evidence event {kind}")

    require(final_end is not None and final_end >= stop, "incomplete evidence: no end after measurement window")
    require(not data or data[-1]["at_ns"] <= final_end, "events after end marker")
    require(samples and samples[0]["at_ns"] <= start and samples[-1]["at_ns"] >= stop,
            "samples must bracket the fixed observation window")
    require(all(txid in sends and at >= sends[txid]["at_ns"] for txid, at in acks.items()),
            "ack without an earlier send")
    for txid, (at, _) in finalized.items():
        if txid in sends:
            require(at >= sends[txid]["at_ns"], "business transaction finalized before send")

    in_window = lambda at: start <= at < stop
    successful = {txid: value for txid, value in finalized.items() if txid in sends}
    measured = {txid: value for txid, value in successful.items() if in_window(value[0])}
    included = {}
    for (txid, bid), at in inclusions.items():
        if bid in branch and txid in sends:
            require(at >= sends[txid]["at_ns"], "business transaction included before send")
            included[txid] = min(at, included.get(txid, at))
    cohort = {txid for txid, event in sends.items() if in_window(event["at_ns"])}
    seconds = (stop - start) / 1e9
    samples_ns = [s["at_ns"] for s in samples]
    instrumentation_errors = [e for e in errors if e["type"] == "error"]
    observer_errors = [e for e in instrumentation_errors if e.get("stage") == "observe"]
    return {
        "schema": "pureeos-finality-tps-v1", "window_seconds": seconds,
        "sent_unique": len(cohort), "send_attempts_in_window": sent_attempts,
        "duplicate_send_attempts_all": duplicate_sends,
        "sent_unique_tps": len(cohort) / seconds,
        "rpc_accepted_unique_tps_diagnostic": (None if window.get("sender") == "cpp-p2p" else
                                               sum(in_window(at) for at in acks.values()) / seconds),
        "canonical_included_unique_tps": sum(in_window(at) for at in included.values()) / seconds,
        "finalized_success_unique": len(measured), "finalized_success_tps": len(measured) / seconds,
        "finalized_actions_per_second": sum(sends[t].get("actions", 1) for t in measured) / seconds,
        "finalized_during_drain": sum(at >= stop for at, _ in successful.values()),
        "cohort_finalized_by_window_end": sum(t in successful and successful[t][0] < stop for t in cohort),
        "cohort_finalized_by_evidence_end": sum(t in successful for t in cohort),
        "cohort_unresolved_at_end": sorted(cohort - successful.keys()),
        "rpc_error_events": len(errors) - len(instrumentation_errors),
        "observer_error_events": len(observer_errors),
        "sender_missed_schedule": sum(e.get("stage") == "sender_capacity" for e in instrumentation_errors),
        "measurement_complete_without_sampling_or_sender_errors": not instrumentation_errors,
        "finalization_latency": quantiles(at - sends[t]["at_ns"] for t, (at, _) in measured.items()),
        "inclusion_latency": quantiles(at - sends[t]["at_ns"] for t, at in included.items() if in_window(at)),
        "send_schedule_lag": quantiles(lags),
        "max_observation_gap_ms": max((b - a for a, b in zip(samples_ns, samples_ns[1:])), default=0) / 1e6,
        "max_sample_collection_ms": max(s["at_ns"] - s.get("started_ns", s["at_ns"]) for s in samples) / 1e6,
        "max_head_lib_gap_blocks": max(blocks[s["head_id"]]["num"] - blocks[s["lib_id"]]["num"] for s in samples),
        "counting_rule": "first observed finalization in [start,end); successful sent IDs only; drain separate",
        "latency_clock": "single observer monotonic clock; includes polling and collection delay",
    }


def read_events(path):
    with Path(path).open() as source:
        for line in source:
            if line.strip():
                yield json.loads(line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(analyze(read_events(args.evidence)), indent=2, ensure_ascii=False) + "\n"
    if args.output:
        with args.output.open("x") as output:
            output.write(result)
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
