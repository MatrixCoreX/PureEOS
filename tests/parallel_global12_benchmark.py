#!/usr/bin/env python3
"""Whole-corpus independent calls/transfers; paired WASM0/0 versus12/12.

Business independence is checked over every transaction, not just a batch.
Code/metadata reads and canonical settlement remain real. This does NOT force
parallel admission, relax deadlines, or pretend 12 workers are 12 busy CPUs.
"""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import threading
import time
from types import SimpleNamespace

from parallel_execution_test import Node, checked_manifest, require, sha256
from parallel_mixed_stress_benchmark import run_case
from parallel_mixed_stress_workload import independent_light_profile, independent_transfer_profile, check_ledger
from parallel_release_benchmark import (parallel_diagnostics, admission_diagnostics, intake_diagnostics,
    measure_presigned, pin_paused_head, wait_for, final_ids, head_business_ids, receipt_capacity,
    producer_health_errors, check_live_resource_policy, finish_runtime_cpu_evidence, end_runtime_observation)
from benchmark_environment import preflight, sample, runtime_cpu_evidence, wait_for_quiet_gate
from parallel_saturation_metrics import utilization, logical_cpu_ticks
from parallel_pressure_metrics import corpus_completion
from finality_tps import read_events


def configuration_matrix(smoke=False):
    if smoke:
        return [(1, 0, 0), (1, 12, 12)]
    return [(1, 0, 0), (1, 12, 12), (2, 12, 12), (2, 0, 0),
            (3, 0, 0), (3, 12, 12), (0, 0, 0), (0, 12, 12)]


