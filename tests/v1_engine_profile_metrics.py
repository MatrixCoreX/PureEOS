#!/usr/bin/env python3
"""Read-only host/cgroup sampling during one exact local candidate, not TPS."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess
import time

NAME = 'pureeos-v1-local-engine-profile-retest-attempt-1'
IMAGE = 'sha256:5dd671bd834c70a74b076e4b0dc90e280958df1200d837d19e15df68fad4e83d'


def sha(path):
    with Path(path).open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()


def cpu_delta(before, after):
    if len(before) < 8 or len(after) < 8: raise ValueError('Incomplete CPU counters')
    delta = [b-a for a, b in zip(before[:8], after[:8])]
    if min(delta) < 0 or sum(delta) <= 0: return None
    total = sum(delta)
    return dict(total_ticks=total, busy_percent=100*(total-delta[3]-delta[4])/total,
        steal_percent=100*delta[7]/total, iowait_percent=100*delta[4]/total)


def explicit_absence(code, error, name):
    return code != 0 and re.search(r'No such (?:container|object):\s*'+re.escape(name)+r'(?:\s|$)',
        error, re.IGNORECASE) is not None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(exist_ok=False)
    assert args.core.name == 'local-engine-profile-retest-attempt-1'
    assert args.output.parent.resolve() == args.core.parent.resolve()
    stopped = False
    def stop(_sig, _frame):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop); signal.signal(signal.SIGINT, stop)
    report = dict(status='sampling', scope=__doc__, release_ready=False, samples=0, errors=[],
        started_utc=datetime.now(timezone.utc).isoformat(), source_sha256=sha(__file__),
        note='Bound to repaired engine-profile candidate only; not TPS or the original cloud failure')
    def save():
        pending = args.output/'report.pending'; pending.write_text(json.dumps(report, indent=2))
        pending.replace(args.output/'report.json')
    previous, cgroup, identity = None, None, None
    deadline, next_inspection, max_gap, last_sample = time.monotonic()+9*3600, 0, 0, None
    try:
        save()
        wait_deadline = time.monotonic()+10*3600
        while True:
            check = subprocess.run(['sudo', '-n', 'docker', 'container', 'inspect', NAME],
                capture_output=True, text=True, timeout=15)
            if check.returncode == 0: break
            if not explicit_absence(check.returncode, check.stderr, NAME):
                raise RuntimeError('Container wait failed, absence unproved: '+check.stderr)
            state = subprocess.check_output(['systemctl', '--user', 'show', NAME+'.service',
                '-p', 'ActiveState', '--value'], text=True, timeout=10).strip()
            if state not in ('active', 'activating') or time.monotonic() > wait_deadline or stopped:
                raise RuntimeError('Core never started before bounded wait/stop')
            report['status'] = 'waiting-for-container'; save(); time.sleep(2)
        report['status'] = 'sampling'
        report['sampling_started_utc'] = datetime.now(timezone.utc).isoformat()
        deadline = time.monotonic()+9*3600
        with (args.output/'samples.jsonl').open('x') as stream:
            while not stopped and time.monotonic() < deadline:
                start = time.monotonic()
                if start >= next_inspection:
                    result = subprocess.run(['sudo', '-n', 'docker', 'container', 'inspect', NAME],
                        capture_output=True, text=True, timeout=15)
                    if result.returncode:
                        state = subprocess.check_output(['systemctl', '--user', 'show', NAME+'.service',
                            '-p', 'ActiveState', '--value'], text=True, timeout=10).strip()
                        if identity is not None and explicit_absence(result.returncode, result.stderr, NAME):
                            report['status'] = 'finished-container-absent'; break
                        raise RuntimeError('Container absence unproved (core unit '+state+'); do not hide inspect/tool errors')
                    value = json.loads(result.stdout)[0]
                    assert value['Image'] == IMAGE and value['Config']['Labels'].get('org.pureeos.task') == NAME
                    assert re.fullmatch('[0-9a-f]{64}', value['Id'])
                    if identity is not None: assert identity == value['Id'], 'Never follow a replacement container'
                    identity = value['Id']; report['container_id'] = identity
                    if not value['State']['Running']:
                        report['status'] = 'finished-container-stopped'; break
                    pid = int(value['State']['Pid'])
                    relative = Path(f'/proc/{pid}/cgroup').read_text().strip().split('0::')[-1]
                    assert identity in relative and relative.startswith('/') and '..' not in Path(relative).parts
                    cgroup = Path('/sys/fs/cgroup')/relative.lstrip('/')
                    next_inspection = start+30
                row = dict(utc=datetime.now(timezone.utc).isoformat(), monotonic=start)
                current = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:]))
                row['cpu_ticks'] = current
                row['cpu_delta'] = cpu_delta(previous, current) if previous is not None else None
                previous = current
                row['loadavg'] = Path('/proc/loadavg').read_text().strip()
                row['pressure'] = {name: Path('/proc/pressure', name).read_text().strip()
                    for name in ('cpu', 'memory', 'io')}
                row['memory_kib'] = {parts[0].rstrip(':'): int(parts[1])
                    for line in Path('/proc/meminfo').read_text().splitlines()
                    if (parts := line.split()) and parts[0] in ('MemAvailable:', 'MemFree:', 'SwapFree:')}
                try:
                    row['container'] = {name: (cgroup/name).read_text().strip()
                        for name in ('memory.current', 'memory.swap.current', 'memory.events', 'cpu.stat', 'pids.current')}
                except FileNotFoundError:
                    row['container'] = {'status': 'cgroup-ended'}; next_inspection = 0
                progress = args.core/'report.json'
                try:
                    value = json.loads(progress.read_text())
                    row['candidate'] = dict(status=value['status'], completed_jobs=len(value['jobs']),
                        last_job=value['jobs'][-1]['name'] if value['jobs'] else None)
                except (FileNotFoundError, json.JSONDecodeError): row['candidate'] = {'status': 'report-write-in-progress'}
                stream.write(json.dumps(row)+'\n'); stream.flush()
                report['samples'] += 1
                if last_sample is not None: max_gap = max(max_gap, start-last_sample)
                last_sample = start
                if report['samples'] % 30 == 0:
                    report['latest_sample_utc'] = row['utc']; report['max_gap_seconds'] = max_gap; save()
                time.sleep(max(0, 1-(time.monotonic()-start)))
        if stopped: report['status'] = 'stopped-before-container-end'
        elif report['status'] == 'sampling': report['status'] = 'bounded-sampling-timeout'
    except Exception as error:
        report.update(status='sampling-incomplete', errors=[type(error).__name__+': '+str(error)])
    report.update(finished_utc=datetime.now(timezone.utc).isoformat(), max_gap_seconds=max_gap,
        samples_sha256=sha(args.output/'samples.jsonl') if (args.output/'samples.jsonl').is_file() else None)
    save()
    return 0 if report['status'] in ('finished-container-absent', 'finished-container-stopped') else 1


if __name__ == '__main__': raise SystemExit(main())

