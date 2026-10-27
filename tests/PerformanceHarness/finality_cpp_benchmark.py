#!/usr/bin/env python3
"""Bounded C++ P2P sender + canonical finality observer, one Linux clock domain.

Only for an isolated, already bootstrapped loopback test chain. P2P writes are
not node acknowledgements. Keeps raw journals, refuses incomplete send evidence.
"""
import argparse
import json
import math
from pathlib import Path
import subprocess
import shutil
import time

from benchmark_environment import sample, check_host_health
from finality_benchmark import Journal, Observer
from finality_tps import analyze, quantiles, read_events, require


def validate_sends(events, boot_id, expected):
    events = list(events)
    require(len(events) >= 2 and events[0]['type'] == 'generator_start'
            and events[-1]['type'] == 'generator_end', 'Incomplete generator journal')
    require(events[0]['clock'] == 'CLOCK_MONOTONIC' and events[0]['boot_id'] == boot_id,
            'Generator and observer are not in the same Linux boot clock domain')
    previous, sends, ids, errors = -1, [], set(), 0
    for event in events:
        require(isinstance(event['at_ns'], int) and event['at_ns'] >= previous,
                'Generator clock moved backwards or journal reordered')
        previous = event['at_ns']
    for event in events[1:-1]:
        if event['type'] == 'error':
            errors += 1
            continue
        require(event['type'] == 'sent', 'Unexpected generator journal event')
        txid = event['id']
        require(isinstance(txid, str) and len(txid) == 64 and all(c in '0123456789abcdef' for c in txid), 'Invalid transaction ID')
        require(txid not in ids and isinstance(event['actions'], int) and event['actions'] > 0,
                'Duplicate ID or invalid business action count')
        require(events[0]['at_ns'] <= event['queued_ns'] <= event['at_ns'], 'Invalid send queue timing')
        ids.add(txid)
        sends.append(event)
    end = events[-1]
    require(not errors and end['errors'] == 0 and end['attempted'] == end['completed'] == len(sends) == expected,
            'Generator failed, saturated, terminated early or lost send evidence')
    return sends


def run(args):
    require(all(math.isfinite(v) for v in (args.poll, args.timeout)), 'Nonfinite timings')
    require(args.rate > 0 and args.seconds > 0 and args.warmup >= 0 and args.drain > 0
            and args.poll > 0 and args.timeout > 0 and args.inflight > 0, 'Invalid benchmark limits')
    require(args.rate <= 1000000, 'Generator pacing supports at most one million scheduled sends/s')
    require(args.endpoint.startswith('http://127.0.0.1:') and 0 < args.port < 65536, 'Loopback test chain only')
    require(len(args.accounts.split(',')) >= 2, 'At least two ordinary funded accounts are required')
    require(args.warmup + args.seconds + args.drain < 3500, 'TAPOS/expiration safety window exceeded')
    require(args.keys.stat().st_mode & 0o077 == 0, 'Signing key file must be private (0600)')
    root = args.output
    root.mkdir(mode=0o700)  # Exclusively create; never overwrite previous evidence.
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    journal = Journal(root / 'observer.jsonl')
    observer = Observer(args.endpoint, args.chain_id, journal, args.timeout)
    process = None
    try:
        observer.initialize()  # Before signing/sending, verify chain identity.
        info = observer.info()
        origin = time.monotonic_ns()
        start = origin + args.warmup * 10**9
        end = start + args.seconds * 10**9
        journal.emit('window', version=1, clock='monotonic_ns', start_ns=start, end_ns=end,
                     requested_tps=args.rate, chain_id=args.chain_id, sender='cpp-p2p', boot_id=boot)
        command = [str(args.generator), '--chain-id', args.chain_id,
                   '--contract-owner-account', 'eosio.token', '--accounts', args.accounts,
                   '--transfer-quantity', '1.0000 SYS',
                   '--priv-keys-file', str(args.keys), '--last-irreversible-block-id', info['last_irreversible_block_id'],
                   '--trx-gen-duration', str(args.warmup + args.seconds), '--target-tps', str(args.rate),
                   '--peer-endpoint', '127.0.0.1', '--port', str(args.port), '--peer-endpoint-type', 'p2p',
                   '--evidence-log', str(root / 'sends.jsonl'), '--max-pending-sends', str(args.inflight),
                   '--log-dir', str(root), '--trx-expiration', '3599']
        with (root / 'generator.log').open('x') as log, (root / 'host.jsonl').open('x') as host:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            next_host = 0
            while time.monotonic_ns() < end + args.drain * 10**9:
                require(process.poll() in (None, 0), 'Generator exited unsuccessfully; inspect retained log')
                try:
                    observer.sample()
                except Exception as error:
                    journal.emit('error', stage='observe', category=type(error).__name__)
                if time.monotonic_ns() >= next_host:
                    status = sample([process.pid, *getattr(args, 'pids', [])])
                    host.write(json.dumps(status) + '\n')
                    host.flush()
                    check_host_health(status)
                    require(all(not status['processes'][str(pid)].get('unavailable') for pid in getattr(args, 'pids', [])),
                            'Observed node process exited during measurement')
                    require(shutil.disk_usage(root).free >= getattr(args, 'storage_reserve_bytes', 0),
                            'Benchmark disk reserve exhausted; measurement invalid')
                    next_host = time.monotonic_ns() + 10**9
                time.sleep(args.poll)
            require(process.poll() is not None, 'Generator did not finish within fixed window plus drain')
            require(process.returncode == 0, 'Generator exited unsuccessfully; inspect retained log')
        sends = validate_sends(read_events(root / 'sends.jsonl'), boot, args.rate * (args.warmup + args.seconds))
        observer.sample()
        journal.emit('end')
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        journal.close()
    events = [*read_events(root / 'observer.jsonl'), *sends]
    with (root / 'evidence.jsonl').open('x') as output:
        for event in events:
            output.write(json.dumps(event) + '\n')
    result = analyze(events)
    result['rpc_accepted_unique_tps_diagnostic'] = None
    result['sender'] = 'C++ P2P; socket-write attempt is not node acceptance'
    result['p2p_send_queue_latency'] = quantiles(e['at_ns'] - e['queued_ns'] for e in sends)
    result['requested_tps'] = args.rate
    result['scope'] = 'Fixed-window finality observation; not proof of maximum capacity or SSD performance'
    with (root / 'report.json').open('x') as output:
        json.dump(result, output, indent=2)
    return result


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--chain-id', required=True)
    parser.add_argument('--port', type=int, required=True, help='Loopback P2P port')
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--keys', type=Path, required=True, help='0600 JSON array of signing keys, never CLI secrets')
    parser.add_argument('--accounts', default='alice,bob')
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--rate', type=int, required=True)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--warmup', type=int, default=120)
    parser.add_argument('--drain', type=int, default=30)
    parser.add_argument('--poll', type=float, default=.25)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--inflight', type=int, default=256)
    return parser.parse_args()


if __name__ == '__main__':
    print(json.dumps(run(arguments()), indent=2))
