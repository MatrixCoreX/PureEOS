#!/usr/bin/env python3
"""Read saved evidence only. Never changes chain parameters or drops samples."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import statistics

from stage_report import compare


def counter_delta(before, after, fields):
    if before.get('start_ticks') != after.get('start_ticks'):
        return None  # PID/TID reuse, not a negative/huge CPU observation
    values = [after[k] - before[k] for k in fields]
    return None if any(v < 0 for v in values) else sum(values)


def block_saturation(rows, cpu_limit):
    if cpu_limit <= 0:
        raise ValueError('Invalid block CPU limit')
    nonempty = [r for r in rows if r['transactions'] > 0]
    transactions = sum(r['transactions'] for r in nonempty)
    return {'nonempty_blocks': len(nonempty),
        'transactions': transactions,
        'mean_receipt_cpu_us_per_transaction': sum(r['receipt_cpu_us'] for r in nonempty) / transactions if transactions else None,
        'nonempty_at_least_99_percent_receipt_cpu': sum(r['receipt_cpu_us'] >= .99 * cpu_limit for r in nonempty),
        'scope': 'All produced business blocks, including warmup/drain; not just the TPS measurement window.'}


def window_summary(samples, start, end, pids, harness_pid):
    samples = [s for s in samples if start <= s['at_ns'] < end]
    if len(samples) < 2:
        return {'available': False, 'reason': 'fewer than two in-window samples'}
    hz = samples[0]['clock_ticks_per_second']
    if not hz or any(s['clock_ticks_per_second'] != hz for s in samples):
        raise ValueError('Inconsistent tick units')
    seconds = (samples[-1]['at_ns'] - samples[0]['at_ns']) / 1e9
    if seconds <= 0:
        raise ValueError('Non-increasing sample clock')
    categories = defaultdict(float)
    processes, threads = {}, {}
    roles = {str(pid): role for role, pid in pids.items()}
    roles[str(harness_pid)] = 'harness'
    waits_available = all(s['scheduler']['kernel_schedstats'] == 1 for s in samples)
    for before, after in zip(samples, samples[1:]):
        if after['at_ns'] <= before['at_ns']:
            raise ValueError('Non-increasing sample clock')
        old, new = before['scheduler'], after['scheduler']
        for pid in old['process_cpu'].keys() & new['process_cpu'].keys():
            a, b = old['process_cpu'][pid], new['process_cpu'][pid]
            ticks = counter_delta(a, b, ('user_ticks', 'system_ticks'))
            if ticks is None:
                continue
            role = roles.get(pid, 'sender' if b['name'].startswith('presigned_tps') else 'other')
            categories[role] += ticks / hz
            key = (pid, b['start_ticks'])
            item = processes.setdefault(key, {'pid': pid, 'name': b['name'], 'role': role, 'cpu_seconds': 0})
            item['cpu_seconds'] += ticks / hz
        for pid in roles:
            a = old['threads'].get(pid, {})
            b = new['threads'].get(pid, {})
            for tid in a.keys() & b.keys():
                ticks = counter_delta(a[tid], b[tid], ('user_ticks', 'system_ticks'))
                if ticks is None:
                    continue
                key = (pid, tid, b[tid]['start_ticks'])
                item = threads.setdefault(key, {'role': roles[pid], 'tid': tid,
                    'name': b[tid]['name'], 'cpu_seconds': 0, 'runqueue_wait_seconds': 0 if waits_available else None,
                    'voluntary_context_switches': 0, 'involuntary_context_switches': 0})
                item['cpu_seconds'] += ticks / hz
                for field, label in (('voluntary_ctxt_switches', 'voluntary_context_switches'),
                                     ('nonvoluntary_ctxt_switches', 'involuntary_context_switches')):
                    x, y = a[tid]['context_switches'].get(field), b[tid]['context_switches'].get(field)
                    if x is not None and y is not None and y >= x:
                        item[label] += y - x
                if waits_available:
                    wait = b[tid]['schedstat'][1] - a[tid]['schedstat'][1]
                    if wait < 0:
                        raise ValueError('Scheduler counter reset')
                    item['runqueue_wait_seconds'] += wait / 1e9
    # Guest time is already included in user/nice: use the first eight counters.
    delta = [b - a for a, b in zip(samples[0]['cpu_ticks'][:8], samples[-1]['cpu_ticks'][:8])]
    if sum(delta) <= 0 or min(delta) < 0:
        raise ValueError('Host CPU counter reset')
    frequency = [statistics.mean(s['frequency_khz'].values()) / 1000
                 for s in samples if s['frequency_khz']]
    return {'available': True, 'samples': len(samples), 'sampled_seconds': seconds,
        'host_idle_percent': 100 * delta[3] / sum(delta),
        'host_busy_cpu_seconds_excluding_iowait': (sum(delta) - delta[3] - delta[4]) / hz,
        'observed_process_cpu_seconds': dict(categories),
        'top_processes': sorted(processes.values(), key=lambda x: x['cpu_seconds'], reverse=True)[:12],
        'node_threads': sorted(threads.values(), key=lambda x: (x['role'], -x['cpu_seconds'])),
        'mean_reported_frequency_mhz': statistics.mean(frequency) if frequency else None,
        'mean_sample_collection_ms': statistics.mean(s['collection_ns'] for s in samples) / 1e6,
        'max_sample_collection_ms': max(s['collection_ns'] for s in samples) / 1e6,
        'scheduler_wait_available': waits_available,
        'scope': 'In-window adjacent observations only; exited/inaccessible/reused tasks omitted. Other includes background apps and diagnostic tools. CPU seconds are not billed receipt CPU; thread waits overlap.'}


def analyze(path):
    path = Path(path)
    report = json.loads(path.read_text())
    root = path.parent
    result = {'source': str(path), 'source_status': report['status'], 'samples': []}
    for row in report['samples']:
        case = root / f"round-{row['round']}" / f"workers-{row['workers']}"
        item = {'round': row['round'], 'workers': row['workers'], 'valid': row['valid'],
                'validator_workers': row['validator_workers']}
        result['samples'].append(item)
        if not (case / 'observer.jsonl').is_file() or not (case / 'host.jsonl').is_file():
            item['available'] = False
            continue
        window = next(json.loads(line) for line in (case / 'observer.jsonl').read_text().splitlines()
                      if json.loads(line)['type'] == 'window')
        samples = [json.loads(line) for line in (case / 'host.jsonl').read_text().splitlines()]
        item['host'] = window_summary(samples, window['start_ns'], window['end_ns'],
                                      row['node_pids'], report['harness_pid'])
        for key in ('receipt_capacity', 'production_attempts_including_aborted_blocks',
                    'production_diagnostics_including_aborted_blocks'):
            item[key] = row.get(key)
        item['finalized_tps'] = row.get('finality', {}).get('finalized_success_tps')
        if (case / 'block-receipts.json').is_file() and row.get('receipt_capacity'):
            blocks = json.loads((case / 'block-receipts.json').read_text())
            limit = row['receipt_capacity']['max_block_cpu_usage']
            item['business_block_saturation'] = block_saturation(blocks, limit)
            observed = set()
            with (case / 'observer.jsonl').open() as source:
                for line in source:
                    event = json.loads(line)
                    if event['type'] == 'block' and window['start_ns'] <= event['at_ns'] < window['end_ns']:
                        observed.add(event['id'])
            item['observed_window_block_saturation'] = block_saturation([b for b in blocks if b['id'] in observed], limit)
            item['observed_window_block_saturation']['scope'] = 'Business blocks first observed inside the window; NOT the exact finalized-transaction cohort.'
        if (case / 'metrics-after.txt').is_file():
            item['whole_sample_stages'] = compare((case / 'metrics-before.txt').read_text(),
                                                 (case / 'metrics-after.txt').read_text())
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.report), indent=2))