def run_staged_case(args, case, corpus, manifest, entries, workers, round_number, storage):
    """Production measured alone; independent P2P verifier starts afterwards."""
    import shutil
    case.mkdir()
    row = {'round': round_number, 'workers': workers, 'validator_workers': workers, 'valid': False,
           'measurement_scope': 'producer alone; independent validation AFTER measured production',
           'quiet_gate': wait_for_quiet_gate(args.quiet_gate, case / 'quiet-gate')}
    producer = Node(args.nodeos, case / 'producer', corpus, manifest, workers, peer='listen', engine='jit', batch_size=24)
    validator = Node(args.nodeos, case / 'validator', corpus, manifest, workers, peer=producer.endpoint, engine='jit', batch_size=24)
    stop, monitor_errors = threading.Event(), []
    thread = None
    try:
        lines = ['producer-name = eosio', 'enable-stale-production = true', 'pause-on-startup = true',
                 'read-mode = head', 'max-transaction-time = 100', 'performance-metrics = false']
        if workers: lines.append('experimental-parallel-production = true')
        lines += ['signature-provider = ' + key for key in json.loads((corpus / 'TEST-ONLY-keys.json').read_text())]
        with (producer.root / 'config/config.ini').open('a') as output: output.write('\n'.join(lines) + '\n')
        options = ['--genesis-json', str(corpus / 'genesis.json'), '--chain-state-db-size-mb', '256', '--chain-threads', '12']
        producer.start('live', options)
        row['node_process_ids'] = {'producer': producer.process.pid, 'validator': None}
        for point in manifest['checkpoints']:
            producer.feed(point)
        configuration = json.loads((corpus / 'genesis.json').read_text())['initial_configuration']
        row['live_resource_policy_before'] = {'producer': check_live_resource_policy(producer.rpc('get_consensus_parameters'), configuration)}
        def monitor():
            with (case / 'host.jsonl').open('x') as output:
                while not stop.is_set():
                    try:
                        state = sample([producer.process.pid], include_scheduler=True)
                        state['logical_cpu_ticks'] = logical_cpu_ticks()
                        output.write(json.dumps(state) + '\n'); output.flush()
                        if state['memory_kib'].get('MemAvailable', 0) < 1024 * 1024: monitor_errors.append('low-memory')
                        if shutil.disk_usage(case).free < storage['reserve_bytes']: monitor_errors.append('disk-reserve-exhausted')
                        if any(p.get('unavailable') for p in state['processes'].values()): monitor_errors.append('node-exited')
                        for process in Path('/proc').iterdir():
                            if not process.name.isdigit(): continue
                            try:
                                if re.fullmatch(r'cc1(?:plus)?|clang(?:\+\+)?(?:-\d+)?|rustc|cargo|cmake|g?make|ninja|lto1',
                                                (process / 'comm').read_text().strip()): monitor_errors.append('concurrent-compiler')
                            except OSError: pass
                    except Exception as error: monitor_errors.append(type(error).__name__)
                    stop.wait(1)
        thread = threading.Thread(target=monitor); thread.start()
        producer.rpc('resume', namespace='producer')
        measurement = SimpleNamespace(endpoint=f'http://127.0.0.1:{producer.port}',
            observer_endpoint=f'http://127.0.0.1:{producer.port}', chain_id=manifest['chain_id'],
            evidence=case / 'finality.jsonl', rate=args.rate, seconds=args.seconds, warmup=0,
            drain=args.drain, poll=.1, timeout=10, inflight=32)
        measurement.on_observation_end = lambda: end_runtime_observation(stop, thread, case,
            [producer.process.pid], storage, monitor_errors, True, logical_cpu_ticks)
        row['finality'] = measure_presigned(measurement, args.presigned_sender, corpus, producer.p2p_port)
        producer.rpc('pause', namespace='producer')
        stop.set(); thread.join(); thread = None
        host = finish_runtime_cpu_evidence(row, case, [producer.process.pid], storage, monitor_errors,
                                           scheduler=True, logical_ticks=logical_cpu_ticks)
        events = list(read_events(case / 'finality.jsonl'))
        row['corpus_completion'] = corpus_completion(events)
        row['cpu_utilization'] = utilization(host, row['corpus_completion']['first_send_ns'], row['corpus_completion']['last_finalization_ns'])
        info, state = pin_paused_head(producer)
        # Producer remains paused and only serves blocks. The verifier was NOT
        # running during the performance span; this is explicit, not full-chain TPS.
        verify_started = time.monotonic()
        validator.start('live', options)
        row['node_process_ids']['validator'] = validator.process.pid
        wait_for(lambda: validator.rpc('get_info')['head_block_id'] == info['head_block_id'], 'independent P2P verifier catches head', timeout=600)
        row['independent_verification_wall_seconds_including_bootstrap'] = time.monotonic() - verify_started
        require(state == validator.rpc('get_integrity_hash', namespace='producer'), 'Replicated state diverged')
        row['state'] = state
        row['checkpoint'] = {'block_num': info['head_block_num'], 'block_id': info['head_block_id'], 'integrity_hash': state['integrity_hash']}
        capacity = []
        executed = head_business_ids(validator, info['head_block_id'], manifest['checkpoints'][-1]['block_id'], capacity)
        sent = {event['id'] for event in events if event['type'] == 'sent'}
        require(executed <= sent and sent <= final_ids(events) and len(executed) == len(entries), 'Unexecuted or unfinalized independent TX')
        row['ledger'] = check_ledger(validator, manifest, entries, executed)
        row['receipt_capacity'] = receipt_capacity(capacity, configuration)
        with (case / 'block-receipts.json').open('x') as output: json.dump(capacity, output)
        row['live_resource_policy_after'] = {node.root.name: check_live_resource_policy(node.rpc('get_consensus_parameters'), configuration)
                                              for node in (producer, validator)}
        producer.stop(); validator.stop()
        log = producer.log.read_text()
        row['producer_health_errors'] = producer_health_errors(log)
        row['production_diagnostics_including_aborted_blocks'] = parallel_diagnostics(log)
        row['production_admission_including_aborted_blocks'] = admission_diagnostics(log)
        row['live_intake_dispatch_counts'] = intake_diagnostics(log)
        match = re.search(r'Experimental parallel production totals: batches=(\d+), attempts=(\d+), succeeded=(\d+), adopted=(\d+), serial_fallbacks=(\d+)', log)
        row['production_attempts_including_aborted_blocks'] = dict(zip(('batches','attempts','succeeded','adopted','serial_fallbacks'), map(int,match.groups()))) if match else None
        finality = row['finality']
        row['valid'] = (not monitor_errors and not row['producer_health_errors'] and
            finality['measurement_complete_without_sampling_or_sender_errors'] and
            finality['sent_unique_tps'] >= .98 * args.rate and not finality['rpc_error_events'] and not finality['cohort_unresolved_at_end'])
    except Exception as error:
        import traceback
        row.update(error_type=type(error).__name__, error=str(error), error_traceback=traceback.format_exc())
    finally:
        stop.set()
        if thread is not None: thread.join()
        row['monitor_errors'] = monitor_errors
        for node in (producer, validator): node.stop(check=False)
    return row


