#!/usr/bin/env python3
"""Real isolated 1->2->3->4 node admission, ranking, slot order and outage test.

Uses the real sealed system contract election, never bios/setprods or shared
producer keys. This covers the 1-4 node stage, NOT the full 21/210-node rotation.
All child processes stop on exit; private data, logs and public evidence remain.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import time
import traceback
import urllib.error
import urllib.request

from native_savanna_genesis_test import free_port
from pureeos_single_choice_local_test import LocalTest


def stop_owned_children(processes, stop):
    """Stop only explicit child handles, concurrently; collect every failure."""
    children = list({p.pid: p for p in processes if p is not None}.values())
    def close(process):
        try:
            stop(process)
            return None
        except Exception as error:
            return {'pid': process.pid, 'error': str(error)}
    with ThreadPoolExecutor(max_workers=min(8, max(1, len(children)))) as pool:
        return [error for error in pool.map(close, children) if error is not None]


class MultiNodeTest(LocalTest):
    local_signers = ("eosio",)  # Never let the bootstrap process sign for peers.
    names = ("eosio", "nodea", "nodeb", "nodec")

    def __init__(self, build, contracts):
        super().__init__(build, contracts, resource_split=True)
        self.peers = {}
        self.ports = {self.http_port, self.p2p_port}
        self.report.update(scope=__doc__, stages=[], outages=[], rankings=[], block_samples=[])
        self.report["test_script_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        self.report["binaries"] = {
            name: hashlib.sha256((build / f"programs/{name}/{name}").read_bytes()).hexdigest()
            for name in ("nodeos", "cleos")}

    def port(self):
        while True:
            port = free_port()
            if port not in self.ports:
                self.ports.add(port)
                return port

    def start_node(self, phase, extra=()):
        # Keep an absolute 8 GiB disk reserve even on a >90%-used test disk.
        super().start_node(phase, [*extra, "--resource-monitor-space-absolute-gb", "8",
                                   "--p2p-max-nodes-per-host", "10"])

    def register(self, name):
        self.action("regproducer", {"producer": name, "producer_key": self.accounts[name]["Public key"],
                                    "url": "", "location": 0}, name)
        self.action("regfinkey", {"finalizer_name": name, "finalizer_key": self.bls[name]["Public key"],
                                 "proof_of_possession": self.bls[name]["Proof of Possession"]}, name)

    def action(self, name, data, actor, *, contract="eosio", failure=None):
        if contract == "eosio.token" and name == "create":
            data = {**data, "maximum_supply": "2000000.0000 SYS"}
        elif contract == "eosio.token" and name == "issue":
            data = {**data, "quantity": "1000000.0000 SYS"}
        if name == "govinit" and failure is None:
            # Genesis authority is available only before sealing. These are normal
            # account/token actions, not a consensus/election bypass.
            self.register("eosio")
            self.accounts["nodec"], self.bls["nodec"] = self.key(), self.key(True)
            self.run(["wallet", "import", "-n", "localtest"], input_text=self.accounts["nodec"]["Private key"] + "\n")
            pub = self.accounts["nodec"]["Public key"]
            self.tx(["system", "newaccount", "eosio", "nodec", pub, pub,
                     "--buy-ram", "100.0000 SYS", "--stake-net", "10.0000 SYS", "--stake-cpu", "100.0000 SYS"])
            self.tx(["transfer", "eosio", "nodec", "100000.0000 SYS", "private node test"])
        return super().action(name, data, actor, contract=contract, failure=failure)

    def node_rpc(self, name, method, data=None):
        if name == "eosio":
            return self.rpc(method, data)
        return self.read_rpc(self.peers[name]["url"], method, data)

    def start_peer(self, name, restart=False):
        if not restart:
            root = self.root / name
            (root / "config").mkdir(parents=True)
            http, p2p = self.port(), self.port()
            config = ["plugin = eosio::producer_plugin", "plugin = eosio::chain_api_plugin",
                      "producer-name = " + name, "enable-stale-production = true",
                      "chain-state-db-size-mb = 128", "chain-state-db-guard-size-mb = 4",
                      "resource-monitor-space-absolute-gb = 8",
                      "p2p-max-nodes-per-host = 10",
                      f"http-server-address = 127.0.0.1:{http}",
                      f"p2p-listen-endpoint = 127.0.0.1:{p2p}",
                      f"p2p-peer-address = 127.0.0.1:{self.p2p_port}",
                      "verbose-http-errors = true"]
            for key in (self.accounts[name], self.bls[name]):
                config.append("signature-provider = " + key["Public key"] + "=KEY:" + key["Private key"])
            (root / "config/config.ini").write_text("\n".join(config) + "\n")
            self.peers[name] = {"root": root, "url": f"http://127.0.0.1:{http}", "p2p_port": p2p}
        peer = self.peers[name]
        peer["starts"] = peer.get("starts", 0) + 1
        extra = [] if restart else ["--genesis-json", str(self.root / "genesis.json")]
        peer["process"] = self.spawn([str(self.nodeos), "--data-dir", str(peer["root"] / "data"),
                                      "--config-dir", str(peer["root"] / "config"), *extra],
                                     name + (f"-restart-{peer['starts']}.log" if restart else "-join.log"))
        print(f"START {name}: pid={peer['process'].pid}, {peer['url']}", flush=True)

    def wait_for(self, description, predicate, timeout=150):
        deadline, next_notice = time.monotonic() + timeout, 0
        while time.monotonic() < deadline:
            for process in [self.node, self.wallet] + [p["process"] for p in self.peers.values() if not p.get("stopped")]:
                if process.poll() is not None:
                    raise RuntimeError(f"Child {process.pid} exited while waiting for {description}")
            try:
                result = predicate()
                if result:
                    return result
            except (OSError, urllib.error.URLError):
                pass
            if time.monotonic() >= next_notice:
                print("WAIT: " + description, flush=True)
                next_notice = time.monotonic() + 20
            time.sleep(0.5)
        raise RuntimeError("Timeout: " + description)

    def synced(self, names):
        infos = {name: self.node_rpc(name, "get_info") for name in names}
        if any(v["chain_id"] != self.report["chain_id"] for v in infos.values()):
            raise AssertionError("Peer chain ID mismatch")
        if max(v["last_irreversible_block_num"] for v in infos.values()) - min(v["last_irreversible_block_num"] for v in infos.values()) > 4:
            return False
        lib = min(v["last_irreversible_block_num"] for v in infos.values())
        if lib < 17:
            return False
        ids = {self.node_rpc(name, "get_block", {"block_num_or_id": lib})["id"] for name in names}
        if len(ids) != 1:
            raise AssertionError("Peers disagree on an irreversible block")
        return {"common_lib": lib, "common_block_id": next(iter(ids)), "infos": infos}

    def policies(self, names):
        expected = sorted(names)
        evidence = {}
        for name in names:
            schedule = self.node_rpc(name, "get_producer_schedule")
            finalizers = self.node_rpc(name, "get_finalizer_info")
            active = schedule["active"]
            policy = finalizers["active_finalizer_policy"]
            if ([p["producer_name"] for p in active["producers"]] != expected or
                    sorted(f["description"] for f in policy["finalizers"]) != expected or
                    policy["threshold"] != len(names) * 2 // 3 + 1 or
                    finalizers.get("pending_finalizer_policy") is not None or
                    schedule.get("pending") is not None or schedule.get("proposed") is not None):
                return False
            evidence[name] = {"schedule": active, "finalizer_policy": policy}
        return evidence

    @staticmethod
    def slot(timestamp):
        delta = datetime.datetime.fromisoformat(timestamp) - datetime.datetime(2000, 1, 1)
        return (delta.days * 86400 + delta.seconds) * 2 + delta.microseconds // 500000

    def sample_blocks(self, names, label, rounds=2):
        policies = self.wait_for(label + ": settled policies", lambda: self.policies(names))
        start = self.rpc("get_info")["head_block_num"] + 2
        span = 12 * len(names)
        target = start + span * (rounds + 1)
        self.wait_lib(target, timeout=120)
        blocks = []
        for number in range(start, target + 1):
            block = self.rpc("get_block", {"block_num_or_id": number})
            blocks.append({k: block[k] for k in ("block_num", "id", "timestamp", "producer", "schedule_version")})
        expected = sorted(names)
        sample = {"label": label, "expected_order": expected, "policies": policies,
                  "counts": dict(Counter(b["producer"] for b in blocks)), "blocks": blocks}
        self.report["block_samples"].append(sample)
        # Savanna deliberately fixes this legacy header field to 1<<31. The
        # changing proposer-policy version is exposed by get_producer_schedule,
        # not encoded in the lower bits of the legacy block header field.
        self.check(all(b["schedule_version"] == 1 << 31 and
                       b["producer"] == expected[(self.slot(b["timestamp"]) % span) // 12] for b in blocks),
                   label + ": every irreversible block obeys name-sorted 12-slot schedule")
        grouped = {}
        for block in blocks:
            grouped.setdefault(self.slot(block["timestamp"]) // span, []).append(block)
        full = [bs for bs in grouped.values() if len(bs) == span and
                all(self.slot(b["timestamp"]) == self.slot(bs[0]["timestamp"]) + i for i, b in enumerate(bs))]
        self.check(len(full) >= rounds, label + f": at least {rounds} complete rounds, 12 actual blocks per producer per round")
        self.check(set(b["producer"] for b in blocks) == set(names), label + ": every elected independent node really produces finalized blocks")
        sample["complete_rounds"] = len(full)
        return policies

    def stake(self, account, net, cpu, undelegate=False):
        kind = "unstake" if undelegate else "stake"
        data = {"from": account, "receiver": account,
                kind + "_net_quantity": f"{net:.4f} SYS", kind + "_cpu_quantity": f"{cpu:.4f} SYS"}
        if not undelegate:
            data["transfer"] = False
        self.action("undelegatebw" if undelegate else "delegatebw", data, account)

    def ranking(self, expected, label):
        ranked = self.ranked_all()
        rows = ranked["rows"]
        actual = {r["owner"]: int(r["total_votes"]) for r in rows}
        expected_units = {name: int(tokens * 10000) for name, tokens in expected.items()}
        order = [r["owner"] for r in rows]
        # Retain the actual response even when an assertion fails.
        self.report["rankings"].append({"label": label, "order": order, "votes_sys": expected,
                                        "actual_units": actual, "rpc": ranked})
        self.check(actual == expected_units and int(ranked["total_producer_vote_amount"]) == sum(expected_units.values()),
                   label + ": exact integer votes and total agree")
        self.check(order == sorted(expected, key=lambda name: (-expected[name], name)), label + ": RPC ranking follows votes")

    def outage(self, names):
        down = names[-1]
        print(f"OUTAGE: {len(names)} nodes, stopping {down}", flush=True)
        peer = self.peers[down]
        self.stop(peer["process"])
        peer["stopped"] = True
        # Drain already signed/in-flight votes before observing a stalled LIB.
        time.sleep(10)
        begin = self.rpc("get_info")
        time.sleep(30 if len(names) == 4 else 12)
        end = self.rpc("get_info")
        evidence = {"nodes": list(names), "stopped": down, "before": begin, "after": end}
        if len(names) < 4:
            self.check(end["last_irreversible_block_num"] == begin["last_irreversible_block_num"],
                       f"{len(names)} nodes: loss of one finalizer stops finality (threshold {len(names)})")
        else:
            self.check(end["last_irreversible_block_num"] >= begin["last_irreversible_block_num"] + 12,
                       "4 nodes: three surviving finalizers continue finality with one node offline")
            blocks = [self.rpc("get_block", {"block_num_or_id": i})
                      for i in range(begin["last_irreversible_block_num"] + 1, end["last_irreversible_block_num"] + 1)]
            self.check(all(b["producer"] != down for b in blocks), "Offline producer creates no new finalized blocks")
            slots = [self.slot(b["timestamp"]) for b in blocks]
            evidence.update(producer_counts=dict(Counter(b["producer"] for b in blocks)),
                            missing_slots_inside_sample=slots[-1] - slots[0] + 1 - len(slots),
                            block_timestamps=[b["timestamp"] for b in blocks])
        self.report["outages"].append(evidence)
        self.start_peer(down, restart=True)
        peer["stopped"] = False
        self.wait_lib(end["head_block_num"] + 12, timeout=90)
        self.wait_for(down + " recovered and synced", lambda: self.synced(names))
        self.check(True, f"{len(names)} nodes: restarting same node/data restores synchronization and finality")

    def double_outage(self):
        print("OUTAGE: 4 nodes, stopping nodeb and nodec together", flush=True)
        checkpoint = self.rpc("get_info")
        for name in ("nodeb", "nodec"):
            self.stop(self.peers[name]["process"])
            self.peers[name]["stopped"] = True
        time.sleep(10)
        begin = self.rpc("get_info")
        time.sleep(15)
        end = self.rpc("get_info")
        self.check(end["last_irreversible_block_num"] == begin["last_irreversible_block_num"],
                   "4 nodes: two offline cannot meet threshold 3, finality stops")
        self.report["outages"].append({"nodes": list(self.names), "stopped": ["nodeb", "nodec"],
                                        "before": begin, "after": end})
        # Recover just one first, proving that three (not necessarily all four)
        # are sufficient to resume finality after an actual quorum loss.
        self.start_peer("nodeb", restart=True)
        self.peers["nodeb"]["stopped"] = False
        self.wait_lib(end["head_block_num"] + 12, timeout=90)
        self.check(True, "Recovering a third of four nodes is sufficient to resume finality")
        self.start_peer("nodec", restart=True)
        self.peers["nodec"]["stopped"] = False
        self.wait_for("both failed nodes recovered", lambda: self.synced(self.names))
        number = checkpoint["last_irreversible_block_num"]
        self.check(all(self.node_rpc(n, "get_block", {"block_num_or_id": number})["id"] ==
                       checkpoint["last_irreversible_block_id"] for n in self.names),
                   "Recovery preserves the pre-failure irreversible block on all four nodes")

    def tests(self):
        self.tx(["system", "voteproducer", "approve", "alice", "eosio"])
        self.action("deposit", {"owner": "alice", "amount": "1600.0000 SYS"}, "alice")
        self.action("buyrex", {"from": "alice", "amount": "1600.0000 SYS"}, "alice")
        self.check(datetime.datetime.fromisoformat(self.table("globalv2")[0]["thresh_activated_stake_time"]) > datetime.datetime(1970, 1, 1),
                   "Small positive REX vote activates rewards without a 15% or fixed-supply threshold")
        self.check(self.table("stat", code="eosio.token", scope="SYS")[0]["supply"] == "1000000.0000 SYS",
                   "Only a small partial supply is issued; activation does not require upfront full issuance")
        expected = {"eosio": 1600}
        self.ranking(expected.copy(), "One producer")
        self.sample_blocks(["eosio"], "1-node genesis")
        for count, (name, voter, weight) in enumerate((("nodea", "bob", 200), ("nodeb", "renter", 400),
                                                       ("nodec", "nodec", 600)), start=2):
            names = self.names[:count]
            self.start_peer(name)
            self.wait_for(name + " initial P2P synchronization", lambda: self.synced(names))
            self.check(True, name + ": independent process synchronized same chain and irreversible block")
            self.register(name)
            self.check(name not in [p["producer_name"] for p in self.rpc("get_producer_schedule")["active"]["producers"]],
                       name + ": registration without positive votes does not elect the node")
            self.action("claimrewards", {"owner": name}, name, failure="standby rewards are disabled")
            self.check(True, name + ": registered but zero-output producer cannot claim rewards")
            self.stake(voter, weight // 2, weight // 2)
            self.tx(["system", "voteproducer", "approve", voter, name])
            expected[name] = weight
            self.ranking(expected.copy(), name + " voted in")
            policies = self.wait_for(name + " automatic election and finalizer activation", lambda: self.policies(names))
            self.check(True, f"{count} nodes: real contract elects all positive-vote producers; finalizer threshold={count * 2 // 3 + 1}")
            self.sample_blocks(names, f"{count}-node admission")
            self.report["stages"].append({"node_count": count, "new_node": name, "policies": policies,
                                            "synchronization": self.synced(names)})
            self.outage(names)

        # Keep a small independent vote on nodea so moving bob's entire vote
        # never requests a smaller committee (a separate, legacy restriction).
        self.stake("nodea", 50, 50)
        self.tx(["system", "voteproducer", "approve", "nodea", "nodea"])
        expected["nodea"] += 100
        self.ranking(expected.copy(), "Independent self vote")
        self.stake("bob", 250, 250)
        expected["nodea"] += 500
        self.ranking(expected.copy(), "Stake increase changes rank")
        self.stake("bob", 125, 125, undelegate=True)
        expected["nodea"] -= 250
        self.ranking(expected.copy(), "Unstake decreases vote immediately")
        self.tx(["system", "voteproducer", "approve", "bob", "nodeb"])
        expected["nodea"] -= 450
        expected["nodeb"] += 450
        self.ranking(expected.copy(), "Atomic single-choice switch moves all bob votes")
        self.check(self.voter("bob")["producers"] == ["nodeb"], "Switch leaves exactly one selected producer")
        self.sample_blocks(self.names, "After vote-ranking changes")
        self.double_outage()
        self.sample_blocks(self.names, "After double-failure recovery")
        self.action("deposit", {"owner": "renter", "amount": "2.0000 SYS"}, "renter")
        # Small voting balances now activate rewards but do not waive the REX
        # market's existing minimum economically useful lending liquidity.
        # Verify its rejection is atomic, then fund normal rentals separately.
        pool, fund = self.table("rexpool"), self.table("rexfund")
        self.action("rentcpu", {"from": "renter", "receiver": "renter", "loan_payment": "1.0000 SYS",
                                "loan_fund": "0.0000 SYS"}, "renter", failure="loan price does not favor renting")
        self.check(self.table("rexpool") == pool and self.table("rexfund") == fund and not self.table("cpuloan"),
                   "Small REX pool rejects an uneconomic rental without debiting funds or creating a loan")
        self.action("deposit", {"owner": "alice", "amount": "50000.0000 SYS"}, "alice")
        self.action("buyrex", {"from": "alice", "amount": "50000.0000 SYS"}, "alice")
        expected["eosio"] += 50000
        self.ranking(expected.copy(), "REX liquidity funded after small-vote activation")
        for resource in ("cpu", "net"):
            self.rent_with_custody_check(resource)
        self.check_resource_split("After four-node admission and voting changes")
        self.report["final_synchronization"] = self.wait_for("final agreement", lambda: self.synced(self.names))
        self.report["processes"] = {"eosio": {"pid": self.node.pid, "url": self.url},
                                      **{n: {"pid": p["process"].pid, "url": p["url"]} for n, p in self.peers.items()}}

    def execute(self):
        try:
            self.setup(register_candidates=False)
            self.tests()
            self.report["status"] = "passed"
        except (Exception, KeyboardInterrupt) as error:
            self.report["status"], self.report["error"] = "failed", self.redact(str(error))
            if isinstance(error, KeyboardInterrupt):
                self.report["status"] = "interrupted"
            if isinstance(error, urllib.error.HTTPError):
                self.report["error"] += ": " + self.redact(error.read().decode(errors="replace"))
            self.report["traceback"] = self.redact(traceback.format_exc())
            print("FAIL: " + self.report["error"], flush=True)
        finally:
            if self.report["status"] == "running":
                self.report["status"] = "interrupted"
            processes = [p.get("process") for p in reversed(list(self.peers.values()))] + [self.node, self.wallet]
            self.report['shutdown'] = dict(phase='stopping-owned-children', outcome_before_shutdown=self.report['status'],
                started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
            pending = self.root/'progress.pending'
            pending.write_text(json.dumps(self.report, indent=2)); pending.replace(self.root/'progress.json')
            for error in stop_owned_children(processes, self.stop):
                self.report['status'] = 'failed'
                self.report.setdefault('shutdown_errors', []).append(self.redact(str(error)))
            for log in self.logs:
                log.close()
            self.report["processes_stopped"] = all(p is None or p.poll() is not None for p in processes)
            self.report['shutdown'].update(phase='complete', finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
            (self.root / "report.json").write_text(json.dumps(self.report, indent=2))
            print(f"Report: {self.root / 'report.json'}", flush=True)
        return 0 if self.report["status"] == "passed" else 1


def main():
    os.umask(0o077)
    def terminate(_signum, _frame):
        raise KeyboardInterrupt("Test termination requested; stopping private children")
    signal.signal(signal.SIGTERM, terminate)
    parser = argparse.ArgumentParser(description=__doc__)
    project = Path(__file__).resolve().parents[1]
    parser.add_argument("--build-dir", type=Path, default=project / "build")
    parser.add_argument("--contracts-dir", type=Path, default=project.parent / "reference-contracts/build/pureeos-rotation-v2")
    args = parser.parse_args()
    return MultiNodeTest(args.build_dir.resolve(), args.contracts_dir.resolve()).execute()


if __name__ == "__main__":
    raise SystemExit(main())
