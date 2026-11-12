#!/usr/bin/env python3
"""Bounded, negative CLI checks. No valid corpus, wallet or running chain needed."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sender", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, help="Optional existing signed fixture for pre-connect metadata rejection checks")
    args = parser.parse_args()
    sender = args.sender.resolve(strict=True)
    root = args.work_dir.resolve()
    root.mkdir(mode=0o700)
    base = ["--transactions", str(root / "missing.json"), "--evidence", str(root / "must-not-exist.jsonl"),
            "--port", "9876", "--rate", "100", "--count", "100", "--origin-ns", "1000000000"]
    cases = [("help", ["--help"], 0), ("missing-options", [], 2)]
    for label, index, value in (("zero-port", 5, "0"), ("large-port", 5, "65536"),
                              ("negative-rate", 7, "-1"), ("zero-rate", 7, "0"),
                              ("large-rate", 7, "1000001"), ("zero-count", 9, "0"),
                              ("large-count", 9, "262145"), ("junk-origin", 11, "10x"),
                              ("unknown-option", 0, "--unknown"), ("repeated-option", 0, "--rate")):
        command = base.copy()
        command[index] = value
        cases.append((label, command, 2))
    report = []
    for label, options, expected in cases:
        completed = subprocess.run([str(sender), *options], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, timeout=15)
        (root / f"{label}.log").write_text(completed.stdout)
        passed = completed.returncode == expected and not (root / "must-not-exist.jsonl").exists()
        report.append({"case": label, "expected": expected, "actual": completed.returncode, "passed": passed})
    if args.corpus:
        entry = json.loads(args.corpus.read_text())[0]
        for label, declared in (("zero-declared-actions", 0), ("excess-declared-actions", 101),
                                ("mismatched-declared-actions", int(entry["actions"]) + 1)):
            fixture = root / f"{label}.json"
            fixture.write_text(json.dumps([{**entry, "actions": declared}]))
            options = base.copy()
            options[1], options[9], options[11] = str(fixture), "1", str(time.monotonic_ns() + 30_000_000_000)
            completed = subprocess.run([str(sender), *options], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, timeout=15)
            (root / f"{label}.log").write_text(completed.stdout)
            passed = completed.returncode == 2 and "failed in load-fixture" in completed.stdout and not (root / "must-not-exist.jsonl").exists()
            report.append({"case": label, "expected": 2, "actual": completed.returncode, "passed": passed})
    (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": sum(r["passed"] for r in report), "cases": len(report)}))
    return 0 if all(r["passed"] for r in report) else 2


if __name__ == "__main__":
    raise SystemExit(main())