def pipeline_timing(producer_log, validator_log, anchor, expected_count):
    """Actual log completion times, not block slots or claimed steady TPS."""
    pattern = re.compile(r'info\s+(\S+)\s+.*?(Produced|Received) block ([0-9a-f]+)\.\.\. #([0-9]+).*?'
                         r'\[trxs: ([0-9]+),.*?elapsed: ([0-9]+) us(?:, applying time: ([0-9]+) us)?')
    nodes = {}
    for role, log, operation in [('producer', producer_log, 'Produced'), ('validator', validator_log, 'Received')]:
        blocks = {}
        for line in log.splitlines():
            match = pattern.search(line)
            if not match or match[2] != operation or int(match[4]) <= anchor or int(match[5]) == 0:
                continue
            number = int(match[4])
            require(number not in blocks, 'Repeated business block log; do not silently select a fork')
            blocks[number] = {'at': datetime.fromisoformat(match[1]), 'prefix': match[3], 'transactions': int(match[5]),
                              'elapsed_us': int(match[6]), 'applying_time_us': int(match[7]) if match[7] else None}
        require(blocks and sum(block['transactions'] for block in blocks.values()) == expected_count,
                'Business log count incomplete')
        nodes[role] = blocks
    require(nodes['producer'].keys() == nodes['validator'].keys() and all(
        nodes['producer'][number]['prefix'] == nodes['validator'][number]['prefix'] for number in nodes['producer']),
        'Producer and validator business block logs differ')
    last = max(nodes['producer'])
    result = {'scope': 'wall log completion of business blocks after exported anchor; not steady-state TPS',
              'business_blocks': len(nodes['producer']),
              'validator_lag_last_business_block_ms':
                  (nodes['validator'][last]['at'] - nodes['producer'][last]['at']).total_seconds() * 1000}
    for role, blocks in nodes.items():
        times = [block['at'] for block in blocks.values()]
        result[role] = {'first_business_completion_utc': min(times).isoformat(),
                        'last_business_completion_utc': max(times).isoformat(),
                        'sum_reported_block_elapsed_us': sum(block['elapsed_us'] for block in blocks.values())}
        if role == 'validator':
            require(all(block['applying_time_us'] is not None for block in blocks.values()), 'Missing validator apply timing')
            result[role]['sum_reported_applying_time_us'] = sum(block['applying_time_us'] for block in blocks.values())
    return result


