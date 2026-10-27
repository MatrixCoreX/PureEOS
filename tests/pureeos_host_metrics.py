#!/usr/bin/env python3
"""Structured read-only metrics for disposable Linux node test hosts.

An idle ps or a journal query with no matches may exit 1. Recognize only their
specific empty results; permission/tool/timeout errors remain explicit errors.
The CLI always emits a structured sample, never a generic shell failure.
"""
import json
import re
import subprocess
from datetime import datetime, timezone

COMMANDS = {
    'memory': ['free', '-b'],
    'processes': ['ps', '-C', 'nodeos', '-C', 'nodeos.elf', '-o', 'pid,pcpu,rss,nlwp,etimes,comm'],
    'oom': ['journalctl', '-k', '-b', '--no-pager', '--grep=Out of memory|Killed process|oom-kill', '-n', '5'],
}
PROCESS_HEADER = ['PID', '%CPU', 'RSS', 'NLWP', 'ELAPSED', 'COMMAND']
OOM = re.compile(r'Out of memory|Killed process|oom-kill', re.IGNORECASE)


def interpret(name, returncode, stdout, stderr):
    if name not in COMMANDS: raise ValueError('Unknown metric probe')
    result = dict(command=COMMANDS[name], returncode=returncode, stdout=stdout, stderr=stderr, status='error')
    if stderr.strip():
        result['error'] = 'Probe wrote stderr; evidence may be incomplete'
        return result
    if name == 'oom':
        if returncode in (0, 1) and stdout.strip() == '-- No entries --':
            result.update(status='empty', oom_detected=False)
        elif returncode == 0 and OOM.search(stdout):
            result.update(status='ok', oom_detected=True)
        else:
            result['error'] = 'Unrecognized journal result; not proof of no OOM'
        return result
    if name == 'processes':
        lines = stdout.strip().splitlines()
        if lines and lines[0].split() == PROCESS_HEADER:
            if returncode in (0, 1) and len(lines) == 1:
                result.update(status='empty', nodes=[])
                return result
            if returncode == 0:
                try:
                    nodes = []
                    for line in lines[1:]:
                        pid, cpu, rss, threads, elapsed, command = line.split(maxsplit=5)
                        assert command in ('nodeos', 'nodeos.elf')
                        row = dict(pid=int(pid), cpu_percent=float(cpu), rss_kib=int(rss),
                            threads=int(threads), elapsed_seconds=int(elapsed), command=command)
                        assert row['pid'] > 0 and row['rss_kib'] >= 0 and row['threads'] > 0 and row['elapsed_seconds'] >= 0
                        nodes.append(row)
                    result.update(status='ok', nodes=nodes)
                    return result
                except (ValueError, AssertionError): pass
        result['error'] = 'Unrecognized process result; not proof that no nodes exist'
        return result
    if returncode == 0:
        try:
            row = next(line.split() for line in stdout.splitlines() if line.startswith('Mem:'))
            values = list(map(int, row[1:]))
            assert len(values) == 6 and all(v >= 0 for v in values) and values[0] > 0
            result.update(status='ok', memory_bytes=dict(zip(
                ('total', 'used', 'free', 'shared', 'buffers_cache', 'available'), values)))
            return result
        except (StopIteration, ValueError, AssertionError): pass
    result['error'] = 'Memory command failed or returned unrecognized data'
    return result


def collect(run=subprocess.run, *, since=None):
    if since is not None:
        start = datetime.fromisoformat(since)
        if start.tzinfo is None:
            raise ValueError('Metric window must include an explicit timezone')
        since = start.astimezone(timezone.utc).isoformat()
    probes = {}
    for name, command in COMMANDS.items():
        command = list(command)
        if name == 'oom' and since is not None:
            command += ['--since', since]
        try:
            completed = run(command, capture_output=True, text=True, timeout=10)
            probes[name] = interpret(name, completed.returncode, completed.stdout, completed.stderr)
        except (OSError, subprocess.TimeoutExpired) as error:
            probes[name] = dict(status='error', command=command, returncode=None,
                error=type(error).__name__, detail=str(error))
    return dict(version=1, utc=datetime.now(timezone.utc).isoformat(), since_utc=since, probes=probes,
        status='complete' if all(p['status'] != 'error' for p in probes.values()) else 'incomplete')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--since', help='Explicit UTC/offset start of this test, not the entire host boot')
    args = parser.parse_args()
    print(json.dumps(collect(since=args.since)))
