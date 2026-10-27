#!/usr/bin/env python3
"""Real system/application/mixed-load finality tests on private loopback chains.

Same candidate/JIT/resource policy in every mode; all samples retained. A given
offered rate is not maximum capacity. Unsupported system operations may safely
fall back; their throughput is not mislabeled successful private execution.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import traceback
import time
from types import SimpleNamespace

from parallel_execution_test import Node, checked_manifest, require, sha256
from parallel_mixed_stress_workload import (balanced_profile, independent_light_profile,
                                         independent_transfer_profile, check_ledger)
from parallel_saturation_metrics import logical_cpu_ticks, utilization
from parallel_pressure_metrics import corpus_completion, block_time_metrics
from parallel_release_benchmark import (admission_diagnostics, check_live_resource_policy,
    final_ids, head_business_ids, intake_diagnostics, measure_presigned,
    parallel_diagnostics, pin_paused_head, producer_health_errors, receipt_capacity,
    summarize, wait_for, finish_runtime_cpu_evidence, end_runtime_observation)
sys.path.insert(0, str(Path(__file__).resolve().parent / 'PerformanceHarness'))
from benchmark_environment import preflight, runtime_cpu_evidence, sample, wait_for_quiet_gate
from finality_tps import analyze, read_events
from parallel_replay_benchmark import proc_usage


def per_kind_finality(events, entries):
    result = {}
    for kind in sorted({e['kind'] for e in entries}):
        ids = {e['id'] for e in entries if e['kind'] == kind}
        result[kind] = analyze(e for e in events if e['type'] not in ('sent', 'ack') or e['id'] in ids)
    return result


def run_case(args, case, corpus, manifest, entries, nodeos, sender, gate, storage, workers, round_number):
    case.mkdir()
    row = {'round': round_number, 'workers': workers, 'validator_workers': args.validator_workers,
           'valid': False, 'quiet_gate': wait_for_quiet_gate(gate, case / 'quiet-gate')}
    validator = Node(nodeos, case / 'validator', corpus, manifest, args.validator_workers,
                     peer='listen', engine='jit', batch_size=args.batch_size)
    producer = Node(nodeos, case / 'producer', corpus, manifest, workers,
                    peer=validator.endpoint, engine='jit', batch_size=args.batch_size)
    stop = threading.Event()
    monitor_errors = []
    thread = None
    try:
        lines = ['producer-name = eosio', 'enable-stale-production = true', 'pause-on-startup = true',
                 'read-mode = head', 'max-transaction-time = 100', 'performance-metrics = false']
        if workers:
            lines.append('experimental-parallel-production = true')
        lines.extend('signature-provider = ' + key for key in json.loads((corpus / 'TEST-ONLY-keys.json').read_text()))
        with (producer.root / 'config/config.ini').open('a') as output:
            output.write('\n'.join(lines) + '\n')
        for node in (validator, producer):
            node.start('live', ['--genesis-json', str(corpus / 'genesis.json'),
                                '--chain-state-db-size-mb', str(args.state_db_mb),
                                '--chain-threads', str(args.chain_threads)])
        row['node_process_ids'] = {'producer': producer.process.pid, 'validator': validator.process.pid}
        for point in manifest['checkpoints']:
            validator.feed(point)
            producer.check(point)
        configuration = json.loads((corpus / 'genesis.json').read_text())['initial_configuration']
        row['live_resource_policy_before'] = {node.root.name: check_live_resource_policy(
            node.rpc('get_consensus_parameters'), configuration) for node in (producer, validator)}

        def monitor():
            with (case / 'host.jsonl').open('x') as output:
                while not stop.is_set():
                    try:
                        state = sample([producer.process.pid, validator.process.pid], include_scheduler=True)
                        state['logical_cpu_ticks'] = logical_cpu_ticks()
                        output.write(json.dumps(state) + '\n'); output.flush()
                        if state['memory_kib'].get('MemAvailable', 0) < 1024 * 1024:
                            monitor_errors.append('low-memory')
                        if shutil.disk_usage(case).free < storage['reserve_bytes']:
                            monitor_errors.append('disk-reserve-exhausted')
                        if any(p.get('unavailable') for p in state['processes'].values()):
                            monitor_errors.append('observed-node-exited')
                        for proc in Path('/proc').iterdir():
                            if not proc.name.isdigit():
                                continue
                            try:
                                name = (proc / 'comm').read_text().strip()
                                if re.fullmatch(r'cc1(?:plus)?|clang(?:\+\+)?(?:-\d+)?|rustc|cargo|cmake|g?make|ninja|lto1', name):
                                    monitor_errors.append('concurrent-compiler:' + name)
                            except OSError:
                                pass
                    except Exception as error:
                        monitor_errors.append(type(error).__name__)
                    stop.wait(1)

        thread = threading.Thread(target=monitor)
        thread.start()
        row['clock_correlation_before'] = {'monotonic_ns': time.monotonic_ns(), 'unix_time_ns': time.time_ns()}
        producer.rpc('resume', namespace='producer')
        measurement = SimpleNamespace(endpoint=f'http://127.0.0.1:{producer.port}',
            observer_endpoint=f'http://127.0.0.1:{validator.port}', chain_id=manifest['chain_id'],
            evidence=case / 'finality.jsonl', rate=args.rate, seconds=args.seconds,
            warmup=args.warmup, drain=args.drain, poll=.1, timeout=10, inflight=32)
        measurement.on_observation_end = lambda: end_runtime_observation(stop, thread, case,
            [producer.process.pid, validator.process.pid], storage, monitor_errors, True, logical_cpu_ticks)
        row['finality'] = measure_presigned(measurement, sender, corpus, producer.p2p_port)
        row['clock_correlation_after'] = {'monotonic_ns': time.monotonic_ns(), 'unix_time_ns': time.time_ns()}
        producer.rpc('pause', namespace='producer')
        stop.set(); thread.join(); thread = None
        finish_runtime_cpu_evidence(row, case, [producer.process.pid, validator.process.pid], storage,
                                    monitor_errors, scheduler=True, logical_ticks=logical_cpu_ticks)
        events = list(read_events(case / 'finality.jsonl'))
        row['corpus_completion'] = corpus_completion(events)
        row['cpu_utilization'] = utilization(
            [json.loads(line) for line in (case / 'host.jsonl').read_text().splitlines()],
            row['corpus_completion']['first_send_ns'],
            row['corpus_completion']['last_finalization_ns'])
        sent = {e['id'] for e in events if e['type'] == 'sent'}
        finalized = final_ids(events)
        row['per_kind_finality'] = per_kind_finality(events, entries)
        require(sum(part['finalized_success_unique'] for part in row['per_kind_finality'].values()) ==
                row['finality']['finalized_success_unique'], 'Per-kind finality partition mismatch')
        info, state = pin_paused_head(producer)
        wait_for(lambda: validator.rpc('get_info')['head_block_id'] == info['head_block_id'], 'validator catches pinned head')
        require(state == validator.rpc('get_integrity_hash', namespace='producer'), 'Replicated state diverged')
        row['state'] = state
        row['checkpoint'] = {'block_num': info['head_block_num'], 'block_id': info['head_block_id'],
                             'lib': info['last_irreversible_block_num'], 'integrity_hash': state['integrity_hash']}
        capacity = []
        executed = head_business_ids(validator, info['head_block_id'], manifest['checkpoints'][-1]['block_id'], capacity)
        require(executed <= sent and sent <= finalized, 'Unexpected, rejected or unfinalized business transaction')
        row['ledger'] = check_ledger(validator, manifest, entries, executed)
        row['receipt_capacity'] = receipt_capacity(capacity, configuration)
        row['block_time_metrics'] = block_time_metrics(validator, capacity, events, row['clock_correlation_before'])
        with (case / 'block-receipts.json').open('x') as output:
            json.dump(capacity, output)
        row['process_usage'] = {node.root.name: proc_usage(node.process.pid) for node in (producer, validator)}
        row['live_resource_policy_after'] = {node.root.name: check_live_resource_policy(
            node.rpc('get_consensus_parameters'), configuration) for node in (producer, validator)}
        producer.stop(); validator.stop()
        log = producer.log.read_text()
        row['producer_health_errors'] = producer_health_errors(log)
        row['production_diagnostics_including_aborted_blocks'] = parallel_diagnostics(log)
        row['production_admission_including_aborted_blocks'] = admission_diagnostics(log)
        row['live_intake_dispatch_counts'] = intake_diagnostics(log)
        match = re.search(r'Experimental parallel production totals: batches=(\d+), attempts=(\d+), '
                          r'succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)', log)
        row['production_attempts_including_aborted_blocks'] = dict(zip(
            ('batches', 'attempts', 'succeeded', 'adopted', 'serial_fallbacks'), map(int, match.groups()))) if match else None
        # System actions can legitimately be unsupported. Never require an
        # artificial private success or label their safe serial path parallel.
        row['adoption_scope'] = 'aggregate attempts including aborts; not per-kind unique adoption'
        finality = row['finality']
        row['monitor_errors'] = monitor_errors
        row['valid'] = (not monitor_errors and not row['producer_health_errors'] and
            finality['measurement_complete_without_sampling_or_sender_errors'] and
            finality['sent_unique_tps'] >= .98 * args.rate and not finality['rpc_error_events'] and
            not finality['cohort_unresolved_at_end'])
        return row
    except Exception as error:
        row['error_type'] = type(error).__name__
        row['error'] = str(error)
        row['error_traceback'] = traceback.format_exc()
        return row
    finally:
        stop.set()
        if thread is not None:
            thread.join()
        row['monitor_errors'] = monitor_errors
        cleanup_errors = []
        for node in (producer, validator):
            try:
                node.stop(check=False)
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
        if cleanup_errors:
            row.update(valid=False, cleanup_errors=cleanup_errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('nodeos', 'fixture-exporter', 'presigned-sender', 'quiet-gate', 'output-parent',
                 'system-contracts', 'simple-wasm', 'complex-wasm'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--mode', choices=('token', 'system', 'simple', 'complex', 'mixed', 'balanced'), default='mixed')
    parser.add_argument('--workers', default='0,6')
    parser.add_argument('--validator-workers', type=int, default=4)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--rate', type=int, required=True)
    parser.add_argument('--seconds', type=int, default=10)
    parser.add_argument('--warmup', type=int, default=2)
    parser.add_argument('--drain', type=int, default=70)
    parser.add_argument('--batch-size', type=int, default=12)
    parser.add_argument('--complex-roots', type=int, default=16)
    parser.add_argument('--iterations', type=int, default=62500)
    parser.add_argument('--hot-percent', type=int, default=0)
    parser.add_argument('--unique-business-rows', action='store_true',
                        help='Deploy lightbench: every corpus TX updates its own preallocated primary key')
    parser.add_argument('--state-db-mb', type=int, choices=(64, 256), default=64)
    parser.add_argument('--chain-threads', type=int, choices=(2, 6, 12), default=2)
    parser.add_argument('--transfer-pairs', type=int, choices=(64, 1024), default=64)
    parser.add_argument('--test-block-cpu-us', type=int, choices=(200000, 800000, 3000000, 6000000))
    parser.add_argument('--test-block-net-bytes', type=int, choices=(1048576, 8388608))
    parser.add_argument('--saturation-diagnostic', action='store_true',
                        help='TEST ONLY: explicitly permit >800000 us in a fresh isolated genesis, never production defaults')
    args = parser.parse_args()
    workers = [int(value) for value in args.workers.split(',')]
    count = args.rate * (args.seconds + args.warmup)
    require(args.test_block_cpu_us in (None, 200000, 800000) or args.saturation_diagnostic,
            'Higher test block CPU requires explicit saturation diagnostic')
    require(args.test_block_net_bytes in (None, 1048576) or args.saturation_diagnostic,
            'Higher test block NET requires explicit saturation diagnostic')
    require(not args.unique_business_rows or (args.mode == 'simple' and args.hot_percent == 0 and
            args.iterations == 0 and args.complex_roots == 1 and args.state_db_mb == 256),
            'Unique light rows require simple/zero work/one root/no hotspot/256 MiB state')
    require(args.transfer_pairs == 64 or (args.mode == 'token' and args.hot_percent == 0 and args.state_db_mb == 256),
            'Larger independent transfer pool requires token/no hotspot/256 MiB state')
    require(args.mode != 'balanced' or (args.hot_percent == 50 and count % 4 == 0),
            'Balanced mix requires --hot-percent 50 and a multiple-of-four corpus')
    require(0 in workers and 1 < len(workers) <= 4 and len(set(workers)) == len(workers) and
            all(0 <= value <= 128 for value in workers) and 0 <= args.validator_workers <= 128, 'Invalid worker modes')
    require(1 <= args.rounds <= 5 and 1 <= args.rate <= 32768 and 1 <= args.seconds <= 120 and
            0 <= args.warmup <= 10 and 1 <= args.drain <= 180 and 64 <= count <= 131072 and
            1 <= args.complex_roots <= 100 and count * args.complex_roots <= 2000000 and
            0 <= args.iterations <= 1000000 and 0 <= args.hot_percent <= 100 and
            1 <= args.batch_size <= 256 and len(workers) * (args.seconds + args.warmup + args.drain + 30) < 1200,
            'Invalid bounded workload/lifetime')
    os.umask(0o077)
    storage = preflight(args.output_parent)
    root = Path(tempfile.mkdtemp(prefix='parallel-real-stress-', dir=args.output_parent.resolve()))
    for key in ('nodeos', 'fixture_exporter', 'presigned_sender', 'quiet_gate', 'system_contracts', 'simple_wasm', 'complex_wasm'):
        setattr(args, key, getattr(args, key).resolve(strict=True))
    contracts = [args.system_contracts / name / (name + suffix)
                 for name in ('eosio.system', 'eosio.token', 'eosio.bios') for suffix in ('.wasm', '.abi')]
    contracts += [args.system_contracts / ('eosio.system/.powerup/powup.results' + suffix) for suffix in ('.wasm', '.abi')]
    sources = [Path(__file__), Path(__file__).with_name('parallel_mixed_stress_workload.py'),
               Path(__file__).with_name('parallel_saturation_metrics.py'),
               Path(__file__).with_name('parallel_pressure_metrics.py'),
               Path(__file__).with_name('parallel_release_benchmark.py'), Path(__file__).with_name('parallel_execution_test.py')]
    sources += [Path(__file__).parent / 'PerformanceHarness' / name for name in
                ('benchmark_environment.py', 'finality_tps.py', 'finality_benchmark.py', 'finality_cpp_benchmark.py')]
    files = [args.nodeos, args.fixture_exporter, args.presigned_sender, args.quiet_gate,
             args.simple_wasm, args.complex_wasm, *contracts, *sources]
    if args.unique_business_rows:
        files.append(args.simple_wasm.with_suffix('.abi'))
    files += [binary.with_suffix('.elf') for binary in (args.nodeos, args.fixture_exporter, args.presigned_sender)
              if binary.with_suffix('.elf').is_file()]
    frozen = {str(path): sha256(path) for path in files}
    report = {'status': 'running', 'root': str(root), 'inputs': frozen, 'storage': storage,
              'parameters': {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
              'workload': {'mode': args.mode, 'hot_percent': args.hot_percent, 'complex_roots': args.complex_roots,
                           'iterations_per_complex_root': args.iterations,
                           'mixed_weights': ({'simple': 50, 'complex': 50} if args.mode == 'balanced' else
                                             {'token': 40, 'simple': 20, 'system': 20, 'complex': 20})},
              'samples': [], 'scope': 'Observed finality TPS, real contracts/quotas; not proof of maximum capacity',
              'production_default_performance_evidence': args.test_block_cpu_us in (None, 200000) and
                                                         args.test_block_net_bytes in (None, 1048576)}
    print('Evidence: ' + str(root), flush=True)
    try:
        for repetition in range(args.rounds):
            group = root / f'round-{repetition + 1}'
            group.mkdir()
            corpus = group / 'corpus'
            env = {key: value for key, value in os.environ.items() if not key.startswith('PUREEOS_PARALLEL_')}
            env.update(PUREEOS_PARALLEL_STRESS_EXPORT_DIR=str(corpus), PUREEOS_PARALLEL_STRESS_COUNT=str(count),
                PUREEOS_PARALLEL_STRESS_WORK=str(args.iterations), PUREEOS_PARALLEL_STRESS_ROOTS=str(args.complex_roots),
                PUREEOS_PARALLEL_STRESS_HOT_PERCENT=str(args.hot_percent), PUREEOS_PARALLEL_STRESS_MODE=args.mode,
                PUREEOS_PARALLEL_STRESS_TRANSFER_PAIRS=str(args.transfer_pairs),
                PUREEOS_PARALLEL_STRESS_CONTRACTS=str(args.system_contracts),
                PUREEOS_PARALLEL_STRESS_SIMPLE_WASM=str(args.simple_wasm),
                PUREEOS_PARALLEL_STRESS_COMPLEX_WASM=str(args.complex_wasm))
            if args.test_block_cpu_us is not None:
                env['PUREEOS_PARALLEL_STRESS_TEST_CPU'] = str(args.test_block_cpu_us)
            if args.saturation_diagnostic:
                env['PUREEOS_PARALLEL_STRESS_SATURATION'] = '1'
            if args.test_block_net_bytes is not None:
                env['PUREEOS_PARALLEL_STRESS_TEST_NET'] = str(args.test_block_net_bytes)
            if args.unique_business_rows:
                env['PUREEOS_PARALLEL_STRESS_UNIQUE_ROWS'] = '1'
            with (group / 'export.log').open('x') as output:
                result = subprocess.run([str(args.fixture_exporter),
                    '--run_test=parallel_mixed_stress_fixtures/export_system_application_mixed_fixture',
                    '--report_level=detailed', '--color_output=no'], env=env, cwd=group,
                    stdout=output, stderr=subprocess.STDOUT, timeout=600)
            require(result.returncode == 0, 'Real-contract fixture export failed; see retained export.log')
            manifest = checked_manifest(corpus)
            require(manifest['fixture_kind'] == 'real-system-mixed-stress' and manifest['mode'] == args.mode and
                    manifest['hot_percent'] == args.hot_percent and manifest['complex_roots'] == args.complex_roots and
                    manifest['loop_iterations'] == args.iterations, 'Exporter workload mismatch')
            require(manifest.get('transfer_pairs', 64) == args.transfer_pairs, 'Transfer-pair pool mismatch')
            genesis = json.loads((corpus / 'genesis.json').read_text())
            expected = dict(manifest['default_initial_configuration'])
            if args.test_block_cpu_us is not None:
                expected['max_block_cpu_usage'] = args.test_block_cpu_us
            if args.test_block_net_bytes is not None:
                expected['max_block_net_usage'] = args.test_block_net_bytes
            require(genesis['initial_configuration'] == expected and genesis['initial_savanna']['version'] == 2,
                    'Unexpected chain/resource policy')
            entries = json.loads((corpus / 'transactions.json').read_text())
            require(len(entries) == count and len({entry['id'] for entry in entries}) == count, 'Corpus identity mismatch')
            if args.mode == 'balanced':
                report['workload']['balanced_profile'] = balanced_profile(manifest, entries)
            if args.unique_business_rows:
                report['workload']['independent_light_profile'] = independent_light_profile(manifest, entries)
            if args.mode == 'token' and args.transfer_pairs > 64:
                report['workload']['independent_transfer_profile'] = independent_transfer_profile(manifest, entries)
            order = workers[repetition % len(workers):] + workers[:repetition % len(workers)]
            for mode in order:
                require(all(sha256(Path(path)) == value for path, value in frozen.items()), 'Frozen input changed')
                row = run_case(args, group / f'workers-{mode}', corpus, manifest, entries, args.nodeos,
                               args.presigned_sender, args.quiet_gate, storage, mode, repetition + 1)
                report['samples'].append(row)
                print(json.dumps({'round': repetition + 1, 'workers': mode, 'valid': row['valid'],
                                  'tps': row.get('finality', {}).get('finalized_success_tps'),
                                  'error': row.get('error')}), flush=True)
                require(row['valid'], 'Invalid stress sample; retained failure, no performance claim')
        report['comparison'] = summarize(report['samples'], workers, args.rounds)
        report['status'] = 'passed' if report['comparison']['available'] else 'inconclusive'
    except KeyboardInterrupt:
        report['status'] = 'interrupted'
    except Exception as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
    finally:
        if not all(sha256(Path(path)) == value for path, value in frozen.items()):
            report.update(status='failed', input_integrity_failure=True)
        with (root / 'report.json').open('x') as output:
            json.dump(report, output, indent=2)
    print(json.dumps({'status': report['status'], 'report': str(root / 'report.json')}), flush=True)
    return 0 if report['status'] == 'passed' else 2


if __name__ == '__main__':
    raise SystemExit(main())