def compare(samples):
    """Report all paired samples; zero fixed-window baselines are not errors."""
    import statistics
    selected = {label: [row for row in samples if row['pipeline'] == label and row['round'] > 0]
                for label in ('wasm0-0', 'wasm12-12')}
    require(all(rows and all(row['valid'] for row in rows) for rows in selected.values()), 'Missing paired sample')
    result = {'metric': 'unique whole corpus / first send to LAST finalization; includes confirmation latency',
              'scope': 'paired diagnostic, not absolute capacity; all failed/noisy samples retained', 'modes': {}}
    eligible = all(row['runtime_cpu_evidence']['status'] == 'eligible' for rows in selected.values() for row in rows)
    for label, rows in selected.items():
        values = [row['corpus_completion']['tps'] for row in rows]
        result['modes'][label] = {'rounds': len(values), 'all_tps': values,
                                 'median_tps': statistics.median(values), 'min_tps': min(values), 'max_tps': max(values)}
    serial, parallel = result['modes']['wasm0-0'], result['modes']['wasm12-12']
    result['ratio_parallel_vs_serial'] = parallel['median_tps'] / serial['median_tps']
    result['runtime_noise_evidence'] = 'eligible' if eligible else 'inconclusive'
    result['repeatable_20_percent_whole_cohort_gain'] = (eligible and min(serial['rounds'], parallel['rounds']) >= 3 and
        result['ratio_parallel_vs_serial'] >= 1.2 and parallel['min_tps'] > serial['max_tps'])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('nodeos', 'fixture-exporter', 'presigned-sender', 'quiet-gate', 'output-parent',
                 'system-contracts', 'simple-wasm', 'complex-wasm'):
        parser.add_argument('--' + flag, type=Path, required=True)
    parser.add_argument('--smoke', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    storage = preflight(args.output_parent)
    root = Path(tempfile.mkdtemp(prefix='parallel-global12-', dir=args.output_parent.resolve()))
    for flag in ('nodeos', 'fixture_exporter', 'presigned_sender', 'quiet_gate', 'system_contracts', 'simple_wasm', 'complex_wasm'):
        setattr(args, flag, getattr(args, flag).resolve(strict=True))
    frozen_paths = [args.nodeos, args.fixture_exporter, args.presigned_sender, args.quiet_gate,
        args.simple_wasm, args.simple_wasm.with_suffix('.abi'), args.complex_wasm,
        Path(__file__), *Path(__file__).parent.glob('parallel_*stress*.py')]
    frozen_paths += list(args.system_contracts.rglob('*.wasm')) + list(args.system_contracts.rglob('*.abi'))
    frozen_paths += [Path(__file__).with_name(name) for name in ('parallel_execution_test.py',
        'parallel_release_benchmark.py', 'parallel_pressure_metrics.py', 'parallel_saturation_metrics.py')]
    frozen_paths += [Path(__file__).parent / 'PerformanceHarness' / name for name in
        ('benchmark_environment.py', 'finality_tps.py', 'finality_benchmark.py')]
    frozen_paths += [path.with_suffix('.elf') for path in (args.nodeos, args.fixture_exporter, args.presigned_sender)]
    frozen = {str(path): sha256(path) for path in frozen_paths}
    count, rate, seconds, drain = (64, 64, 1, 10) if args.smoke else (8192, 8192, 1, 30)
    report = {'status': 'running', 'scope': 'all business callers/rows or sender/recipient pairs distinct across entire corpus',
              'parameters': {'count': count, 'rate': rate, 'seconds': seconds, 'drain': drain,
                  'batch_size': 24, 'block_cpu_us': 6000000, 'block_net_bytes': 8388608,
                  'chain_threads': 12, 'smoke': args.smoke, 'matrix': configuration_matrix(args.smoke)},
              'inputs': frozen, 'workloads': [], 'all_business_checks_passed': False}
    print('Evidence: ' + str(root), flush=True)
    try:
        for kind in ('simple', 'token'):
            directory = root / kind; directory.mkdir(); corpus = directory / 'corpus'
            env = {key: value for key, value in os.environ.items() if not key.startswith('PUREEOS_PARALLEL_')}
            env.update(PUREEOS_PARALLEL_STRESS_EXPORT_DIR=str(corpus), PUREEOS_PARALLEL_STRESS_COUNT=str(count),
                PUREEOS_PARALLEL_STRESS_WORK='0', PUREEOS_PARALLEL_STRESS_ROOTS='1', PUREEOS_PARALLEL_STRESS_HOT_PERCENT='0',
                PUREEOS_PARALLEL_STRESS_MODE=kind, PUREEOS_PARALLEL_STRESS_TRANSFER_PAIRS=str(count),
                PUREEOS_PARALLEL_STRESS_GLOBAL_ACCOUNTS='1', PUREEOS_PARALLEL_STRESS_UNIQUE_ROWS='1' if kind == 'simple' else '0',
                PUREEOS_PARALLEL_STRESS_TEST_CPU='6000000', PUREEOS_PARALLEL_STRESS_TEST_NET='8388608',
                PUREEOS_PARALLEL_STRESS_SATURATION='1', PUREEOS_PARALLEL_STRESS_CONTRACTS=str(args.system_contracts),
                PUREEOS_PARALLEL_STRESS_SIMPLE_WASM=str(args.simple_wasm if kind == 'simple' else args.system_contracts / 'statebench.wasm'),
                PUREEOS_PARALLEL_STRESS_COMPLEX_WASM=str(args.complex_wasm))
            print(json.dumps({'exporting': kind, 'transactions': count, 'distinct_account_pairs': count}), flush=True)
            with (directory / 'export.log').open('x') as output:
                exported = subprocess.run([str(args.fixture_exporter),
                    '--run_test=parallel_mixed_stress_fixtures/export_system_application_mixed_fixture',
                    '--report_level=detailed', '--color_output=no'], cwd=directory, env=env,
                    stdout=output, stderr=subprocess.STDOUT, timeout=1200)
            require(exported.returncode == 0, 'Independent fixture export failed; retained export.log')
            manifest = checked_manifest(corpus)
            entries = json.loads((corpus / 'transactions.json').read_text())
            require(manifest['globally_unique_accounts'] is True and len(entries) == count and
                    manifest['business_transactions'] == count, 'Wrong independent corpus')
            profile = (independent_light_profile if kind == 'simple' else independent_transfer_profile)(manifest, entries)
            genesis = json.loads((corpus / 'genesis.json').read_text())
            expected = dict(manifest['default_initial_configuration'])
            expected.update(max_block_cpu_usage=6000000, max_block_net_usage=8388608)
            require(genesis['initial_configuration'] == expected and genesis['initial_savanna']['version'] == 2,
                    'Unexpected genesis policy')
            workload = {'kind': kind, 'profile': profile, 'samples': []}; report['workloads'].append(workload)
            params = SimpleNamespace(rate=rate, seconds=seconds, warmup=0, drain=drain, batch_size=24,
                                     state_db_mb=256, chain_threads=12)
            for index, (round_number, producer_workers, validator_workers) in enumerate(configuration_matrix(args.smoke)):
                require(all(sha256(Path(path)) == value for path, value in frozen.items()), 'Frozen input changed')
                params.validator_workers = validator_workers
                label = f'wasm{producer_workers}-{validator_workers}'
                case = directory / f'{index + 1:02d}-{label}-round{round_number}'
                if round_number > 0:
                    params.nodeos, params.presigned_sender, params.quiet_gate = args.nodeos, args.presigned_sender, args.quiet_gate
                    row = run_staged_case(params, case, corpus, manifest, entries, producer_workers, round_number, storage)
                else:
                    row = run_case(params, case, corpus, manifest, entries, args.nodeos, args.presigned_sender,
                                   args.quiet_gate, storage, producer_workers, round_number)
                    row['measurement_scope'] = 'producer and independent verifier concurrently; end-to-end finality'
                row['pipeline'] = label
                if row['valid']:
                    try:
                        row['pipeline_timing'] = pipeline_timing((case / 'producer/live.log').read_text(),
                            (case / 'validator/live.log').read_text(), manifest['checkpoints'][-1]['block_num'], count)
                        if round_number > 0:
                            row['pipeline_timing'].pop('validator_lag_last_business_block_ms')
                            row['pipeline_timing']['scope'] = 'sequential production then independent P2P verification; NOT a concurrent lag measurement'
                        apply_us = row['pipeline_timing']['validator']['sum_reported_applying_time_us']
                        row['pipeline_timing']['validation_business_apply_tps'] = count * 1e6 / apply_us
                        row['validation_diagnostics_including_bootstrap'] = parallel_diagnostics(
                            (case / 'validator/live.log').read_text(), 'validation')
                    except Exception as error:
                        row.update(valid=False, diagnostic_error=str(error))
                workload['samples'].append(row)
                with (case / 'result.json').open('x') as output: json.dump(row, output, indent=2)
                print(json.dumps({'kind': kind, 'pipeline': label, 'round': round_number, 'valid': row['valid'],
                    'whole_corpus_tps': row.get('corpus_completion', {}).get('tps'), 'error': row.get('error'),
                    'diagnostic_error': row.get('diagnostic_error')}), flush=True)
                require(row['valid'], 'Invalid sample retained; do not continue blindly')
            workload['comparison'] = compare(workload['samples'])
        report.update(status='completed', all_business_checks_passed=True)
    except Exception as error:
        report.update(status='failed', error_type=type(error).__name__, error=str(error))
    finally:
        if not all(sha256(Path(path)) == value for path, value in frozen.items()):
            report.update(status='failed', all_business_checks_passed=False, input_integrity_failure=True)
        with (root / 'report.json').open('x') as output: json.dump(report, output, indent=2)
    print(json.dumps({'status': report['status'], 'report': str(root / 'report.json')}), flush=True)
    return 0 if report['all_business_checks_passed'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
