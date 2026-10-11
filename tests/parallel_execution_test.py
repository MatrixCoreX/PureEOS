#!/usr/bin/env python3
"""Compare an immutable business corpus in old/new, isolated nodeos processes.

No wallet, signer, external peer or existing data directory is used. Only our
own subprocesses receive signals. Artifacts (including failures) are retained.
This is a compatibility/recovery test, not a throughput or power-loss test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def checked_manifest(directory):
    manifest = json.loads((directory / "manifest.json").read_text())
    require(manifest["format"] == 1, "Unsupported corpus format")
    require(bool(manifest["checkpoints"]), "Empty corpus")
    for code_hash in [manifest["chain_id"], *manifest["code_hashes"]]:
        require(bool(re.fullmatch(r"[a-f0-9]{64}", code_hash)), "Invalid digest")
    last = 1
    for point in manifest["checkpoints"]:
        require(point["block_num"] == last + 1, "Corpus must start at block 2 and be contiguous")
        require(point["file"] == f"block-{point['block_num']}.json", "Unsafe corpus filename")
        require((directory / point["file"]).is_file(), "Missing corpus block")
        last = point["block_num"]
    for index, point in enumerate(manifest.get("forks", [])):
        require(point["file"] == f"fork-{index}.json", "Unsafe fork filename")
        require((directory / point["file"]).is_file(), "Missing fork block")
    return manifest


class Node:
    def __init__(self, binary, root, corpus, manifest, workers=None, peer=None, history=False):
        self.binary, self.root, self.corpus, self.manifest = binary, root, corpus, manifest
        self.workers, self.process, self.output = workers, None, None
        root.mkdir()
        (root / "config").mkdir()
        self.port, self.p2p_port = self.free_port(), self.free_port()
        while self.port == self.p2p_port:
            self.p2p_port = self.free_port()
        self.endpoint = f"127.0.0.1:{self.p2p_port}"
        lines = [
            "plugin = eosio::chain_api_plugin", "plugin = eosio::producer_api_plugin",
            "plugin = eosio::net_plugin", "chain-state-db-size-mb = 64",
            "chain-state-db-guard-size-mb = 4", "resource-monitor-space-absolute-gb = 1",
            "p2p-max-nodes-per-host = 16",  # this fixture intentionally shares loopback
            f"http-server-address = 127.0.0.1:{self.port}", "unix-socket-path =",
            f"p2p-listen-endpoint = {self.endpoint}" if peer else "p2p-listen-endpoint =",
            "wasm-runtime = eos-vm", "validation-mode = full", "http-validate-host = true",
        ]
        if peer and peer != "listen":
            lines.append(f"p2p-peer-address = {peer}")
        if history:
            lines.extend(["plugin = eosio::state_history_plugin", "trace-history = true",
                          "chain-state-history = true", "state-history-endpoint ="])
        if workers is not None:
            lines.extend([f"experimental-parallel-validation-threads = {workers}",
                          "experimental-parallel-validation-batch-size = 8"])
            lines.extend(f"experimental-parallel-code-hash = {h}" for h in manifest["code_hashes"])
        (root / "config/config.ini").write_text("\n".join(lines) + "\n")

    @staticmethod
    def free_port():
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            return sock.getsockname()[1]

    def rpc(self, method, payload=None, namespace="chain"):
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}/v1/{namespace}/{method}",
            data=json.dumps({} if payload is None else payload).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            raise RuntimeError(f"{self.root.name}/{method}: {error.read().decode()}") from error

    def command(self, extra):
        return [str(self.binary), "--data-dir", str(self.root / "data"), "--config-dir",
                str(self.root / "config"), *extra]

    def start(self, phase, extra=()):
        require(self.process is None, "Process already active")
        self.log = self.root / f"{phase}.log"
        self.output = self.log.open("x")
        self.process = subprocess.Popen(self.command(extra), stdout=self.output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            require(self.process.poll() is None, f"{self.root.name}/{phase} exited; see {self.log}")
            try:
                return self.rpc("get_info")
            except (OSError, urllib.error.URLError):
                time.sleep(0.1)
        raise RuntimeError(f"{phase} startup watchdog expired; see {self.log}")

    def stop(self, kill=False, check=True):
        if self.process is None:
            return
        process, self.process = self.process, None
        forced = False
        try:
            if process.poll() is None:
                process.send_signal(signal.SIGKILL if kill else signal.SIGTERM)
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    forced = True
                    process.kill()
                    process.wait(timeout=10)
        finally:
            self.output.close()
            self.output = None
        if check:
            require(not forced and process.returncode == (-signal.SIGKILL if kill else 0),
                    f"{self.root.name} unexpected stop {process.returncode}; see {self.log}")

    def check(self, point, lib_floor=0):
        deadline = time.monotonic() + 30
        while True:
            info = self.rpc("get_info")
            # Competing forks can have the SAME height; waiting on height alone
            # would race receipt of the replacement block over P2P.
            if info["head_block_id"] == point["block_id"] or time.monotonic() >= deadline:
                break
            time.sleep(0.1)
        require(info["chain_id"] == self.manifest["chain_id"], "Chain ID mismatch")
        require(info["head_block_id"] == point["block_id"],
                f"{self.root.name}: block {point['block_num']} ID mismatch ({info['head_block_num']})")
        # A trusted snapshot / irreversible log establishes a finalized floor;
        # it must never move backwards to an older QC in the first tail block.
        expected_lib = max(point["lib"], lib_floor)
        require(info["last_irreversible_block_num"] == expected_lib, "LIB mismatch")
        state = self.rpc("get_integrity_hash", namespace="producer")
        require(state["head_block_id"] == point["block_id"], "Integrity checkpoint moved")
        require(state["integrity_hash"] == point["integrity_hash"],
                f"{self.root.name}: state mismatch at block {point['block_num']}")
        return {"head": point["block_num"], "lib": expected_lib, "state": state["integrity_hash"]}

    def feed(self, point, lib_floor=0):
        self.rpc("push_block", json.loads((self.corpus / point["file"]).read_text()))
        # push_block historically returns HTTP success even when it drops an
        # execution error: acceptance MUST be proved independently by state.
        return self.check(point, lib_floor)

    def totals(self, phase, require_adoption=True):
        text = (self.root / f"{phase}.log").read_text(errors="replace")
        rows = re.findall(r"Experimental parallel validation totals: batches=(\d+), attempts=(\d+), "
                          r"succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)", text)
        if self.workers and require_adoption:
            require(len(rows) == 1 and int(rows[0][3]) > 0, f"No actual adoption: {self.root.name}/{phase}")
        return [dict(zip(("batches", "attempts", "succeeded", "adopted", "fallbacks"), map(int, row))) for row in rows]

    def history_digests(self):
        result = {}
        for stream in ("trace_history", "chain_state_history"):
            for suffix in ("log", "index"):
                name = f"{stream}.{suffix}"
                path = self.root / "data/state-history" / name
                require(path.is_file() and path.stat().st_size > 0, "Missing history artifact: " + str(path))
                result[name] = sha256(path)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-nodeos", type=Path, required=True)
    parser.add_argument("--new-nodeos", type=Path, required=True)
    parser.add_argument("--old-sha256", required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    args = parser.parse_args()
    old, new, corpus = args.old_nodeos.resolve(), args.new_nodeos.resolve(), args.corpus.resolve()
    require(old != new and old.is_file() and new.is_file(), "Distinct executable files are required")
    require(args.output_parent.is_dir(), "Output parent must already exist")
    old_hash, new_hash = sha256(old), sha256(new)
    require(old_hash == args.old_sha256, "Pre-change nodeos hash mismatch")
    manifest = checked_manifest(corpus)
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix="parallel-process-", dir=args.output_parent.resolve()))
    report = {"status": "running", "scope": __doc__, "old_sha256": old_hash, "new_sha256": new_hash,
              "script_sha256": sha256(Path(__file__)), "corpus_sha256": sha256(corpus / "manifest.json"),
              "phases": [], "root": str(root)}
    nodes = []

    def record(phase, **fields):
        item = {"phase": phase, **fields}
        report["phases"].append(item)
        print(json.dumps(item), flush=True)

    print("Evidence: " + str(root), flush=True)
    try:
        rejection_cases = [
            ("threads-17", ["--experimental-parallel-validation-threads", "17"], manifest),
            ("batch-zero", ["--experimental-parallel-validation-batch-size", "0"], manifest),
            ("batch-65", ["--experimental-parallel-validation-batch-size", "65"], manifest),
            ("bad-code-hash", ["--experimental-parallel-code-hash", "not-a-digest"], manifest),
            ("missing-code-hash", [], {**manifest, "code_hashes": []}),
            ("light-validation", ["--validation-mode", "light"], manifest),
            ("jit-runtime", ["--wasm-runtime", "eos-vm-jit"], manifest),
            ("trusted-producer", ["--trusted-producer", "eosio"], manifest),
            ("deep-mind", ["--deep-mind"], manifest),
        ]
        for label, options, configuration in rejection_cases:
            node = Node(new, root / ("reject-" + label), corpus, configuration, 2)
            result = subprocess.run(node.command(["--genesis-json", str(corpus / "genesis.json"), *options]),
                                    capture_output=True, text=True, timeout=60)
            diagnostic = result.stdout + result.stderr
            (node.root / "rejection.log").write_text(diagnostic)
            require(result.returncode != 0 and "experimental" in diagnostic.lower(),
                    label + ": expected explicit experimental configuration rejection")
            record("reject-" + label, exit_code=result.returncode)
        for workers in (None, 0, 1, 2, 4):
            label = "old-serial" if workers is None else f"new-{workers}"
            # The preserved baseline has a pre-existing JSON push_block cache
            # bug (tracked in the retained first-run failure). Feed THAT binary
            # through its unchanged P2P deserializer, not its broken HTTP path.
            feeder = None
            if workers is None:
                feeder = Node(new, root / (label + "-feeder"), corpus, manifest, 0, peer="listen")
                nodes.append(feeder)
                feeder.start("feed", ["--genesis-json", str(corpus / "genesis.json")])
            node = Node(old if workers is None else new, root / label, corpus, manifest, workers,
                        feeder.endpoint if feeder else None)
            nodes.append(node)
            node.start("feed", ["--genesis-json", str(corpus / "genesis.json")])
            for point in manifest["checkpoints"]:
                if feeder:
                    feeder.feed(point)
                    node.check(point)
                else:
                    node.feed(point)
            if feeder:
                feeder.stop()
            last = manifest["checkpoints"][-1]
            node.stop()
            record(label + "-every-block", checkpoints=len(manifest["checkpoints"]), totals=node.totals("feed"))
            node.start("restart")
            node.check(last)
            node.stop(kill=True)
            record(label + "-clean-restart-then-kill", head=last["block_num"])
            # A dirty database must not be silently reused after SIGKILL.
            dirty = subprocess.run(node.command([]), capture_output=True, text=True, timeout=60)
            (node.root / "dirty-refusal.log").write_text(dirty.stdout + dirty.stderr)
            require(dirty.returncode == 2 and "atabase dirty flag set" in dirty.stdout + dirty.stderr,
                    label + ": dirty state was not explicitly refused")
            node.start("checked-replay", ["--replay-blockchain", "--force-all-checks", "--disable-replay-opts"])
            node.check(last)
            node.stop()
            record(label + "-checked-recovery", totals=node.totals("checked-replay"))
            original_records = {p.name: sha256(p) for p in (node.root / "data/blocks/reversible/recovery").glob("*.block")}
            for damage, diagnostic in (("truncated", "Truncated or oversized committed recovery block"),
                                       ("missing-parent", "Missing reversible recovery ancestor")):
                damaged = Node(node.binary, root / (label + "-" + damage), corpus, manifest, workers)
                shutil.copytree(node.root / "data/blocks", damaged.root / "data/blocks",
                                ignore=shutil.ignore_patterns("fork_db.dat"))
                records = sorted(p for p in (damaged.root / "data/blocks/reversible/recovery").glob("*.block")
                                 if int(p.stem[:8], 16) > last["lib"])
                require(len(records) >= 2, "Fault test needs two reversible records")
                # Deliberately damage ONLY this newly created test copy.
                if damage == "truncated":
                    records[-1].write_bytes(b"torn committed recovery record")
                else:
                    records[0].unlink()
                failure = subprocess.run(damaged.command(["--replay-blockchain", "--force-all-checks", "--disable-replay-opts"]),
                                         capture_output=True, text=True, timeout=60)
                text = failure.stdout + failure.stderr
                (damaged.root / "failure.log").write_text(text)
                require(failure.returncode != 0 and diagnostic.lower() in text.lower(),
                        label + ": did not fail closed on " + damage)
                require("start listening on" not in text, "Invalid recovery reached API startup")
                record(label + "-reject-" + damage, exit_code=failure.returncode)
            require(original_records == {p.name: sha256(p) for p in (node.root / "data/blocks/reversible/recovery").glob("*.block")},
                    "Fault injection changed original records")
            if workers:
                node.start("disable-parallel", ["--experimental-parallel-validation-threads", "0"])
                node.check(last)
                node.stop()
                record(label + "-disable-without-state-conversion")

            # Restore a snapshot into a FRESH database, then accept the tail.
            # Creating in irreversible read mode avoids a snapshot RPC waiting
            # indefinitely for future blocks on this deliberately static chain.
            snapshotter = Node(node.binary, root / (label + "-snapshotter"), corpus, manifest, workers)
            nodes.append(snapshotter)
            shutil.copytree(corpus / "blocks", snapshotter.root / "data/blocks")
            info = snapshotter.start("snapshot-source", ["--read-mode", "irreversible",
                "--force-all-checks", "--disable-replay-opts"])
            snapshot = snapshotter.rpc("create_snapshot", namespace="producer")
            point = next(p for p in manifest["checkpoints"] if p["block_num"] == snapshot["head_block_num"])
            require(snapshot["head_block_id"] == point["block_id"], "Snapshot head mismatch")
            require(snapshotter.rpc("get_integrity_hash", namespace="producer")["integrity_hash"] == point["integrity_hash"],
                    "Snapshot source state mismatch")
            snapshot_path = Path(snapshot["snapshot_name"]).resolve()
            require(snapshot_path.is_relative_to(snapshotter.root) and snapshot_path.is_file(), "Unsafe snapshot path")
            snapshotter.stop()
            restored = Node(node.binary, root / (label + "-restored"), corpus, manifest, workers, peer="listen")
            nodes.append(restored)
            restored.start("snapshot-restore", ["--snapshot", str(snapshot_path)])
            state = restored.rpc("get_integrity_hash", namespace="producer")
            require(state["head_block_id"] == point["block_id"] and state["integrity_hash"] == point["integrity_hash"],
                    "Snapshot restored state mismatch")
            tail = [p for p in manifest["checkpoints"] if p["block_num"] > point["block_num"]]
            require(bool(tail), "Snapshot corpus needs a post-snapshot block")
            lib_floor = snapshot["head_block_num"]
            sender = Node(new, root / (label + "-tail-sender"), corpus, manifest, 0, peer=restored.endpoint)
            nodes.append(sender)
            shutil.copytree(corpus / "blocks", sender.root / "data/blocks")
            sender.start("snapshot-tail", ["--force-all-checks", "--disable-replay-opts"])
            for point in tail:
                sender.feed(point, lib_floor)
                restored.check(point, lib_floor)
            sender.stop()
            restored.stop()
            record(label + "-snapshot-restore", tail_blocks=len(tail), snapshot_sha256=sha256(snapshot_path))

        # A serial sender disseminates the original signed blocks. BOTH the old
        # and new observers receive and fully execute them through real P2P.
        source = Node(new, root / "p2p-serial-source", corpus, manifest, 0, peer="listen", history=True)
        nodes.append(source)
        source.start("network", ["--genesis-json", str(corpus / "genesis.json")])
        observers = []
        for workers in (None, 0, 1, 2, 4):
            label = "p2p-old" if workers is None else f"p2p-new-{workers}"
            observer = Node(old if workers is None else new, root / label, corpus, manifest, workers, source.endpoint, history=True)
            nodes.append(observer)
            observers.append(observer)
            observer.start("network", ["--genesis-json", str(corpus / "genesis.json")])
        for point in manifest["checkpoints"]:
            source.feed(point)
            for observer in observers:
                observer.check(point)
        history_reference = None
        for node in [source, *observers]:
            node.stop()
            history = node.history_digests()
            if history_reference is None:
                history_reference = history
            require(history == history_reference, node.root.name + ": trace/state-history stream mismatch")
            record(node.root.name, checkpoints=len(manifest["checkpoints"]), totals=node.totals("network"), history=history)

        # Competing reversible blocks through the SAME network path. The second
        # branch has a later slot and supersedes actual adopted first-branch writes.
        require(len(manifest.get("forks", [])) >= 3, "A real competing-fork corpus is required")
        source = Node(new, root / "fork-p2p-serial-source", corpus, manifest, 0, peer="listen", history=True)
        nodes.append(source)
        source.start("fork-network", ["--genesis-json", str(corpus / "genesis.json")])
        observers = []
        for workers in (None, 0, 1, 2, 4):
            label = "fork-p2p-old" if workers is None else f"fork-p2p-new-{workers}"
            observer = Node(old if workers is None else new, root / label, corpus, manifest, workers, source.endpoint, history=True)
            nodes.append(observer)
            observers.append(observer)
            observer.start("fork-network", ["--genesis-json", str(corpus / "genesis.json")])
        for point in [*manifest["checkpoints"][:manifest["fork_prefix_blocks"]], *manifest["forks"]]:
            source.feed(point)
            for observer in observers:
                observer.check(point)
        history_reference = None
        for node in [source, *observers]:
            node.stop()
            history = node.history_digests()
            if history_reference is None:
                history_reference = history
            require(history == history_reference, node.root.name + ": fork trace/state-history stream mismatch")
            record(node.root.name, competing_blocks=len(manifest["forks"]), totals=node.totals("fork-network"), history=history)
        require(sha256(old) == old_hash, "Baseline executable changed during test")
        report["status"] = "passed"
    except BaseException as error:
        report["status"], report["error"] = "failed", str(error)
        raise
    finally:
        for node in reversed(nodes):
            node.stop(check=False)
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: old/new process, recovery and mixed-version P2P comparison", flush=True)


if __name__ == "__main__":
    main()
