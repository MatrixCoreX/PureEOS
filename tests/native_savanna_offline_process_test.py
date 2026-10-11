#!/usr/bin/env python3
"""Real nodeos process/replay test with every network listener disabled.

Creates a new private test directory, never touches an existing chain, and only
signals its own child process. This tests SIGKILL recovery, NOT peer networking,
power loss, disk corruption, or system-contract business rules.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import tempfile
import time

from native_savanna_genesis_test import create_keys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--output-parent", type=Path, required=True)
    parser.add_argument("--profile", type=int, choices=(1, 2), default=2)
    parser.add_argument("--kill-cycles", type=int, default=1)
    parser.add_argument("--initial-lib", type=int, default=18)
    parser.add_argument("--check-corruption", action="store_true")
    args = parser.parse_args()
    if args.kill_cycles < 1 or args.initial_lib < 18:
        parser.error("kill-cycles must be positive and initial-lib must be at least 18")
    build = args.build_dir.resolve()
    nodeos = build / "programs/nodeos/nodeos"
    cleos = build / "programs/cleos/cleos"
    util = build / "programs/helios-util/helios-util"
    for binary in (nodeos, cleos, util):
        if not binary.is_file():
            parser.error(f"Missing executable: {binary}")
    if not args.output_parent.is_dir():
        parser.error("Output parent must already exist")
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="offline-nodeos-", dir=args.output_parent.resolve()))
    config = root / "config"
    config.mkdir()
    keys = create_keys([str(cleos), "create", "key", "--to-console"])
    bls = create_keys([str(util), "bls", "create", "key", "--to-console"])
    genesis = {
        "initial_timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000"),
        "initial_key": keys["Public key"],
        "initial_savanna": {"version": args.profile, "finalizer_key": bls["Public key"],
                            "proof_of_possession": bls["Proof of Possession"]},
    }
    genesis_path = root / "genesis.json"
    genesis_path.write_text(json.dumps(genesis), encoding="utf-8")
    (config / "config.ini").write_text("\n".join([
        "producer-name = eosio", "enable-stale-production = true",
        "chain-state-db-size-mb = 64", "chain-state-db-guard-size-mb = 4",
        "resource-monitor-space-absolute-gb = 1",
        "p2p-listen-endpoint =", "http-server-address =", "unix-socket-path =",
        "signature-provider = " + keys["Public key"] + "=KEY:" + keys["Private key"],
        "signature-provider = " + bls["Public key"] + "=KEY:" + bls["Private key"], "",
    ]), encoding="utf-8")
    base = [str(nodeos), "--data-dir", str(root / "data"), "--config-dir", str(config)]
    report = {"status": "running", "scope": __doc__, "root": str(root), "phases": [],
              "profile": args.profile, "kill_cycles": args.kill_cycles,
              "nodeos_sha256": hashlib.sha256(nodeos.read_bytes()).hexdigest(),
              "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    progress = re.compile(r"Produced block .* #([0-9]+) @ .* signed by eosio \[trxs: [0-9]+, lib: ([0-9]+)")

    def scrub(text):
        for secret in (keys["Private key"], bls["Private key"]):
            text = text.replace(secret, "<redacted test key>")
        return text

    def run_phase(label, minimum_lib, extra=(), kill=False, dirty=False):
        log_path = root / (label + ".log")
        observation = None
        with log_path.open("w", encoding="utf-8") as output:
            process = subprocess.Popen([*base, *extra], stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + (30 if dirty else max(120, args.initial_lib + 90))
                while time.monotonic() < deadline:
                    text = log_path.read_text(encoding="utf-8", errors="replace")
                    code = process.poll()
                    if dirty:
                        if code is not None:
                            if code != 2 or "atabase dirty flag set" not in text:
                                raise RuntimeError(f"{label}: expected explicit dirty-database refusal, exit={code}")
                            observation = {"phase": label, "dirty_database_rejected": True, "exit_code": code}
                            break
                    elif code is not None:
                        raise RuntimeError(f"{label}: nodeos exited early ({code})")
                    else:
                        matches = progress.findall(text)
                        if matches and int(matches[-1][1]) >= minimum_lib:
                            observation = {"phase": label, "head": int(matches[-1][0]),
                                           "lib": int(matches[-1][1]), "termination": "SIGKILL" if kill else "SIGTERM"}
                            break
                    time.sleep(0.25)
                if observation is None:
                    raise RuntimeError(f"{label}: no expected result within watchdog")
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
                    try:
                        process.wait(timeout=20)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                # Keep public diagnostics, not private signing credentials.
                log_path.write_text(scrub(log_path.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")
            expected = 2 if dirty else -signal.SIGKILL if kill else 0
            if process.returncode != expected:
                raise RuntimeError(f"{label}: unexpected termination {process.returncode}, wanted {expected}")
        report["phases"].append(observation)
        print(json.dumps(observation), flush=True)
        return observation

    def blocks():
        command = [str(util), "block-log", "--blocks-dir", str(root / "data/blocks")]
        subprocess.run([*command, "smoke-test"], check=True, capture_output=True, text=True, timeout=30)
        result = subprocess.run([*command, "print-log", "--print-from=block_log",
                                 "--as-json-array", "--no-pretty-print"],
                                check=True, capture_output=True, text=True, timeout=30)
        rows = json.loads(result.stdout)
        assert rows and all(b["producer"] == "eosio" and b["schedule_version"] == 1 << 31 for b in rows)
        assert [b["block_num"] for b in rows] == list(range(1, rows[-1]["block_num"] + 1))
        return {b["block_num"]: b["id"] for b in rows}

    def reject_corruption(label, damage, expected):
        # Deliberately damage only a fresh test copy, never the chain above.
        data = root / label / "data"
        shutil.copytree(root / "data/blocks", data / "blocks", ignore=shutil.ignore_patterns("fork_db.dat"))
        original_safety = next((root / "data").rglob("safety.dat"))
        safety = data / original_safety.relative_to(root / "data")
        safety.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original_safety, safety)
        records = sorted((data / "blocks/reversible/recovery").glob("*.block"))
        reversible = [p for p in records if int(p.stem[:8], 16) > max(final)]
        assert len(reversible) >= 2
        damage(reversible, safety)
        before = safety.read_bytes()
        result = subprocess.run([str(nodeos), "--data-dir", str(data), "--config-dir", str(config),
                                 "--replay-blockchain"], capture_output=True, text=True, timeout=60)
        diagnostic = scrub(result.stdout + result.stderr)
        (root / (label + ".log")).write_text(diagnostic)
        assert result.returncode != 0 and expected.lower() in diagnostic.lower(), (label, result.returncode)
        assert not progress.search(diagnostic), label + " must not produce after a recovery-integrity failure"
        assert safety.read_bytes() == before, label + " must not reset safety history"
        report["phases"].append({"phase": label, "rejected": True, "exit_code": result.returncode,
                                 "safety_preserved": True})
        print("PASS: fail closed: " + label, flush=True)

    print("Evidence: " + str(root), flush=True)
    try:
        first = run_phase("genesis", args.initial_lib, ["--genesis-json", str(genesis_path)])
        prior = blocks()
        assert max(prior) >= first["lib"]
        for cycle in range(args.kill_cycles):
            prefix = "" if args.kill_cycles == 1 else f"cycle-{cycle + 1}-"
            second = run_phase(prefix + "clean-restart-then-kill", max(prior) + 18, kill=True)
            crashed = blocks()
            assert all(crashed.get(n) == block_id for n, block_id in prior.items())
            recovery_dir = root / "data/blocks/reversible/recovery"
            saved = {int(p.stem[:8], 16): p.stem for p in recovery_dir.glob("*.block")}
            assert max(saved) >= second["head"]
            safety = next((root / "data").rglob("safety.dat"))
            safety_before = safety.read_bytes()
            assert safety_before
            # A dirty mmap database must stay rejected on repeated startup attempts.
            for attempt in range(3):
                run_phase(prefix + f"dirty-restart-{attempt + 1}", 0, dirty=True)
                assert safety.read_bytes() == safety_before
            recovered = run_phase(prefix + "replay-after-kill", second["lib"] + 18, ["--replay-blockchain"])
            restored = blocks()
            assert all(restored.get(n) == block_id for n, block_id in crashed.items())
            # Preserve the ACTUAL pre-crash reversible branch, not just LIB history.
            assert all(restored.get(n) == block_id for n, block_id in saved.items())
            assert "Restored " in (root / (prefix + "replay-after-kill.log")).read_text()
            prior = restored
        run_phase("restart-after-replay", recovered["lib"] + 18)
        final = blocks()
        assert all(final.get(n) == block_id for n, block_id in restored.items())
        report.update(status="passed", irreversible_blocks=len(final), preserved_crash_blocks=len(crashed),
                      preserved_reversible_head=max(saved), recovery_records=len(list(recovery_dir.glob("*.block"))))
        if args.initial_lib >= 260:
            assert min(int(p.stem[:8], 16) for p in recovery_dir.glob("*.block")) >= 257
            assert report["recovery_records"] < 256
            report["durable_pruning_checked"] = True
        if args.check_corruption:
            reject_corruption("committed-record-truncated",
                              lambda records, _: records[-1].write_bytes(b"torn committed record"),
                              "Truncated committed recovery block")
            reject_corruption("reversible-parent-missing", lambda records, _: records[0].unlink(),
                              "Missing reversible recovery ancestor")
            reject_corruption("safety-record-corrupted", lambda _, safety: safety.write_bytes(b"bad safety"),
                              "finalizer safety persistence file")
    except Exception as error:
        report.update(status="failed", error=scrub(str(error)))
        raise
    finally:
        (root / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("PASS: real offline nodeos startup, SIGKILL, dirty-state refusal, replay and restart", flush=True)


if __name__ == "__main__":
    main()
