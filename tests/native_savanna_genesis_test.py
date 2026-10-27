#!/usr/bin/env python3
"""Isolated native-genesis CLI smoke test. Never touches an existing chain."""
import argparse
import datetime
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def create_keys(command):
    # Generated test-only keys stay in memory and a private temporary config.
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return dict(line.split(": ", 1) for line in result.stdout.splitlines() if ": " in line)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--profile", type=int, choices=(1, 2), default=1)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    nodeos = build / "programs/nodeos/nodeos"
    cleos = build / "programs/cleos/cleos"
    util = build / "programs/helios-util/helios-util"
    for binary in (nodeos, cleos, util):
        if not binary.is_file():
            parser.error(f"Build the required executable first: {binary}")
    keys = create_keys([str(cleos), "create", "key", "--to-console"])
    bls = create_keys([str(util), "bls", "create", "key", "--to-console"])
    with tempfile.TemporaryDirectory(prefix="pureeos-native-genesis-") as temporary:
        root = Path(temporary)
        config = root / "config"
        config.mkdir()
        http_port, p2p_port = free_port(), free_port()
        while p2p_port == http_port:
            p2p_port = free_port()
        genesis = {
            "initial_timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000"),
            "initial_key": keys["Public key"],
            "initial_savanna": {
                "version": args.profile,
                "finalizer_key": bls["Public key"],
                "proof_of_possession": bls["Proof of Possession"],
            },
        }
        genesis_path = root / "genesis.json"
        genesis_path.write_text(json.dumps(genesis), encoding="utf-8")
        config_path = config / "config.ini"
        with config_path.open("x", encoding="utf-8") as output:
            os.chmod(config_path, 0o600)
            output.write("\n".join([
                "plugin = eosio::producer_plugin",
                "plugin = eosio::chain_api_plugin",
                "plugin = eosio::producer_api_plugin",
                "producer-name = eosio",
                "enable-stale-production = true",
                "chain-state-db-size-mb = 64",
                "chain-state-db-guard-size-mb = 4",
                "resource-monitor-space-absolute-gb = 1",
                "http-server-address = 127.0.0.1:" + str(http_port),
                "p2p-listen-endpoint = 127.0.0.1:" + str(p2p_port),
                "signature-provider = " + keys["Public key"] + "=KEY:" + keys["Private key"],
                "signature-provider = " + bls["Public key"] + "=KEY:" + bls["Private key"],
                "",
            ]))

        def rpc(method, payload=None, namespace="chain"):
            request = urllib.request.Request(
                f"http://127.0.0.1:{http_port}/v1/{namespace}/{method}",
                data=json.dumps(payload or {}).encode(),
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=2) as response:
                return json.load(response)

        previous_lib, chain_id, checkpoint = 1, None, None
        for phase, extra in (
            ("genesis", ["--genesis-json", str(genesis_path)]),
            ("restart", []),
            ("replay", ["--replay-blockchain"]),
        ):
            log_path = root / f"{phase}.log"
            with log_path.open("w", encoding="utf-8") as log:
                process = subprocess.Popen(
                    [str(nodeos), "--data-dir", str(root / "data"), "--config-dir", str(config), *extra],
                    stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 60
                    info = None
                    while time.monotonic() < deadline:
                        if process.poll() is not None:
                            raise RuntimeError(f"{phase}: nodeos exited with code {process.returncode}")
                        try:
                            info = rpc("get_info")
                            # Run beyond the default 6-second vote-pause timeout in each phase.
                            if info["last_irreversible_block_num"] >= previous_lib + 16:
                                break
                        except (OSError, urllib.error.URLError):
                            pass
                        time.sleep(0.25)
                    else:
                        raise RuntimeError(f"{phase}: LIB did not advance, last info: {info}")
                    governance = rpc("get_upgrade_state")
                    supported = rpc("get_supported_protocol_features", {"exclude_disabled": True}, "producer")
                    names = {s["value"] for feature in supported for s in feature["specification"]}
                    expected = {spec["builtin_feature_codename"] for path in (config / "protocol_features").glob("*.json")
                                if (spec := json.loads(path.read_text()))["subjective_restrictions"]["enabled"]}
                    if names != expected or len(names) != len(supported):
                        raise RuntimeError("Protocol-feature API omitted or duplicated an enabled feature")
                    seen = set()
                    for feature in supported:
                        if not set(feature["dependencies"]).issubset(seen):
                            raise RuntimeError("Protocol-feature API dependency ordering is invalid")
                        seen.add(feature["feature_digest"])
                    if governance["profile_enabled"] != (args.profile == 2) or governance["sealed"]:
                        raise RuntimeError("Unexpected governance profile or sealing state")
                    block = rpc("get_block", {"block_num_or_id": 2})
                    if block["producer"] != "eosio" or not block["schedule_version"] & (1 << 31):
                        raise RuntimeError("First produced block is not native Savanna")
                    if chain_id is not None and info["chain_id"] != chain_id:
                        raise RuntimeError("Chain ID changed on restart/replay")
                    if checkpoint is not None:
                        restored = rpc("get_block", {"block_num_or_id": checkpoint[0]})
                        if restored["id"] != checkpoint[1]:
                            raise RuntimeError(f"{phase}: replaced the persisted reversible branch")
                    checkpoint = (info["head_block_num"], info["head_block_id"])
                    chain_id = info["chain_id"]
                    previous_lib = info["last_irreversible_block_num"]
                    print(f"{phase}: head={info['head_block_num']}, LIB={previous_lib}, native=true, profile={args.profile}", flush=True)
                except Exception:
                    # Prevent even test private keys from leaking through nodeos error logs.
                    details = log_path.read_text(encoding="utf-8", errors="replace")
                    for private in (keys["Private key"], bls["Private key"]):
                        details = details.replace(private, "<redacted test private key>")
                    lines = details.splitlines()
                    diagnostics = [line for line in lines if "Not producing block because no recent" not in line]
                    print("\n".join(diagnostics)[-24000:])
                    print("\n".join(lines[-5:]))
                    raise
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
                if process.returncode != 0:
                    raise RuntimeError(f"{phase}: unclean shutdown ({process.returncode})")
        print("Native Savanna nodeos genesis, restart and replay passed.")


if __name__ == "__main__":
    main()
