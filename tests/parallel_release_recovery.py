#!/usr/bin/env python3
"""Recover ONLY fresh copies of a stopped current-release benchmark chain.

No old binary/state, in-place replay, external peer, wallet or user chain is used.
Tests recovery, not throughput or physical power-loss durability. Retains copies.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from parallel_execution_test import Node, checked_manifest, require, sha256
sys.path.insert(0, str(Path(__file__).resolve().parent / "PerformanceHarness"))
from benchmark_environment import preflight, wait_for_quiet_gate


def block_hashes(directory):
    require(directory.is_dir() and not directory.is_symlink(), "Expected ordinary blocks directory")
    result = {}
    for path in sorted(directory.rglob("*")):
        require(not path.is_symlink(), "Refuse linked block artifacts")
        if path.is_file():
            result[str(path.relative_to(directory))] = sha256(path)
    require(result, "Missing block artifacts")
    return result


def select_sample(report, workers, round_index):
    require(report.get("status") == "passed", "Need a fully checked stopped benchmark attempt")
    return checked_sample(report, workers, round_index)


def checked_sample(report, workers, round_index):
    """Validate one checkpoint; caller separately enforces whole-report policy."""
    rows = [r for r in report["samples"] if r["workers"] == workers and r["round"] == round_index]
    require(len(rows) == 1 and rows[0].get("valid") and "checkpoint" in rows[0], "Missing valid source checkpoint")
    point = rows[0]["checkpoint"]
    require(0 < point["lib"] < point["block_num"], "Need both irreversible and reversible blocks")
    require(rows[0]["state"]["head_block_id"] == point["block_id"] and
            rows[0]["state"]["integrity_hash"] == point["integrity_hash"], "Inconsistent source checkpoint")
    return rows[0]


def recovery_closure_errors(blocks, original, frozen):
    """Missing evidence is a recorded failure, not an exception hiding the report."""
    errors=[]
    try:
        if block_hashes(blocks)!=original:errors.append('source-blocks-changed')
    except (OSError,RuntimeError,ValueError) as error:
        errors.append('source-block-verification-'+type(error).__name__)
    for path,digest in frozen.items():
        try:
            if sha256(Path(path))!=digest:errors.append('frozen-input-changed:'+path)
        except (OSError,RuntimeError,ValueError) as error:
            errors.append('frozen-input-unreadable:'+path+':'+type(error).__name__)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--quiet-gate", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--round", type=int, default=1)
    args = parser.parse_args()
    require(1 <= args.workers <= 16 and args.round > 0, "Unsupported worker/round")
    source_report = args.report.resolve(strict=True)
    document = json.loads(source_report.read_text())
    selected = select_sample(document, args.workers, args.round)
    source_root = source_report.parent
    require(Path(document["root"]).resolve() == source_root, "Source report root moved")
    binary = Path(document["parameters"]["nodeos"]).resolve(strict=True)
    require(sha256(binary) == document["inputs"][str(binary)], "Node is not the benchmarked binary")
    group = source_root / f"round-{args.round}"
    corpus = group / "corpus"
    manifest = checked_manifest(corpus)
    require(manifest["fixture_kind"] == "fresh-profile2-jit-row-update" and
            json.loads((corpus / "genesis.json").read_text())["initial_savanna"]["version"] == 2,
            "Require this version's fresh genesis")
    blocks = group / f"workers-{args.workers}" / "validator/data/blocks"
    original = block_hashes(blocks)
    frozen = {str(p): sha256(p) for p in (source_report, binary, Path(__file__),
              Path(__file__).with_name("parallel_execution_test.py"),
              Path(__file__).parent / "PerformanceHarness/benchmark_environment.py",
              args.quiet_gate.resolve(strict=True))}
    storage = preflight(args.output_parent)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-release-recovery-", dir=args.output_parent.resolve()))
    result = {"status": "running", "root": str(root), "source": str(source_report), "inputs": frozen,
              "source_blocks": original, "storage": storage, "checks": [], "scope": "current-version fresh copies only"}
    nodes = []
    point = selected["checkpoint"]
    print("Evidence: " + str(root), flush=True)

    def clone(label, workers, copy=True, peer=None):
        node = Node(binary, root / label, corpus, manifest, workers, peer=peer, engine="jit")
        nodes.append(node)
        if copy:
            shutil.copytree(blocks, node.root / "data/blocks")
            require(block_hashes(node.root / "data/blocks") == original, "Incomplete block copy")
        return node

    def record(label, **values):
        result["checks"].append({"name": label, **values})
        print(label + " passed", flush=True)

    try:
        result["quiet_gate"] = wait_for_quiet_gate(args.quiet_gate.resolve(), root / "quiet-gate")
        for workers in (0, args.workers):
            node = clone(f"replay-{workers}", workers)
            node.start("full-check-replay", ["--replay-blockchain", "--force-all-checks", "--disable-replay-opts"])
            node.check(point)
            node.stop()
            record(f"full-check-replay-{workers}", totals=node.totals("full-check-replay"))
            node.start("clean-restart")
            node.check(point)
            node.stop(kill=True)
            dirty = subprocess.run(node.command([]), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, timeout=60)
            (node.root / "dirty-refusal.log").write_text(dirty.stdout)
            require(dirty.returncode == 2 and "atabase dirty flag set" in dirty.stdout, "Dirty state was not refused")
            node.start("dirty-checked-replay", ["--replay-blockchain", "--force-all-checks", "--disable-replay-opts"])
            node.check(point)
            node.stop()
            record(f"restart-dirty-refusal-checked-recovery-{workers}", totals=node.totals("dirty-checked-replay"))

        snapshotter = clone("snapshot-source", args.workers)
        snapshotter.start("irreversible", ["--read-mode", "irreversible", "--force-all-checks", "--disable-replay-opts"])
        before = snapshotter.rpc("get_integrity_hash", namespace="producer")
        info = snapshotter.rpc("get_info")
        require(info["head_block_num"] == point["lib"], "Snapshot source must pin the original LIB")
        snapshot = snapshotter.rpc("create_snapshot", namespace="producer")
        path = Path(snapshot["snapshot_name"]).resolve(strict=True)
        require(path.is_relative_to(snapshotter.root) and path.is_file(), "Unsafe snapshot output path")
        require(snapshot["head_block_num"] == point["lib"] and snapshot["head_block_id"] == before["head_block_id"],
                "Snapshot moved")
        snapshotter.stop()
        restored = clone("snapshot-restored", args.workers, copy=False, peer="listen")
        restored.start("restore", ["--snapshot", str(path)])
        require(restored.rpc("get_integrity_hash", namespace="producer") == before, "Snapshot state mismatch")
        # The display-oriented get_block JSON is NOT a lossless signed block
        # transport (in particular for Savanna finality header extensions).
        # Sync the original signed reversible tail through the actual P2P path.
        sender = clone("snapshot-tail-sender", 0, peer=restored.endpoint)
        sender.start("snapshot-tail", ["--force-all-checks", "--disable-replay-opts"])
        sender.check(point)
        restored.check(point, lib_floor=point["lib"])
        sender.stop()
        restored.stop()
        record("snapshot-restore-and-reversible-tail", tail_blocks=point["block_num"] - point["lib"],
               transport="original signed blocks over loopback P2P", snapshot_sha256=sha256(path))
        result["status"] = "passed"
    except Exception as error:
        result["status"] = "failed"
        result["error_type"] = type(error).__name__
        result["error"] = str(error) if isinstance(error, RuntimeError) else "see retained logs"
    finally:
        failures = []
        for node in reversed(nodes):
            try:
                node.stop(check=False)
            except Exception as error:
                failures.append(type(error).__name__)
        failures.extend(recovery_closure_errors(blocks, original, frozen))
        if failures:
            result["status"] = "failed"
            result["cleanup_or_input_failure"] = failures
        with (root / "report.json").open("x") as output:
            json.dump(result, output, indent=2); output.write("\n")
    print(json.dumps({"status": result["status"], "report": str(root / "report.json")}))
    return 0 if result["status"] == "passed" else 2


if __name__ == "__main__":
    raise SystemExit(main())
