#!/usr/bin/env python3
"""Open-loop sender + block-ID-pinned observer for an ALREADY bootstrapped test chain.

Accepts pre-signed, unique packed business transactions (one JSON object per line).
Does not create accounts, alter resources, start nodes or change consensus limits.
The input file contains signatures; evidence deliberately contains IDs only.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import threading
import time
import urllib.request
import zlib

from finality_tps import analyze, read_events, require


class Journal:
    def __init__(self, path):
        self.file = Path(path).open("x")
        self.lock = threading.Lock()

    def emit(self, kind, **fields):
        with self.lock:
            record = {"type": kind, "at_ns": time.monotonic_ns(), **fields}
            self.file.write(json.dumps(record, separators=(",", ":")) + "\n")
            self.file.flush()

    def close(self):
        self.file.close()


def rpc(endpoint, method, payload=None, timeout=10):
    request = urllib.request.Request(endpoint.rstrip("/") + "/v1/chain/" + method,
                                     json.dumps(payload if payload is not None else {}).encode(),
                                     {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def load_transactions(path):
    packets, ids = [], set()
    for item in read_events(path):
        packet = {k: item[k] for k in ("signatures", "compression", "packed_context_free_data", "packed_trx")}
        require(packet["compression"] in (0, 1, "none", "zlib"), "unsupported transaction compression")
        raw = bytes.fromhex(packet["packed_trx"])
        if packet["compression"] in (1, "zlib"):
            raw = zlib.decompress(raw)
        txid = hashlib.sha256(raw).hexdigest()
        require(txid not in ids, "duplicate prepared transaction ID; prepare unique transactions")
        require("id" not in item or item["id"] == txid, "prepared transaction ID mismatch")
        actions = item.get("action_count", 1)
        require(isinstance(actions, int) and actions > 0, "invalid declared action_count")
        ids.add(txid)
        packets.append((txid, packet, actions))
    return packets


class Observer:
    def __init__(self, endpoint, chain_id, journal, timeout):
        self.endpoint, self.chain_id, self.journal, self.timeout = endpoint, chain_id, journal, timeout
        self.known = set()

    def info(self):
        info = rpc(self.endpoint, "get_info", timeout=self.timeout)
        require(info["chain_id"] == self.chain_id, "wrong chain ID")
        return info

    def initialize(self):
        info = self.info()
        self.known.add(info["last_irreversible_block_id"])
        self.journal.emit("anchor", id=info["last_irreversible_block_id"],
                          num=info["last_irreversible_block_num"], chain_id=self.chain_id)
        self.sample()

    def sample(self):
        started = time.monotonic_ns()
        info = self.info()
        head = info["head_block_id"]
        pending = []
        while head not in self.known:
            block = rpc(self.endpoint, "get_block", {"block_num_or_id": head}, self.timeout)
            require(block["id"] == head, "get_block returned a different block ID")
            receipts = [{"id": r["trx"] if isinstance(r["trx"], str) else r["trx"]["id"],
                         "status": r["status"]} for r in block["transactions"]]
            pending.append({"id": head, "num": block["block_num"],
                            "previous": block["previous"], "receipts": receipts})
            head = block["previous"]
            require(len(pending) <= 100000, "observer too far behind; abort capacity measurement")
        for block in reversed(pending):
            self.journal.emit("block", **block)
            self.known.add(block["id"])
        require(info["last_irreversible_block_id"] in self.known, "LIB not on downloaded head ancestry")
        self.journal.emit("sample", started_ns=started, head_id=info["head_block_id"],
                          lib_id=info["last_irreversible_block_id"])


def run(args):
    require(all(math.isfinite(v) for v in (args.rate, args.seconds, args.warmup, args.drain, args.poll, args.timeout)),
            "numeric arguments must be finite")
    require(args.rate > 0 and args.seconds > 0 and args.warmup >= 0 and args.drain > 0
            and args.poll > 0 and args.timeout > 0 and args.inflight > 0, "invalid benchmark limits")
    packets = load_transactions(args.transactions)
    planned = math.ceil((args.warmup + args.seconds) * args.rate)
    require(len(packets) >= planned, f"need {planned} unique transactions including warmup")
    journal = Journal(args.evidence)
    stop_observer = threading.Event()
    fatal_collection_error = threading.Event()
    observer = Observer(args.endpoint, args.chain_id, journal, args.timeout)
    executor = None
    thread = None
    inflight = threading.BoundedSemaphore(args.inflight)
    try:
        observer.initialize()  # Check identity/read access BEFORE any transaction is sent.
        origin = time.monotonic_ns()
        start = origin + round(args.warmup * 1e9)
        end = start + round(args.seconds * 1e9)
        journal.emit("window", version=1, clock="monotonic_ns", start_ns=start, end_ns=end,
                     requested_tps=args.rate, chain_id=args.chain_id,
                     action_count_source="input declaration; TPS always counts transaction IDs")

        def observe():
            while not stop_observer.wait(args.poll):
                try:
                    observer.sample()
                except Exception as error:
                    # Do not leak response bodies, signed payloads or credentials into evidence.
                    try:
                        journal.emit("error", stage="observe", category=type(error).__name__)
                    except Exception:
                        fatal_collection_error.set()
                        return

        def send(item, due):
            txid, packet, actions = item
            try:
                journal.emit("sent", id=txid, actions=actions, due_ns=due)
                response = rpc(args.endpoint, "push_transaction", packet, args.timeout)
                processed = response.get("processed", {})
                accepted = response.get("transaction_id") == txid and not processed.get("except")
                journal.emit("ack", id=txid, accepted=accepted)
            except Exception as error:
                journal.emit("ack", id=txid, accepted=False, category=type(error).__name__)
            finally:
                inflight.release()

        def completed(future):
            # A worker failing to write evidence must not silently disappear in
            # an uninspected Future (e.g. disk full followed by a later recovery).
            if future.exception() is not None:
                fatal_collection_error.set()

        thread = threading.Thread(target=observe, name="finality-observer")
        thread.start()
        executor = ThreadPoolExecutor(max_workers=args.inflight, thread_name_prefix="business-sender")
        for index in range(planned):
            due = origin + round(index * 1e9 / args.rate)
            delay = (due - time.monotonic_ns()) / 1e9
            if delay > 0:
                time.sleep(delay)
            # No unbounded executor queue or catch-up burst after sender saturation.
            now = time.monotonic_ns()
            if now >= end or now - due > 1e9 / args.rate or not inflight.acquire(blocking=False):
                journal.emit("error", stage="sender_capacity", category="missed_schedule")
                continue
            executor.submit(send, packets[index], due).add_done_callback(completed)
        delay = (end + round(args.drain * 1e9) - time.monotonic_ns()) / 1e9
        if delay > 0:
            time.sleep(delay)
        executor.shutdown(wait=True)
        executor = None
        stop_observer.set()
        thread.join()
        thread = None
        require(not fatal_collection_error.is_set(), "incomplete evidence: collector worker failed")
        observer.sample()
        journal.emit("end")
    finally:
        stop_observer.set()
        if executor:
            executor.shutdown(wait=True)
        if thread:
            thread.join()
        journal.close()
    return analyze(read_events(args.evidence))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", required=True, help="isolated, already bootstrapped test-chain RPC URL")
    parser.add_argument("--chain-id", required=True, help="expected test-chain identity; checked before sending")
    parser.add_argument("--transactions", type=Path, required=True, help="pre-signed packed transactions JSONL")
    parser.add_argument("--evidence", type=Path, required=True, help="NEW JSONL output; never overwrites")
    parser.add_argument("--rate", type=float, required=True)
    parser.add_argument("--seconds", type=float, default=600)
    parser.add_argument("--warmup", type=float, default=120)
    parser.add_argument("--drain", type=float, default=30)
    parser.add_argument("--poll", type=float, default=0.25)
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--inflight", type=int, default=16)
    args = parser.parse_args()
    print(json.dumps(run(args), indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
