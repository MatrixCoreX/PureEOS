#!/usr/bin/env python3
"""Bootstrap an isolated one-process PureEOS chain and measure real finality.

No existing chain is modified. Normal PowerUp purchases fund ordinary users.
Default real-disk preflight preserves 8 GiB; no RAM-disk capacity claims.
"""
import argparse
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pureeos_single_choice_local_test import LocalTest
from benchmark_environment import manifest, preflight, sample, check_host_health
from finality_cpp_benchmark import run
from stage_report import compare


class BenchmarkChain(LocalTest):
    local_signers = ('eosio', 'nodea')  # Bootstrap then ONE elected producer, not two physical nodes.

    def action(self, name, data, actor, *, contract='eosio', failure=None):
        if name == 'create' and contract == 'eosio.token':
            data = {**data, 'maximum_supply': '2000000000.0000 SYS'}
        if name == 'govinit' and failure is None:
            self.tx(['set', 'contract', 'eosio.fees', str(self.contracts / 'eosio.fees')])
            super().action('regproducer', {'producer': 'nodea', 'producer_key': self.accounts['nodea']['Public key'],
                                          'url': '', 'location': 0}, 'nodea')
            super().action('regfinkey', {'finalizer_name': 'nodea', 'finalizer_key': self.bls['nodea']['Public key'],
                                        'proof_of_possession': self.bls['nodea']['Proof of Possession']}, 'nodea')
            super().action('delegatebw', {'from': 'alice', 'receiver': 'alice', 'stake_net_quantity': '10.0000 SYS',
                                          'stake_cpu_quantity': '100.0000 SYS', 'transfer': False}, 'alice')
            super().action('voteproducer', {'voter': 'alice', 'proxy': '', 'producers': ['nodea']}, 'alice')
            super().action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
            super().action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        return super().action(name, data, actor, contract=contract, failure=failure)

    def start_node(self, phase, extra=()):
        if self.timing:
            extra = (*extra, '--performance-metrics')
        super().start_node(phase, (*extra, '--plugin', 'eosio::prometheus_plugin',
                                  '--validation-mode', 'full', '--wasm-runtime', 'eos-vm-jit'))

    def metrics(self, label):
        request = urllib.request.Request(self.url + '/v1/prometheus/metrics', b'{}', {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=10) as response:
            (self.root / (label + '-metrics.txt')).write_bytes(response.read())


def execute(args):
    # Preflight before wallet creation or any chain mutation. Evidence/chain must
    # live on the SAME inspected filesystem; relocating a unique empty temp dir
    # on this disk does not touch existing chains.
    args.output_parent = args.output_parent.resolve(strict=True)
    storage = preflight(args.output_parent)
    check_host_health(sample(), temperature_limit=90000)
    cache = (args.build / 'CMakeCache.txt').read_text().splitlines()
    if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache:
        raise ValueError('Capacity harness requires a Release build; debug/default builds are only functional evidence')
    chain = BenchmarkChain(args.build.resolve(), args.contracts.resolve(), True)
    destination = args.output_parent / chain.root.name.replace('integer-v2-', 'performance-')
    if destination.exists():
        raise ValueError('Refusing to overwrite an existing benchmark directory')
    # LocalTest creates its directory under build/local-tests; cross-device
    # rename must not silently move a live chain. This directory is still empty.
    if chain.root.stat().st_dev == args.output_parent.stat().st_dev:
        chain.root.rename(destination)
    else:
        destination.mkdir(mode=0o700)
        chain.root.rmdir()  # Only this newly created, still-empty directory.
    chain.root = destination
    chain.report['directory'] = str(destination)
    chain.report['scope'] = 'One physical node process; native Savanna; normal resources; finalized business TPS'
    chain.timing = args.timing
    chain.report['storage'] = storage
    chain.report['benchmark_configuration'] = {'rate': args.rate, 'warmup': args.warmup, 'seconds': args.seconds,
        'drain': args.drain, 'repeat': args.repeat, 'performance_metrics': args.timing, 'physical_nodes': 1}
    try:
        chain.setup(register_candidates=False)
        chain.wait_produced('nodea')
        for account in ('alice', 'bob'):
            chain.action('powerup', {'payer': account, 'receiver': account, 'days': 30,
                'net_frac': 200000000000000, 'cpu_frac': 200000000000000, 'max_payment': '100.0000 SYS'}, account)
        chain.check_resource_split('Before benchmark')
        profile = {'nodeos_validation': 'FULL (default)', 'wasm_runtime': 'eos-vm-jit (default)',
            'db_size_mib': 128, 'db_guard_mib': 4, 'metrics': args.timing, 'physical_nodes': 1,
            'resource_acquisition': 'normal staking + each user buys 20% CPU/NET PowerUp for 30 days',
            'health_limits': {'startup_max_celsius': 90, 'runtime_max_celsius': 95, 'min_available_memory_gib': 1},
            'governance': chain.governance(), 'global': chain.table('globalv2'),
            'resource_policy': chain.table('respolicy'), 'powerup': chain.table('powup.state', scope='0'),
            'user_accounts': {a: chain.rpc('get_account', {'account_name': a}) for a in ('alice', 'bob')},
            'chain_info': chain.rpc('get_info'), 'storage': storage}
        artifacts = {'nodeos': chain.nodeos, 'trx_generator': args.generator,
                     'system_wasm': chain.contracts / 'eosio.system/eosio.system.wasm',
                     'system_abi': chain.contracts / 'eosio.system/eosio.system.abi',
                     'token_wasm': chain.contracts / 'eosio.token/eosio.token.wasm'}
        document = manifest(Path(__file__).resolve().parents[2], args.build, artifacts, profile)
        (chain.root / 'manifest.json').write_text(json.dumps(document, indent=2))
        keys = chain.root / 'sender-keys.json'
        with keys.open('x') as output:
            json.dump([chain.accounts[a]['Private key'] for a in ('alice', 'bob')], output)
        keys.chmod(0o600)
        chain.report['runs'] = []
        for repeat in range(args.repeat):
            preflight(chain.root)  # Recheck growth; never relax storage protection.
            chain.metrics(f'run-{repeat + 1}-before')
            result = run(SimpleNamespace(endpoint=chain.url, chain_id=chain.report['chain_id'], port=chain.p2p_port,
                generator=args.generator.resolve(), keys=keys, accounts='alice,bob', output=chain.root / f'run-{repeat + 1}',
                rate=args.rate, warmup=args.warmup, seconds=args.seconds, drain=args.drain,
                poll=.25, timeout=10, inflight=256, pids=[chain.node.pid], storage_reserve_bytes=8 * 2**30))
            chain.metrics(f'run-{repeat + 1}-after')
            if args.timing:
                stages = compare((chain.root / f'run-{repeat + 1}-before-metrics.txt').read_text(),
                                 (chain.root / f'run-{repeat + 1}-after-metrics.txt').read_text())
                (chain.root / f'run-{repeat + 1}' / 'stages.json').write_text(json.dumps(stages, indent=2))
            chain.report['runs'].append(result)
            if not result['measurement_complete_without_sampling_or_sender_errors'] or result['cohort_unresolved_at_end']:
                raise RuntimeError('Incomplete sampling or unresolved business transactions; not a passing capacity run')
        chain.check_resource_split('After benchmark')
        chain.report['status'] = 'passed'
    except Exception as error:
        chain.report['status'] = 'failed'
        chain.report['error'] = chain.redact(str(error))
    finally:
        if chain.report['status'] == 'running':
            chain.report['status'] = 'interrupted'
        for process in (chain.node, chain.wallet):
            try:
                chain.stop(process)
            except Exception as error:
                chain.report['status'] = 'failed'
                chain.report.setdefault('shutdown_errors', []).append(str(error))
        for log in chain.logs:
            log.close()
        chain.report['processes_stopped'] = all(p is None or p.poll() is not None for p in (chain.node, chain.wallet))
        (chain.root / 'report.json').write_text(json.dumps(chain.report, indent=2))
        print('Benchmark report: ' + str(chain.root / 'report.json'), flush=True)
    return 0 if chain.report['status'] == 'passed' else 1


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--contracts', type=Path, required=True)
    parser.add_argument('--generator', type=Path, required=True)
    parser.add_argument('--output-parent', type=Path, required=True, help='Existing directory on real local disk')
    parser.add_argument('--rate', type=int, default=10)
    parser.add_argument('--warmup', type=int, default=120)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--drain', type=int, default=30)
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--timing', action='store_true', help='Explicitly enable opt-in nodeos stage timers')
    args = parser.parse_args()
    if not (0 < args.rate <= 1000000 and args.warmup >= 0 and args.seconds > 0 and args.repeat > 0
            and args.drain > 0 and args.warmup + args.seconds + args.drain < 3500):
        parser.error('Invalid duration/rate/repeat, or TAPOS/expiration safety window exceeded')
    return execute(args)


if __name__ == '__main__':
    sys.exit(main())
