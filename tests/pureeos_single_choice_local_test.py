#!/usr/bin/env python3
"""Deploy integer-v2 contracts on a private loopback chain; retain evidence, stop all child processes."""
import argparse
import base64
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from native_savanna_genesis_test import create_keys, free_port


class LocalTest:
    # This is a one-process contract test, not evidence of independent nodes.
    # Reward activation now elects ordinary candidates even with small votes.
    local_signers = ("eosio", "nodea", "nodeb")
    def __init__(self, build, contracts, resource_split=False):
        self.build, self.contracts = build, contracts
        self.resource_split = resource_split
        self.rotation_context = b"get_rotation_context_v1" in (contracts / "eosio.system/eosio.system.wasm").read_bytes()
        base = build / "local-tests"
        base.mkdir(exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="integer-v2-", dir=base))
        self.cleos = build / "programs/cleos/cleos"
        self.nodeos = build / "programs/nodeos/nodeos"
        self.util = build / "programs/helios-util/helios-util"
        self.http_port, self.p2p_port = free_port(), free_port()
        while self.http_port == self.p2p_port:
            self.p2p_port = free_port()
        self.url = f"http://127.0.0.1:{self.http_port}"
        self.node = self.wallet = None
        self.logs, self.secrets, self.checks = [], [], []
        self.nonce = 0
        self.report = {"status": "running", "directory": str(self.root), "endpoint": self.url,
                       "scope": "integer-v2 local deployment; not automatic rotation acceptance",
                       "checks": self.checks}
        self.report["harness_script_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        self.report["artifacts"] = {name: hashlib.sha256((contracts / name).read_bytes()).hexdigest()
                                    for name in ("eosio.system/eosio.system.wasm", "eosio.system/eosio.system.abi",
                                                 "eosio.bpay/eosio.bpay.wasm", "eosio.token/eosio.token.wasm")}
        if resource_split:
            self.report["scope"] += "; live 10/90 resource split and PowerUp"
            name = "eosio.system/.powerup/powup.results.wasm"
            self.report["artifacts"][name] = hashlib.sha256((contracts / name).read_bytes()).hexdigest()
        print(f"Local test directory: {self.root}", flush=True)

    def redact(self, text):
        for secret in self.secrets:
            text = text.replace(secret, "<redacted test secret>")
        return text

    def run(self, args, *, input_text=None, failure=None):
        result = subprocess.run([str(self.cleos), "--no-auto-keosd", "--url", self.url,
                                 "--wallet-url", f"unix://{self.root}/wallet/keosd.sock", *args],
                                input=input_text, capture_output=True, text=True, timeout=40)
        output = self.redact(result.stdout + result.stderr)
        # A failed setcode can contain a large ABI hex dump. Preserve the error
        # header as well as the terminal exception, without exposing test keys.
        diagnostic = output if len(output) <= 10000 else output[:4000] + "\n... middle omitted ...\n" + output[-6000:]
        if failure is not None:
            if result.returncode == 0 or failure.lower() not in output.lower():
                raise RuntimeError(f"Expected rejection ({failure}), got {result.returncode}: {diagnostic}")
        elif result.returncode != 0:
            raise RuntimeError(f"cleos {args[:3]} failed: {diagnostic}")
        return result.stdout

    def rpc(self, method, data=None, *, namespace="chain"):
        return self.read_rpc(self.url, method, data, namespace=namespace)

    def read_rpc(self, url, method, data=None, *, namespace="chain"):
        request = urllib.request.Request(url + "/v1/" + namespace + "/" + method,
                                         data=json.dumps(data or {}).encode(),
                                         headers={"Content-Type": "application/json"})
        readonly = {"get_info", "get_block", "get_table_rows", "get_account", "get_producers",
                    "get_producer_schedule", "get_finalizer_info", "get_raw_abi", "get_upgrade_state",
                    "get_supported_protocol_features"}
        for attempt in range(1, 4):
            try:
                with urllib.request.urlopen(request, timeout=5) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                body = error.read().decode(errors="replace")
                error.close()
                try:
                    code = json.loads(body).get("error", {}).get("code")
                except (ValueError, AttributeError):
                    code = None
                # A read-only ABI serialization deadline is transient on a
                # shared host. Never retry submissions or logical RPC errors,
                # and do not increase either RPC or consensus CPU limits.
                if method not in readonly or code != 3015010 or attempt == 3:
                    raise RuntimeError(f"RPC {method} HTTP {error.code}: {self.redact(body)}") from error
                self.report.setdefault("rpc_deadline_retries", []).append(
                    {"method": method, "attempt": attempt, "code": code})
                time.sleep(0.1 * attempt)

    def wait_lib(self, target, timeout=50):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            if self.node.poll() is not None:
                raise RuntimeError("nodeos exited unexpectedly; inspect retained node log")
            try:
                last = self.rpc("get_info")
                if last["last_irreversible_block_num"] >= target:
                    return last
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.2)
        raise RuntimeError(f"LIB failed to reach {target}; last info={last}")

    def tx(self, args, *, failure=None):
        self.nonce += 1
        start_lib = self.rpc("get_info")["last_irreversible_block_num"] if failure is None else None
        raw = self.run([*args, "-j", "-x", str(120 + self.nonce)], failure=failure)
        if failure is None:
            result = json.loads(raw)
            if result["processed"]["receipt"]["status"] != "executed":
                raise RuntimeError("Transaction was not executed")
            # processed.block_num describes speculative execution. A producer
            # handoff/fork can include the transaction at another height.
            confirmed = self.wait_transaction(result["transaction_id"], start_lib)
            confirmed["reported_block_num"] = result["processed"]["block_num"]
            self.report.setdefault("confirmed_transactions", []).append(confirmed)
            return result

    def wait_transaction(self, transaction_id, start_lib, timeout=50):
        deadline, next_block = time.monotonic() + timeout, start_lib + 1
        while time.monotonic() < deadline:
            if self.node.poll() is not None:
                raise RuntimeError("nodeos exited while confirming transaction")
            try:
                lib = self.rpc("get_info")["last_irreversible_block_num"]
                while next_block <= lib and time.monotonic() < deadline:
                    block = self.rpc("get_block", {"block_num_or_id": next_block})
                    for receipt in block["transactions"]:
                        trx = receipt["trx"]
                        actual_id = trx if isinstance(trx, str) else trx["id"]
                        if actual_id == transaction_id:
                            if receipt["status"] != "executed":
                                raise RuntimeError("Finalized transaction was not executed")
                            return {"transaction_id": transaction_id, "block_num": next_block,
                                    "block_id": block["id"], "observed_lib": lib}
                    next_block += 1
            except (OSError, urllib.error.URLError):
                pass  # Retry the same not-yet-read block, never skip it.
            time.sleep(0.2)
        raise RuntimeError(f"Transaction {transaction_id} not found in irreversible blocks; next={next_block}")

    def action(self, name, data, actor, *, contract="eosio", failure=None):
        return self.tx(["push", "action", contract, name, json.dumps(data), "-p", actor], failure=failure)

    def deploy_system(self):
        # Initial WASM compilation can exceed the unchanged 150 ms transaction
        # limit on a busy host. Retry only that atomic bootstrap failure, with
        # a new transaction header; never retry logical/auth/governance errors.
        for attempt in range(1, 4):
            try:
                return self.tx(["set", "contract", "eosio", str(self.contracts / "eosio.system")])
            except RuntimeError as error:
                if "tx_cpu_usage_exceeded" not in str(error) or "setcode" not in str(error):
                    raise
                self.report.setdefault("deployment_cpu_retries", []).append(
                    {"attempt": attempt, "error": self.redact(str(error))})
                if attempt == 3:
                    raise
                print("RETRY: bootstrap setcode exceeded unchanged CPU limit; failure retained", flush=True)
                self.wait_lib(self.rpc("get_info")["head_block_num"] + 2)

    def wait_produced(self, owner, timeout=100):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.node.poll() is not None:
                raise RuntimeError("Node exited before ordinary producer began producing")
            if any(row["owner"] == owner and int(row["unpaid_blocks"]) > 0 for row in self.table("producersv2")):
                return
            time.sleep(0.5)
        raise RuntimeError("No produced blocks for elected local test signer " + owner)

    def table(self, name, *, code="eosio", scope="eosio"):
        return self.rpc("get_table_rows", {"json": True, "code": code, "scope": scope,
                                            "table": name, "limit": 1000})["rows"]

    def voter(self, owner):
        return self.rpc("get_account", {"account_name": owner})["voter_info"]

    def ranked(self, **kwargs):
        return self.rpc("get_producers", {"json": True, "limit": 100, **kwargs})

    def ranked_all(self, timeout=30):
        deadline = time.monotonic() + timeout
        rows, pages, owners, cursor = [], [], set(), ""
        while time.monotonic() < deadline:
            page = self.ranked(lower_bound=cursor)
            if pages:
                for field in ("vote_schema", "vote_precision", "total_producer_vote_amount"):
                    if page.get(field) != pages[0].get(field):
                        raise RuntimeError("Producer ranking changed during pagination: " + field)
            pages.append(page)
            for row in page["rows"]:
                if row["owner"] in owners:
                    raise RuntimeError("Duplicate producer while following ranking cursor")
                owners.add(row["owner"])
                rows.append(row)
            more = page.get("more", "")
            if not more:
                return {**pages[0], "rows": rows, "more": "", "pages": pages}
            if more == cursor and page["rows"]:
                raise RuntimeError("Producer ranking cursor did not advance")
            cursor = more
            if not page["rows"]:
                time.sleep(0.05)  # Server deadline can expire before its first row.
        raise RuntimeError("Producer ranking pagination timed out")

    def governance(self):
        state = self.rpc("get_upgrade_state")
        # RPC observation metadata advances with each block; it is not governance state.
        state.pop("head_block_num", None)
        state.pop("head_block_id", None)
        return state

    def check(self, condition, description):
        if not condition:
            raise AssertionError(description)
        self.checks.append(description)
        print("PASS: " + description, flush=True)

    def key(self, bls=False):
        command = [str(self.util), "bls", "create", "key", "--to-console"] if bls else [str(self.cleos), "create", "key", "--to-console"]
        key = create_keys(command)
        self.secrets.append(key["Private key"])
        return key

    def spawn(self, command, log_name):
        log = (self.root / log_name).open("w", encoding="utf-8")
        self.logs.append(log)
        return subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)

    def start_node(self, phase, extra=()):
        # Disposable 128 MiB test chains still keep an 8 GiB disk reserve.
        # The production default (10% free) unnecessarily stops these tests on
        # larger, >90%-used disks. Respect an explicit caller reserve.
        if not any(str(arg).split("=", 1)[0] == "--resource-monitor-space-absolute-gb" for arg in extra):
            extra = (*extra, "--resource-monitor-space-absolute-gb", "8")
        self.node = self.spawn([str(self.nodeos), "--data-dir", str(self.root / "data"),
                                "--config-dir", str(self.root / "config"), *extra], phase + ".log")

    @staticmethod
    def stop(process):
        if process is None:
            return
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process.returncode != 0:
            raise RuntimeError(f"Unclean child shutdown: {process.returncode}")

    def setup(self, register_candidates=True):
        self.accounts = {n: self.key() for n in ("eosio", "developer", "devactive", "devupgrade",
                                                "alice", "bob", "renter", "nodea", "nodeb")}
        self.bls = {n: self.key(True) for n in ("eosio", "nodea", "nodeb")}
        (self.root / "config").mkdir()
        (self.root / "wallet").mkdir()
        genesis = {"initial_timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000"),
                   "initial_key": self.accounts["eosio"]["Public key"],
                   "initial_savanna": {"version": 2, "finalizer_key": self.bls["eosio"]["Public key"],
                                       "proof_of_possession": self.bls["eosio"]["Proof of Possession"]}}
        (self.root / "genesis.json").write_text(json.dumps(genesis, indent=2))
        (self.root / "config/config.ini").write_text("\n".join([
            "plugin = eosio::producer_plugin", "plugin = eosio::chain_api_plugin",
            "plugin = eosio::producer_api_plugin",
            "producer-name = eosio", "enable-stale-production = true",
            "chain-state-db-size-mb = 128", "chain-state-db-guard-size-mb = 4",
            "http-server-address = 127.0.0.1:" + str(self.http_port),
            "p2p-listen-endpoint = 127.0.0.1:" + str(self.p2p_port),
            "max-body-size = 8388608", "verbose-http-errors = true",
            "signature-provider = " + self.accounts["eosio"]["Public key"] + "=KEY:" + self.accounts["eosio"]["Private key"],
            "signature-provider = " + self.bls["eosio"]["Public key"] + "=KEY:" + self.bls["eosio"]["Private key"], ""]))
        with (self.root / "config/config.ini").open("a") as output:
            for name in self.local_signers:
                if name == "eosio":
                    continue
                output.write("producer-name = " + name + "\n")
                for key in (self.accounts[name], self.bls[name]):
                    output.write("signature-provider = " + key["Public key"] + "=KEY:" + key["Private key"] + "\n")
        self.wallet = self.spawn([str(self.build / "programs/keosd/keosd"), "--data-dir", str(self.root / "wallet"),
                                  "--config-dir", str(self.root / "wallet"), "--http-server-address", ""], "wallet.log")
        for _ in range(100):
            if (self.root / "wallet/keosd.sock").exists():
                break
            if self.wallet.poll() is not None:
                raise RuntimeError("keosd exited during startup")
            time.sleep(0.1)
        self.run(["wallet", "create", "-n", "localtest", "--file", str(self.root / "wallet/password.txt")])
        for key in self.accounts.values():
            self.run(["wallet", "import", "-n", "localtest"], input_text=key["Private key"] + "\n")
        self.start_node("genesis", ["--genesis-json", str(self.root / "genesis.json")])
        info = self.wait_lib(17)
        self.report["chain_id"] = info["chain_id"]
        self.check(self.rpc("get_block", {"block_num_or_id": 2})["schedule_version"] & (1 << 31),
                   "Native Savanna genesis, head and LIB advancing")
        supported = self.rpc("get_supported_protocol_features", {"exclude_disabled": True}, namespace="producer")
        names = {s["value"] for feature in supported for s in feature["specification"]
                 if s["name"] == "builtin_feature_codename"}
        enabled = {spec["builtin_feature_codename"] for path in (self.root / "config/protocol_features").glob("*.json")
                   if (spec := json.loads(path.read_text()))["subjective_restrictions"]["enabled"]}
        self.check(names == enabled and len(names) == len(supported),
                   "Protocol-feature API lists every enabled feature exactly once")
        seen = set()
        for feature in supported:
            if not set(feature["dependencies"]).issubset(seen):
                raise AssertionError("Protocol-feature API dependencies are not in topological order")
            seen.add(feature["feature_digest"])
        self.report["protocol_feature_names"] = sorted(names)

        system = ("eosio.token", "eosio.stake", "eosio.rex", "eosio.ram", "eosio.ramfee",
                  "eosio.bpay", "eosio.vpay", "eosio.saving", "eosio.fees", "eosio.msig", "eosio.wrap")
        if self.resource_split:
            system += ("eosio.reserv",)
        ordinary = ("developer", "alice", "bob", "renter", "nodea", "nodeb")
        for account in (*system, *ordinary):
            owner = self.accounts[account if account in ordinary else "eosio"]["Public key"]
            active = self.accounts["devactive"]["Public key"] if account == "developer" else owner
            self.tx(["create", "account", "eosio", account, owner, active])
        self.tx(["set", "account", "permission", "developer", "upgrade", self.accounts["devupgrade"]["Public key"], "active"])
        self.tx(["set", "contract", "eosio.token", str(self.contracts / "eosio.token")])
        self.action("create", {"issuer": "eosio", "maximum_supply": "1000000000.0000 SYS"}, "eosio.token", contract="eosio.token")
        self.action("issue", {"to": "eosio", "quantity": "1000000000.0000 SYS", "memo": "isolated test only"}, "eosio", contract="eosio.token")
        for account in ordinary:
            self.tx(["transfer", "eosio", account, "100000.0000 SYS", "isolated test funding"])
        print("Bootstrap accounts and token funding complete; deploying system contracts", flush=True)
        if self.rotation_context:
            # The production rotation-v2 WASM imports a gated native intrinsic.
            # Activate it through the real bootstrap contract before deploying
            # the system contract; never bypass protocol activation in the test.
            feature = next(f for f in supported if any(
                s.get("name") == "builtin_feature_codename" and s.get("value") == "PUREEOS_ROTATION_CONTEXT"
                for s in f["specification"]))
            self.tx(["set", "contract", "eosio", str(self.contracts / "eosio.bios")])
            self.action("activate", {"feature_digest": feature["feature_digest"]}, "eosio")
            self.report["rotation_context_feature"] = feature["feature_digest"]
        self.tx(["set", "contract", "eosio.bpay", str(self.contracts / "eosio.bpay")])
        self.tx(["set", "contract", "eosio.rex", str(self.contracts / "eosio.system/.rex"), "rex.results.wasm", "rex.results.abi"])
        if self.resource_split:
            self.tx(["set", "contract", "eosio.reserv", str(self.contracts / "eosio.system/.powerup"), "powup.results.wasm", "powup.results.abi"])
        self.deploy_system()
        self.action("init", {"version": 0, "core": "4,SYS"}, "eosio")
        if self.resource_split:
            self.action("buyram", {"payer": "eosio", "receiver": "eosio.reserv", "quant": "100.0000 SYS"}, "eosio")
        for account in ordinary:
            self.action("buyram", {"payer": "eosio", "receiver": account, "quant": "100.0000 SYS"}, "eosio")
            # Resources supplied by eosio remain eosio-owned and give these users no votes.
            self.action("delegatebw", {"from": "eosio", "receiver": account, "stake_net_quantity": "10.0000 SYS",
                                        "stake_cpu_quantity": "100.0000 SYS", "transfer": False}, "eosio")
        if self.resource_split:
            self.action("govinit", {"developer": "developer", "review_seconds": 1209600, "protected_accounts": []},
                        "eosio@owner", failure="configure PowerUp before governance sealing")
            self.check(not self.governance()["sealed"], "Missing PowerUp configuration rolls back governance sealing")
            resource = {"current_weight_ratio": None, "target_weight_ratio": None, "assumed_stake_weight": None,
                        "target_timestamp": None, "exponent": 2.0, "decay_secs": 86400,
                        "min_price": "1.0000 SYS", "max_price": "100.0000 SYS"}
            self.action("cfgpowerup", {"args": {"net": resource, "cpu": resource, "powerup_days": 30,
                                                "min_powerup_fee": "0.0001 SYS"}}, "eosio")
            self.check_resource_split("PowerUp initialized before sealing")
        self.action("govinit", {"developer": "developer", "review_seconds": 1209600, "protected_accounts": []}, "eosio@owner")
        state = self.governance()
        self.check(state["sealed"] and state["review_seconds"] == 1209600, "Real contract deployment and 14-day governance sealing")
        for node in (("nodea", "nodeb") if register_candidates else ()):
            self.action("regproducer", {"producer": node, "producer_key": self.accounts[node]["Public key"], "url": "", "location": 0}, node)
            self.action("regfinkey", {"finalizer_name": node, "finalizer_key": self.bls[node]["Public key"],
                                      "proof_of_possession": self.bls[node]["Proof of Possession"]}, node)

    def tests(self):
        self.check(self.table("votepolicy")[0]["version"] == 2 and self.ranked()["vote_schema"] == "integer-v2",
                   "Integer-v2 ABI, tables and RPC profile agree")
        self.tx(["system", "voteproducer", "approve", "bob", "nodea"])
        self.check(self.voter("bob")["last_vote_amount"] == "0", "Zero-balance CLI selection creates no votes")
        self.tx(["system", "voteproducer", "unapprove", "bob", "nodea"])
        self.check(not self.voter("bob")["producers"], "CLI unapprove reads votersv2 and clears an eligible choice")
        self.action("claimrewards", {"owner": "nodeb"}, "nodeb", failure="standby rewards are disabled")
        self.check(True, "Registered zero-output producer cannot claim rewards")
        for args, message in ((["system", "voteproducer", "prods", "alice", "nodea", "nodeb"], "Select exactly one"),
                              (["system", "voteproducer", "proxy", "alice", "nodea"], "Proxy voting is disabled"),
                              (["system", "regproxy", "alice"], "Proxy registration is disabled"),
                              (["system", "unregproxy", "alice"], "disabled")):
            self.tx(args, failure=message)
        self.action("voteproducer", {"voter": "alice", "proxy": "", "producers": ["nodea", "nodeb"]}, "alice", failure="select at most one")
        self.action("voteproducer", {"voter": "alice", "proxy": "nodea", "producers": []}, "alice", failure="proxy voting is disabled")
        self.action("regproxy", {"proxy": "alice", "isproxy": True}, "alice", failure="proxy voting is disabled")
        self.check(True, "CLI and direct-contract multi-vote/proxy bypasses reject")
        self.action("deposit", {"owner": "alice", "amount": "50000.0000 SYS"}, "alice")
        before = self.table("rexfund")
        self.action("buyrex", {"from": "alice", "amount": "50000.0000 SYS"}, "alice", failure="select one producer before buying REX")
        self.check(self.table("rexfund") == before, "REX buy without selection rejects without losing funds")
        self.tx(["system", "voteproducer", "approve", "alice", "nodea"])
        self.action("buyrex", {"from": "alice", "amount": "50000.0000 SYS"}, "alice")
        weight = self.voter("alice")["last_vote_amount"]
        self.check(weight == "500000000", "50,000 SYS in REX allocates exact integer votes")
        self.tx(["system", "voteproducer", "unapprove", "alice", "nodea"], failure="cannot clear vote while holding REX")
        self.tx(["system", "voteproducer", "approve", "alice", "nodeb"])
        ranked = self.ranked(limit=1)
        self.check(ranked["rows"][0]["owner"] == "nodeb" and ranked["rows"][0]["total_votes"] == weight and
                   ranked["vote_precision"] == 4 and ranked["total_producer_vote_amount"] == weight,
                   "Atomic CLI switch and exact RPC totals/precision")
        self.check(self.ranked(limit=1, lower_bound=ranked["more"])["rows"][0]["owner"] == "nodea", "RPC pagination does not repeat boundary")
        listing = self.run(["system", "listproducers"])
        self.check("50000.0000" in listing and "Votes (exact)" in listing, "CLI formats exact token-denominated votes")
        self.action("voteupdate", {"voter_name": "alice"}, "alice")
        self.check(self.voter("alice")["last_vote_amount"] == weight, "Repeated refresh does not create time-weighted votes")
        self.wait_produced("nodeb")
        self.action("claimrewards", {"owner": "nodeb"}, "nodeb", failure="within past day")
        self.check(True, "Elected producer really produces, but cannot bypass the 24-hour claim cooldown")
        balance = self.table("accounts", code="eosio.token", scope="alice")
        self.tx(["transfer", "alice", "eosio.bpay", "1.0000 SYS", "no rank-only payout"], failure="rank-only standby reward distribution is disabled")
        self.check(balance == self.table("accounts", code="eosio.token", scope="alice"), "Zero-block rewards and rank-only bpay funding reject atomically")
        self.action("deposit", {"owner": "renter", "amount": "2.0000 SYS"}, "renter")
        for resource in ("cpu", "net"):
            tx = self.rent_with_custody_check(resource)
            loans = self.table(resource + "loan")
            self.check(len(loans) == 1 and not any(v["owner"] == "renter" for v in self.table("votersv2")),
                       resource.upper() + " rental works without selecting a producer or creating vote stake")
            expiry = datetime.datetime.fromisoformat(loans[0]["expiration"])
            block_time = datetime.datetime.fromisoformat(tx["processed"]["block_time"])
            self.check((expiry - block_time).total_seconds() == 30 * 86400,
                       resource.upper() + " loan retains original 30-day term")
        self.action("unregprod", {"producer": "nodeb"}, "nodeb")
        self.action("voteupdate", {"voter_name": "alice"}, "alice")
        self.action("buyrex", {"from": "alice", "amount": "1.0000 SYS"}, "alice", failure="active registered producer")
        self.tx(["system", "voteproducer", "approve", "alice", "nodea"])
        self.check(self.voter("alice")["producers"] == ["nodea"], "Inactive target does not lock refresh or atomic exit to another producer")
        self.governance_test()
        if self.resource_split:
            self.resource_tests()

    @staticmethod
    def asset_units(value):
        from decimal import Decimal
        return int(Decimal(value.split()[0]) * 10000)

    def rent_with_custody_check(self, resource):
        # This harness intentionally leaves eosio.fees without a forwarder;
        # WASM integration tests separately cover donation back into REX.
        def balances():
            result = {}
            for account in ("eosio", "eosio.rex", "eosio.fees", "renter"):
                rows = self.table("accounts", code="eosio.token", scope=account)
                result[account] = self.asset_units(rows[0]["balance"]) if rows else 0
            result["renter_fund"] = self.asset_units(next(
                row["balance"] for row in self.table("rexfund") if row["owner"] == "renter"))
            return result

        before = balances()
        votes = [row for row in self.table("votersv2") if row["owner"] == "renter"]
        tx = self.action("rent" + resource, {"from": "renter", "receiver": "renter",
                         "loan_payment": "1.0000 SYS", "loan_fund": "0.0000 SYS"}, "renter")
        after = balances()
        self.check(after["eosio"] == before["eosio"] and after["renter"] == before["renter"],
                   resource.upper() + " rent neither debits the system treasury nor double-charges the renter wallet")
        self.check(after["eosio.rex"] == before["eosio.rex"] - 10000 and
                   after["eosio.fees"] == before["eosio.fees"] + 10000 and
                   after["renter_fund"] == before["renter_fund"] - 10000,
                   resource.upper() + " rent transfers exactly 1 SYS from REX custody to fees and debits the renter fund once")
        self.check([row for row in self.table("votersv2") if row["owner"] == "renter"] == votes,
                   resource.upper() + " rent does not change the renter's existing vote state")
        self.report.setdefault("rental_fee_checks", []).append(
            {"resource": resource, "before": before, "after": after, "transaction_id": tx["transaction_id"]})
        return tx

    def check_resource_split(self, label):
        policy = self.table("respolicy")[0]
        market = self.table("powup.state", scope="0")[0]
        reserve = self.table("userres", scope="eosio.reserv")[0]
        self.wait_lib(self.rpc("get_info")["head_block_num"] + 2)
        info = self.rpc("get_info")
        for kind in ("cpu", "net"):
            longterm = int(policy[kind + "_weight"])
            weight = int(market[kind]["weight"])
            used = int(market[kind]["utilization"])
            self.check(weight == max(9 * max(longterm, 1), used) and
                       self.asset_units(reserve[kind + "_weight"]) == weight - used and
                       int(info["total_" + kind + "_weight"]) == longterm + weight,
                       label + ": " + kind.upper() + " native weights equal long-term + PowerUp, long-term <= 10%")

    def resource_tests(self):
        self.check_resource_split("After CPU and NET REX rental")
        policy = self.table("respolicy")
        votes = self.voter("alice")["last_vote_amount"]
        fee_before = self.asset_units(self.table("accounts", code="eosio.token", scope="eosio.fees")[0]["balance"])
        purchase = {"payer": "alice", "receiver": "bob", "days": 30,
                    "net_frac": 100000000000000, "cpu_frac": 100000000000000, "max_payment": "10.0000 SYS"}
        tx = self.action("powerup", purchase, "alice")
        order = self.table("powup.order", scope="0")[0]
        self.check((datetime.datetime.fromisoformat(order["expires"]) -
                    datetime.datetime.fromisoformat(tx["processed"]["block_time"]).replace(microsecond=0)).total_seconds() == 30 * 86400,
                   "PowerUp uses configured 30-day term; REX remains 30 days independently")
        self.check(self.table("respolicy") == policy and self.voter("alice")["last_vote_amount"] == votes,
                   "PowerUp purchase changes neither long-term counters nor voter stake")
        self.check(self.asset_units(self.table("accounts", code="eosio.token", scope="eosio.fees")[0]["balance"]) > fee_before,
                   "PowerUp fee reaches eosio.fees without changing the funding route")
        self.check_resource_split("After PowerUp purchase")
        before = self.table("powup.state", scope="0")
        self.action("powerup", {**purchase, "max_payment": "0.0000 SYS"}, "alice", failure="max_payment is less")
        self.check(self.table("powup.state", scope="0") == before and len(self.table("powup.order", scope="0")) == 1,
                   "Rejected PowerUp payment rolls back market and orders")
        self.action("delegatebw", {"from": "alice", "receiver": "alice", "stake_net_quantity": "2.0000 SYS",
                                    "stake_cpu_quantity": "3.0000 SYS", "transfer": False}, "alice")
        self.check_resource_split("After additional direct staking")
        self.action("unstaketorex", {"owner": "alice", "receiver": "alice", "from_net": "2.0000 SYS",
                                      "from_cpu": "3.0000 SYS"}, "alice")
        self.check(self.table("respolicy") == policy, "Stake-to-REX conversion removes only direct resource weights")
        self.check_resource_split("After stake-to-REX conversion")
        self.action("powerupexec", {"user": "bob", "max": 10}, "bob")
        self.check(self.table("powup.order", scope="0")[0] == order, "Unexpired PowerUp order survives queue processing")

    def recovery_state(self):
        state = {"voters": self.table("votersv2"), "producers": self.table("producersv2"),
                 "governance": self.governance(), "rex": self.table("rexbal")}
        # Newly activated producers continue earning block counts while RPCs,
        # clean restart and replay run. Compare stable registration/vote state.
        for producer in state["producers"]:
            producer.pop("unpaid_blocks", None)
        if self.resource_split:
            state.update({"resource_policy": self.table("respolicy"), "powerup": self.table("powup.state", scope="0"),
                          "orders": self.table("powup.order", scope="0"),
                          "reserve": self.table("userres", scope="eosio.reserv"),
                          "cpu_loans": self.table("cpuloan"), "net_loans": self.table("netloan")})
        return state

    def governance_test(self):
        code = (self.contracts / "eosio.token/eosio.token.wasm").read_bytes() + bytes((0, 2, 1, ord("t")))
        base = self.rpc("get_raw_abi", {"account_name": "eosio.token"})
        abi = base64.b64decode(base["abi"])
        proposal = {"target": "eosio.token", "old_code_hash": base["code_hash"], "old_abi_hash": base["abi_hash"],
                    "new_code_hash": hashlib.sha256(code).hexdigest(), "new_abi_hash": hashlib.sha256(abi).hexdigest(),
                    "review_document_hash": hashlib.sha256(b"LOCAL TEST ONLY: unchanged code plus custom section").hexdigest()}
        def actions(actor):
            return {"actions": [{"account": "eosio", "name": name, "data": data,
                                 "authorization": [{"actor": actor, "permission": "upgrade" if actor == "developer" else "active"}]}
                                for name, data in (("refreshgov", {}), ("devpropose", proposal))]}
        before = self.governance()
        self.tx(["push", "transaction", json.dumps(actions("alice"))], failure="missing authority")
        self.check(self.governance() == before,
                   "Rejected push transaction returns nonzero and rolls back governance refresh/proposal")
        self.action("setcode", {"account": "eosio.token", "vmtype": 0, "vmversion": 0, "code": code.hex()},
                    "developer@active", failure="reviewed native devexec capability")
        self.check(self.rpc("get_raw_abi", {"account_name": "eosio.token"})["code_hash"] == base["code_hash"],
                   "Developer cannot bypass review using direct setcode")
        self.tx(["push", "transaction", json.dumps(actions("developer"))])
        state = self.governance()
        self.check(state["proposal_voters"] == ["nodea"] and state["pending"], "Only developer can publish; frozen electorate reads integer ranking")
        self.check((datetime.datetime.fromisoformat(state["execute_after"]) - datetime.datetime.fromisoformat(state["proposed_at"])).total_seconds() == 1209600,
                   "Published upgrade deadline is exactly 14 days after proposal")
        self.action("devexec", {"proposal_id": state["proposal_id"], "code": code.hex(), "abi": abi.hex()}, "developer@upgrade", failure="review period has not ended")
        self.action("devveto", {"voter": "nodea", "proposal_id": state["proposal_id"]}, "nodea")
        self.check(self.governance()["vetoes"] == ["nodea"], "Early execution rejects and eligible node can veto")
        self.action("devcancel", {"proposal_id": state["proposal_id"]}, "developer@upgrade")
        self.check(not self.governance()["pending"], "Developer cancels local test proposal without changing system code")

    def recovery(self):
        before = self.rpc("get_info")
        expected = self.recovery_state()
        (self.root / "expected-state.json").write_text(json.dumps(expected, indent=2))
        for phase, extra in (("restart", []), ("replay", ["--replay-blockchain"])):
            self.stop(self.node)
            self.start_node(phase, extra)
            info = self.wait_lib(before["last_irreversible_block_num"] + 16)
            self.check(info["chain_id"] == self.report["chain_id"], phase + ": chain ID unchanged and LIB advances")
            self.check(self.rpc("get_block", {"block_num_or_id": before["head_block_num"]})["id"] == before["head_block_id"],
                       phase + ": persisted reversible chain not replaced")
            current = self.recovery_state()
            self.check(current == expected, phase + ": votes, REX, governance and configured resource state preserved")
            if self.resource_split:
                self.check_resource_split(phase)
            before = info
        self.report["final_info"] = self.rpc("get_info")

    def execute(self):
        try:
            self.setup()
            self.tests()
            self.recovery()
            self.report["status"] = "passed"
        except Exception as error:
            self.report["status"] = "failed"
            self.report["error"] = self.redact(str(error))
            print("FAIL: " + self.report["error"], flush=True)
        finally:
            if self.report["status"] == "running":
                self.report["status"] = "interrupted"
            for process in (self.node, self.wallet):
                try:
                    self.stop(process)
                except Exception as error:
                    self.report["status"] = "failed"
                    self.report.setdefault("shutdown_errors", []).append(str(error))
            for log in self.logs:
                log.close()
            self.report["processes_stopped"] = all(p is None or p.poll() is not None for p in (self.node, self.wallet))
            (self.root / "report.json").write_text(json.dumps(self.report, indent=2))
            print(f"Report: {self.root / 'report.json'}", flush=True)
        return 0 if self.report["status"] == "passed" else 1


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    project = Path(__file__).resolve().parents[1]
    parser.add_argument("--build-dir", type=Path, default=project / "build")
    parser.add_argument("--contracts-dir", type=Path, default=project.parent / "reference-contracts/build/pureeos-single-choice")
    parser.add_argument("--resource-split", action="store_true", help="Require resource-split WASM and enable/test PowerUp before sealing")
    args = parser.parse_args()
    return LocalTest(args.build_dir.resolve(), args.contracts_dir.resolve(), args.resource_split).execute()


if __name__ == "__main__":
    raise SystemExit(main())
