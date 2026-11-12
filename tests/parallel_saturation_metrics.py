"""CPU occupancy diagnostics, NOT a replacement for the performance noise gate."""
from collections import defaultdict
from pathlib import Path


def logical_cpu_ticks(path='/proc/stat'):
    return {parts[0]: list(map(int, parts[1:9]))
            for line in Path(path).read_text().splitlines()
            if (parts := line.split()) and parts[0].startswith('cpu') and parts[0][3:].isdigit()}


def utilization(samples, start_ns, end_ns):
    """Whole sample intervals contained in first-send/last-finalization span.

    Per-TID usage means task CPU time, not known worker identity or proof that
    those threads executed simultaneously. No discarded interval makes a
    correctness/performance result eligible; the existing gate stays separate.
    """
    result = {'scope': 'whole sample intervals inside first send to last finalization; '
                       'task CPU is not worker identity or simultaneous execution proof',
              'intervals': 0, 'missing_intervals': 0, 'observed_seconds': 0,
              'requested_start_ns': start_ns, 'requested_end_ns': end_ns}
    cpu_busy = defaultdict(float)
    process_seconds = defaultdict(float)
    thread_seconds = defaultdict(float)
    peak, above90, consecutive, longest, active_peak = 0, 0, 0, 0, 0
    for before, after in zip(samples, samples[1:]):
        if before['at_ns'] < start_ns or after['at_ns'] > end_ns:
            continue
        try:
            seconds = (after['at_ns'] - before['at_ns']) / 1e9
            hz = before['clock_ticks_per_second']
            a, b = before['logical_cpu_ticks'], after['logical_cpu_ticks']
            if not 0 < seconds <= 3 or hz != after['clock_ticks_per_second'] or hz <= 0 or not a or a.keys() != b.keys():
                raise ValueError('CPU topology, clock or sampling gap changed')
            interval_cpu = {}
            for cpu, values in a.items():
                delta = [q - p for p, q in zip(values, b[cpu])]
                if len(delta) != 8 or min(delta) < 0 or sum(delta) <= 0:
                    raise ValueError('Invalid CPU counters')
                interval_cpu[cpu] = (sum(delta) - delta[3] - delta[4]) / sum(delta)
            interval_process, interval_thread = {}, {}
            if before['processes'].keys() != after['processes'].keys():
                raise ValueError('PID set changed')
            for pid, p in before['processes'].items():
                q = after['processes'][pid]
                if p.get('unavailable') or q.get('unavailable') or p['start_ticks'] != q['start_ticks']:
                    raise ValueError('Missing/reused process')
                ticks = sum(q[k] - p[k] for k in ('user_ticks', 'system_ticks'))
                if ticks < 0:
                    raise ValueError('Reset process counter')
                interval_process[pid] = ticks / hz
            for pid, threads in before['scheduler']['threads'].items():
                following = after['scheduler']['threads'].get(pid, {})
                for tid, p in threads.items():
                    q = following.get(tid)
                    if q is None or p['start_ticks'] != q['start_ticks']:
                        continue  # exited/new tasks remain explicitly unobserved
                    ticks = sum(q[k] - p[k] for k in ('user_ticks', 'system_ticks'))
                    if ticks < 0:
                        raise ValueError('Reset task counter')
                    interval_thread[(pid, tid, p['start_ticks'])] = ticks / hz
            occupancy = sum(interval_cpu.values()) / len(interval_cpu)
            result['intervals'] += 1
            result['observed_seconds'] += seconds
            for cpu, value in interval_cpu.items():
                cpu_busy[cpu] += seconds * value
            for pid, value in interval_process.items():
                process_seconds[pid] += value
            for task, value in interval_thread.items():
                thread_seconds[task] += value
            peak = max(peak, occupancy)
            active_peak = max(active_peak, sum(value / seconds >= .05 for value in interval_thread.values()))
            if occupancy >= .9:
                above90 += seconds
                consecutive += seconds
                longest = max(longest, consecutive)
            else:
                consecutive = 0
        except (KeyError, ValueError, TypeError, IndexError):
            result['missing_intervals'] += 1
            consecutive = 0
    wall = result['observed_seconds']
    result['status'] = 'complete' if wall and not result['missing_intervals'] else 'incomplete'
    if wall:
        result.update(logical_cpus=len(cpu_busy),
            mean_host_busy_percent=100 * sum(cpu_busy.values()) / len(cpu_busy) / wall,
            peak_interval_host_busy_percent=100 * peak,
            host_at_least_90_percent_seconds=above90, longest_host_at_least_90_percent_seconds=longest,
            per_logical_cpu_mean_busy_percent={cpu: 100 * value / wall for cpu, value in cpu_busy.items()},
            per_process_mean_cores={pid: value / wall for pid, value in process_seconds.items()},
            peak_tasks_using_at_least_5_percent_in_one_interval=active_peak,
            top_task_mean_cores=[{'pid': pid, 'tid': tid, 'start_ticks': start, 'cores': value / wall}
                for (pid, tid, start), value in sorted(thread_seconds.items(), key=lambda pair: -pair[1])[:32]])
    return result
