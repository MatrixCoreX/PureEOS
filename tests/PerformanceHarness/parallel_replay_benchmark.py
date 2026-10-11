#!/usr/bin/env python3
"""Fixed-corpus, fully checked replay comparison; NOT producer/finality TPS.

All modes read byte-identical finalized logs in fresh private directories.
Default three rotated-order repetitions include startup in wall timing, while
the node's own replay timer is recorded separately. Never operates an existing
chain or signer. No cache-dropping, affinity changes or system tuning.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from parallel_execution_test import Node, checked_manifest, require, sha256
from stage_report import compare as compare_stages


def scrape_stages(node):
    request = urllib.request.Request(f"http://127.0.0.1:{node.port}/v1/prometheus/metrics", b"{}",
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return response.read().decode()


def stages_since_process_start(text):
    # A fresh process starts every bounded histogram at zero. Retain original
    # scrape and derive a matching zero baseline without assuming stage names.
    zero = "\n".join(line.rsplit(" ", 1)[0] + " 0" if line.startswith("nodeos_stage_duration_seconds_")
                     else line for line in text.splitlines())
    return compare_stages(zero, text)


def proc_usage(pid):
    # Linux process-wide cumulative ticks and kernel high-water RSS. Read while
    # alive; do not approximate RSS by directory size or summed thread RSS.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    ticks = os.sysconf("SC_CLK_TCK")
    status = Path(f"/proc/{pid}/status").read_text()
    peak = re.search(r"^VmHWM:\s+(\d+) kB$", status, re.MULTILINE)
    require(peak is not None, "Missing process memory accounting")
    return {"cpu_seconds_through_sample": (int(fields[11]) + int(fields[12])) / ticks,
            "peak_rss_kib_through_sample": int(peak[1])}


def replay_timing(logs):
    rows = re.findall(r"replayed (\d+) blocks in (\d+) seconds, ([0-9.eE+-]+) ms/block", logs)
    require(len(rows) == 1, "Expected exactly one replay timer")
    count, truncated_seconds, milliseconds = int(rows[0][0]), int(rows[0][1]), float(rows[0][2])
    # Both reference and experimental controller log whole integer seconds;
    # their fractional ms/block denominator is head-start, i.e. count-1.
    # Recover elapsed from that fractional field, never divide by rounded 0.
    elapsed = milliseconds * (count - 1) / 1000
    require(count > 1 and elapsed > 0 and truncated_seconds <= elapsed < truncated_seconds + 1.001,
            "Inconsistent replay timer")
    return count, elapsed


def summarize(samples):
    result = {}
    for workload in sorted({row["workload"] for row in samples}):
        groups = {}
        for label in ("old", "new-0", "new-1", "new-2", "new-4"):
            rows = [row for row in samples if row["workload"] == workload and row["mode"] == label]
            if not rows:
                continue
            seconds = [row["replay_seconds"] for row in rows]
            groups[label] = {"runs": len(rows), "replay_seconds_median": statistics.median(seconds),
                "replay_seconds_min": min(seconds), "replay_seconds_max": max(seconds),
                "peak_rss_kib_max": max(row["peak_rss_kib_through_sample"] for row in rows),
                "cpu_seconds_median": statistics.median(row["cpu_seconds_through_sample"] for row in rows)}
        if "old" in groups:
            for row in groups.values():
                row["replay_speed_ratio_vs_old"] = groups["old"]["replay_seconds_median"] / row["replay_seconds_median"]
        result[workload] = groups
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-nodeos", required=True, type=Path)
    parser.add_argument("--new-nodeos", required=True, type=Path)
    parser.add_argument("--old-sha256", required=True)
    parser.add_argument("--corpora", required=True, type=Path)
    parser.add_argument("--output-parent", required=True, type=Path)
    parser.add_argument("--repetitions", type=int, default=3)
    args = parser.parse_args()
    old, new = args.old_nodeos.resolve(), args.new_nodeos.resolve()
    require(old != new and old.is_file() and new.is_file(), "Distinct binaries required")
    require(sha256(old) == args.old_sha256, "Old reference changed")
    require(args.output_parent.is_dir() and 1 <= args.repetitions <= 20, "Invalid output or repetitions")
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-replay-benchmark-", dir=args.output_parent.resolve()))
    report = {"status": "running", "scope": __doc__, "script_sha256": sha256(Path(__file__)),
        "old_sha256": sha256(old), "new_sha256": sha256(new), "samples": [],
        "host": {"platform": platform.platform(), "logical_cpus": os.cpu_count(),
                 "loadavg_start": os.getloadavg(), "cpuinfo": Path("/proc/cpuinfo").read_text(),
                 "meminfo_start": Path("/proc/meminfo").read_text()}, "corpora": {}}
    print("Evidence: " + str(root), flush=True)
    try:
        for workload in ("independent", "hot", "mixed", "serial"):
            corpus = args.corpora.resolve() / workload
            manifest = checked_manifest(corpus)
            point = manifest["replay_checkpoint"]
            files = {name: sha256(corpus / name) for name in ("manifest.json", "genesis.json", "blocks/blocks.log", "blocks/blocks.index")}
            report["corpora"][workload] = files
            for repeat in range(args.repetitions):
                modes = [("old", None), ("new-0", 0), ("new-1", 1), ("new-2", 2), ("new-4", 4)]
                modes = modes[repeat % 5:] + modes[:repeat % 5]
                for label, workers in modes:
                    node = Node(old if workers is None else new, root / f"{workload}-{repeat}-{label}",
                                corpus, manifest, workers)
                    shutil.copytree(corpus / "blocks", node.root / "data/blocks")
                    started = time.monotonic()
                    try:
                        node.start("replay", ["--performance-metrics", "--plugin", "eosio::prometheus_plugin",
                                             "--genesis-json", str(corpus / "genesis.json"),
                                             "--force-all-checks", "--disable-replay-opts"])
                        ready = time.monotonic() - started
                        usage = proc_usage(node.process.pid)
                        state = node.check(point, lib_floor=point["block_num"])
                        metrics = scrape_stages(node)
                        (node.root / "metrics.txt").write_text(metrics)
                        stages = stages_since_process_start(metrics)
                        node.stop()
                        logs = node.log.read_text(errors="replace")
                        count, seconds = replay_timing(logs)
                        require(count == point["block_num"] - 1, "Replay block count differs")
                        totals = node.totals("replay", require_adoption=workload != "serial")
                        row = {"workload": workload, "repeat": repeat, "mode": label, "state": state,
                            "fully_checked_replayed_blocks": count, "replay_seconds": seconds,
                            "validated_blocks_per_second": count / seconds,
                            "startup_to_api_ready_seconds": ready, "totals": totals, "stages": stages, **usage}
                        report["samples"].append(row)
                        print(json.dumps({key: value for key, value in row.items() if key != "stages"}), flush=True)
                    finally:
                        node.stop(check=False)
            require(files == {name: sha256(corpus / name) for name in files}, "Corpus changed during measurement")
        require(sha256(old) == report["old_sha256"] and sha256(new) == report["new_sha256"], "Binary changed during measurement")
        report["status"] = "passed"
    except BaseException as error:
        report["status"], report["error"] = "failed", str(error)
        raise
    finally:
        report["summary"] = summarize(report["samples"])
        report["host"]["loadavg_end"] = os.getloadavg()
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print("Report: " + str(root / "report.json"), flush=True)


if __name__ == "__main__":
    main()
