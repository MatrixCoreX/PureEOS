#!/usr/bin/env python3
"""22 independent loopback nodeos processes: 1->21 admission, FIFO and real outage.

Uses production clocks, elected schedules, independent producer/finalizer keys,
and native Savanna finality. No setprods, table writes or time acceleration.
Large transient state is isolated on a caller-selected filesystem; evidence,
logs and test-only keys remain under build/local-tests (private permissions).
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import signal
import tempfile
import time

from pureeos_multinode_local_test import MultiNodeTest
from pureeos_single_choice_local_test import LocalTest


class LargeMultiNodeTest(MultiNodeTest):
    names = ("eosio", *("node" + chr(ord("a") + i) for i in range(21)))

    def __init__(self, build, contracts, state_parent):
        super().__init__(build, contracts)
        self.state_root = Path(tempfile.mkdtemp(prefix="pureeos-22-state-", dir=state_parent))
        self.observed = set()
        self.last_policy = None
        self.last_version = None
        self.latest_rotation = None
        self.proposals = set()
        self.report.update(scope=__doc__, state_directory=str(self.state_root),
                           committee_events=[], proposals=[], observations=[], fault={},
                           test_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                           production_clock_unchanged=True, process_target=len(self.names),
                           local_rpc_budget_ms=200,
                           rpc_budget_note="Shared-host read/ABI budget only; consensus transaction CPU and production periods unchanged")
        self.report["support_script_sha256"] = {
            name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
            for name in ("pureeos_multinode_local_test.py", "pureeos_single_choice_local_test.py",
                         "native_savanna_genesis_test.py")}

    def spawn(self, command, log_name):
        if command[0] == str(self.build / "programs/keosd/keosd"):
            # Production-clock admission can take over an hour without wallet
            # traffic. This private disposable test wallet must remain usable
            # for the late fault/re-registration actions; it exits with the test.
            command = [*command, "--unlock-timeout", "14400"]
        if command[0] == str(self.nodeos):
            command = list(command)
            position = command.index("--data-dir") + 1
            original = Path(command[position])
            command[position] = str(self.state_root / original.relative_to(self.root))
            # Only these disposable RAM-filesystem chains use a 1 GiB reserve.
            # Existing/manual node settings and consensus CPU limits are untouched.
            options = {"--resource-monitor-space-absolute-gb": "1", "--p2p-max-nodes-per-host": "64",
                       "--chain-threads": "1", "--http-threads": "1", "--net-threads": "1",
                       "--vote-threads": "1", "--http-max-response-time-ms": "200",
                       "--abi-serializer-max-time-ms": "200"}
            for flag, value in options.items():
                if flag in command:
                    command[command.index(flag) + 1] = value
                else:
                    command += [flag, value]
        return super().spawn(command, log_name)

    def action(self, name, data, actor, *, contract="eosio", failure=None):
        if name == "govinit" and failure is None:
            # MultiNodeTest creates nodec, while the base creates nodea/nodeb.
            for account in self.names[4:]:
                self.accounts[account], self.bls[account] = self.key(), self.key(True)
                self.run(["wallet", "import", "-n", "localtest"],
                         input_text=self.accounts[account]["Private key"] + "\n")
                public = self.accounts[account]["Public key"]
                self.tx(["system", "newaccount", "eosio", account, public, public,
                         "--buy-ram", "100.0000 SYS", "--stake-net", "10.0000 SYS",
                         "--stake-cpu", "100.0000 SYS"])
                self.tx(["transfer", "eosio", account, "1000.0000 SYS", "isolated 22-node test"])
        return super().action(name, data, actor, contract=contract, failure=failure)

    def process_evidence(self):
        return {"eosio": {"pid": self.node.pid, "url": self.url},
                **{n: {"pid": p["process"].pid, "url": p["url"], "starts": p["starts"]}
                   for n, p in self.peers.items()}}

    def checkpoint(self):
        self.report["processes"] = self.process_evidence()
        (self.root / "progress.json").write_text(json.dumps(self.report, indent=2))

    def observe(self):
        info = self.rpc("get_info")
        schedule = self.rpc("get_producer_schedule")["active"]
        members = [p["producer_name"] for p in schedule["producers"]]
        finality = self.rpc("get_finalizer_info")
        policy = finality["active_finalizer_policy"]
        rotation = self.table("rotation")[0]
        self.latest_rotation = rotation
        if rotation["stage"] == 1 and rotation["proposer_version"] not in self.proposals:
            active, target = rotation["active"], rotation["target"]
            incoming, outgoing = rotation["incoming"], rotation["outgoing"]
            if target != [n for n in active if n != outgoing] + [incoming]:
                raise AssertionError("Proposal violated single-seat tenure ordering")
            if len(active) == 21 and not self.report["fault"] and outgoing != active[0]:
                raise AssertionError("Healthy full committee did not replace its oldest member")
            self.proposals.add(rotation["proposer_version"])
            self.report["proposals"].append(rotation)
        # Record each real membership policy, not just the contract's target.
        if schedule["version"] != self.last_version:
            if self.last_policy is not None:
                removed, added = set(self.last_policy) - set(members), set(members) - set(self.last_policy)
                if len(removed) > 1 or len(added) > 1 or not added:
                    raise AssertionError("Membership transition was not a single-seat change")
                if self.last_version is not None and schedule["version"] != self.last_version + 1:
                    raise AssertionError("Monitoring missed a producer-policy transition")
            if len(members) > 21:
                raise AssertionError("More than 21 active producers")
            self.report["committee_events"].append({"head": info["head_block_num"],
                "lib": info["last_irreversible_block_num"], "schedule": schedule,
                "finalizers": policy, "rotation": rotation})
            print(f"POLICY {schedule['version']}: {len(members)} members, "
                  f"incoming={rotation['incoming']}, outgoing={rotation['outgoing']}", flush=True)
            self.last_policy, self.last_version = members, schedule["version"]
            self.checkpoint()
        if sorted(f["description"] for f in policy["finalizers"]) == sorted(members):
            if policy["threshold"] != len(members) * 2 // 3 + 1:
                raise AssertionError("Incorrect finalizer quorum")
        producers = self.table("producersv2")
        # Reward ledger advances only through real accepted blocks. Finalized
        # per-producer block IDs are collected separately below.
        self.observed.update(p["owner"] for p in producers if int(p["unpaid_blocks"]) > 0)
        return info, members, rotation

    def scan_finalized(self, start, end, label):
        counts, first = Counter(), {}
        for number in range(start, end + 1):
            block = self.rpc("get_block", {"block_num_or_id": number})
            counts[block["producer"]] += 1
            first.setdefault(block["producer"], {key: block[key] for key in
                             ("block_num", "id", "timestamp", "producer")})
        evidence = {"label": label, "from": start, "through": end,
                    "counts": dict(counts), "first_finalized_blocks": first}
        self.report["observations"].append(evidence)
        return evidence

    def wait_observing(self, description, predicate, timeout):
        def check():
            state = self.observe()
            return predicate(*state)
        return self.wait_for(description, check, timeout=timeout)

    def candidates(self):
        return {r["owner"]: r for r in self.table("rotcands")}

    def producer(self, name):
        return next(r for r in self.table("producersv2") if r["owner"] == name)

    def fault_test(self):
        # Stop a returning ordinary entrant during policy activation, before its
        # complete observation round. Incumbents do not get to vote it out.
        def entrant(_info, _members, rotation):
            name = rotation["incoming"]
            return name if (len(rotation["target"]) == 21 and rotation["stage"] == 1 and
                            name != "eosio" and name in self.observed) else False
        down = self.wait_observing("returning ordinary entrant for real power-off", entrant, 900)
        before = self.rpc("get_info")
        balance = self.table("accounts", code="eosio.token", scope=down)
        self.stop(self.peers[down]["process"])
        self.peers[down]["stopped"] = True
        self.report["fault"] = {"node": down, "before": before, "balance_before": balance,
                                "candidate_before": self.candidates()[down], "progress": []}
        print("FAULT: stopped " + down + "; observing three full personal windows", flush=True)
        last = None

        def banned(info, _members, _rotation):
            nonlocal last
            candidate = self.candidates()[down]
            signature = (candidate["zero_windows"], candidate["banned"])
            if signature != last:
                self.report["fault"]["progress"].append({"info": info, "candidate": candidate})
                last = signature
                print(f"FAULT {down}: zero_windows={signature[0]}, banned={signature[1]}, "
                      f"LIB={info['last_irreversible_block_num']}", flush=True)
                self.checkpoint()
            return candidate if candidate["banned"] else False

        candidate = self.wait_observing("offline entrant permanently banned", banned, 1500)
        after = self.rpc("get_info")
        self.check(candidate["zero_windows"] == 3 and len(set(candidate["evidence_slots"])) == 3,
                   "Exactly three distinct full zero-output personal windows caused the permanent ban")
        self.check(int(candidate["ban_final_block"]) <= after["last_irreversible_block_num"],
                   "Ban evidence is covered by native finality")
        # Independently inspect finalized block timestamps: none of the three
        # accused complete 12-slot windows may contain an actual block.
        evidence_slots = [int(s) for s in candidate["evidence_slots"]]
        fault_blocks = []
        for number in range(before["last_irreversible_block_num"] + 1,
                            after["last_irreversible_block_num"] + 1):
            block = self.rpc("get_block", {"block_num_or_id": number})
            slot = self.slot(block["timestamp"])
            if any(start <= slot < start + 12 for start in evidence_slots):
                raise AssertionError("A claimed zero-output window actually contains an irreversible block")
            fault_blocks.append({k: block[k] for k in ("block_num", "timestamp", "producer", "id")})
        self.report["fault"]["finalized_blocks"] = fault_blocks
        self.check(True, "Independent finalized block timestamps prove all three zero-output windows were empty")
        self.check(after["last_irreversible_block_num"] > before["head_block_num"] + 100,
                   "Twenty surviving producers/finalizers kept finality advancing during outage")
        self.check(self.table("accounts", code="eosio.token", scope=down) == balance,
                   "Blacklisting did not freeze, confiscate or change the account's token balance")
        ledger = self.producer(down)["unpaid_blocks"]
        self.wait_observing("banned seat replaced by one healthy candidate",
                            lambda _i, m, r: len(m) == 21 and down not in m and r["stage"] == 0, 900)
        self.check(self.producer(down)["unpaid_blocks"] == ledger,
                   "Offline account accrued no further payable block count")
        self.check([n for n, c in self.candidates().items() if c["banned"]] == [down],
                   "Healthy accounts were not falsely blacklisted")
        self.start_peer(down, restart=True)
        self.peers[down]["stopped"] = False
        self.wait_for("restarted banned process catches up", lambda: self.synced(self.names), timeout=180)
        # Normal account activity is still allowed. Re-registering must not erase
        # the permanent rotation ban or the historical payable block ledger.
        self.action("unregprod", {"producer": down}, down)
        self.action("regproducer", {"producer": down, "producer_key": self.accounts[down]["Public key"],
                                    "url": "", "location": 0}, down)
        # unregprod retains the existing finalizer key; registering it twice is
        # rightly rejected as a duplicate and is not part of producer re-entry.
        self.check(self.candidates()[down]["banned"], "Unregister/register cannot clear the permanent account ban")
        self.tx(["transfer", down, "alice", "0.0001 SYS", "blacklist is not an asset freeze"])
        self.check(True, "Blacklisted account can still transfer its own assets")
        deadline = time.monotonic() + 270  # More than two unchanged 126-second retry periods.
        while time.monotonic() < deadline:
            self.observe()
            if down in self.last_policy:
                raise AssertionError("Banned account was re-admitted")
            time.sleep(1)
        self.check(self.producer(down)["unpaid_blocks"] == ledger,
                   "Restart and re-registration create no blocks/rewards for the banned account")
        self.report["fault"].update(after=after, candidate_after=self.candidates()[down],
                                    preserved_unpaid_blocks=ledger)

    def tests(self):
        self.tx(["system", "voteproducer", "approve", "alice", "eosio"])
        self.action("deposit", {"owner": "alice", "amount": "100.0000 SYS"}, "alice")
        self.action("buyrex", {"from": "alice", "amount": "100.0000 SYS"}, "alice")
        self.check(self.table("globalv2")[0]["thresh_activated_stake_time"] != "1970-01-01T00:00:00.000",
                   "100 SYS of REX voting activates rewards on the partial-supply chain")
        self.observe()
        first = self.rpc("get_info")["last_irreversible_block_num"] + 1
        expected = {"eosio": 100}
        for index, name in enumerate(self.names[1:], start=1):
            self.start_peer(name)
            self.wait_for(name + " P2P synchronization", lambda n=name: self.synced(["eosio", n]))
            self.register(name)
            weight = 100 + index
            self.stake(name, weight, 0)
            self.tx(["system", "voteproducer", "approve", name, name])
            expected[name] = weight
            self.observe()
        self.check(len({p["pid"] for p in self.process_evidence().values()}) == 22,
                   "22 independently keyed nodeos processes joined the same native Savanna chain")
        self.ranking(expected, "22 registered and voted independent producers")
        self.check(all(not c["banned"] and c["zero_windows"] == 0 for c in self.candidates().values()),
                   "No blacklist strikes accumulate during the under-21 startup phase")
        self.wait_observing("all 22 candidates have actually produced; full 21-seat committee",
                            lambda _i, members, _r: len(members) == 21 and
                            self.observed == set(self.names), 10800)
        self.check(set(len(e["schedule"]["producers"]) for e in self.report["committee_events"]) == set(range(1, 22)),
                   "Every actual committee size from one through 21 was observed without bulk admission")
        self.check(all(not c["banned"] for c in self.candidates().values()),
                   "Healthy 22-process admission and rotation caused no false blacklist")
        # onblock's unpaid ledger can lead LIB. Do not conclude that the last
        # entrant produced final blocks until those accepted blocks are final.
        self.wait_lib(self.rpc("get_info")["head_block_num"] + 12, timeout=150)
        end = self.rpc("get_info")["last_irreversible_block_num"]
        production = self.scan_finalized(first, end, "all 22 candidates' real finalized production")
        self.check(set(production["counts"]) == set(self.names),
                   "All 22 candidates produced irreversible blocks; the 22nd is not a permanent standby")
        self.checkpoint()
        self.fault_test()
        self.report["final_synchronization"] = self.wait_for("all 22 processes agree on an irreversible block",
                                                            lambda: self.synced(self.names), timeout=180)
        self.check_resource_split("After 22-process admission and real fault")
        self.report["processes"] = self.process_evidence()


def main():
    os.umask(0o077)
    def terminate(_signum, _frame):
        raise KeyboardInterrupt("Stopping the private 22-node test")
    signal.signal(signal.SIGTERM, terminate)
    parser = argparse.ArgumentParser(description=__doc__)
    project = Path(__file__).resolve().parents[1]
    parser.add_argument("--build-dir", type=Path, default=project / "build")
    parser.add_argument("--contracts-dir", type=Path,
                        default=project.parent / "reference-contracts/build/pureeos-rotation-v2")
    parser.add_argument("--state-parent", type=Path, default=Path("/dev/shm"))
    args = parser.parse_args()
    return LargeMultiNodeTest(args.build_dir.resolve(), args.contracts_dir.resolve(),
                             args.state_parent.resolve()).execute()


if __name__ == "__main__":
    raise SystemExit(main())
