#!/usr/bin/env python3
"""Opt-in compiled fixture checks; no live node or TPS measurement.

Keeps positive and negative export evidence. Never edits an exported genesis.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from parallel_execution_test import require, sha256
from parallel_release_benchmark import fixture_resource_policy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture-exporter", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    args = parser.parse_args()
    exporter = args.fixture_exporter.resolve(strict=True)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="capacity-fixture-", dir=args.output_parent.resolve(strict=True)))
    digest = sha256(exporter)
    report = {"exporter": str(exporter), "sha256": digest, "status": "running", "cases": []}
    print("Evidence: " + str(root), flush=True)
    values = [None, "200000", "800000", "", "-1", "-0", "+800000", " 800000",
              "800000 ", "800000x", "800000.0", "0", "199999", "800001",
              "4294967296", "184467440737095516160000000000000000000000"]
    try:
        for index, value in enumerate(values):
            case = root / f"case-{index}"
            case.mkdir()
            destination = case / "corpus"
            env = {k: v for k, v in os.environ.items() if not k.startswith("PUREEOS_PARALLEL_")}
            env.update(PUREEOS_PARALLEL_RELEASE_EXPORT_DIR=str(destination),
                       PUREEOS_PARALLEL_RELEASE_TRANSACTIONS="64")
            if value is not None:
                env["PUREEOS_PARALLEL_RELEASE_TEST_BLOCK_CPU_US"] = value
            with (case / "export.log").open("x") as log:
                result = subprocess.run([str(exporter),
                    "--run_test=parallel_release_fixtures/export_fresh_jit_throughput_fixture",
                    "--report_level=detailed", "--color_output=no"], env=env, cwd=case,
                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
            valid = index < 3
            row = {"value": value, "expected_valid": valid, "exit_code": result.returncode}
            report["cases"].append(row)
            if valid:
                require(result.returncode == 0, "Expected valid fixture export")
                genesis = json.loads((destination / "genesis.json").read_text())
                manifest = json.loads((destination / "manifest.json").read_text())
                row["policy"] = fixture_resource_policy(genesis, manifest, None if value is None else int(value))
                transactions = json.loads((destination / "transactions.json").read_text())
                require(len(transactions) == 64 and len({t["id"] for t in transactions}) == 64,
                        "Invalid exported signed corpus")
            else:
                require(result.returncode != 0 and not destination.exists(),
                        "Invalid capacity must fail before creating a fixture")
            row["passed"] = True
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
    finally:
        if sha256(exporter) != digest:
            report["status"] = "failed"
            report["error"] = "Exporter changed during checks"
        with (root / "report.json").open("x") as output:
            json.dump(report, output, indent=2); output.write("\n")
    print(json.dumps({"status": report["status"], "cases": len(report["cases"]), "root": str(root)}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
