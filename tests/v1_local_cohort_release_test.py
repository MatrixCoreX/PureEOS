#!/usr/bin/env python3
"""Real-clock 30 -> 40 candidate correctness on ONE isolated local host.

Reuse the cloud cohort's queue/ranking assertions, not its remote transport.
This is neither a throughput result nor forty independent failure domains.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import tempfile
import time

from pureeos_large_multinode_local_test import LargeMultiNodeTest
from pureeos_rotation_cohort_cloud_test import CohortCloudTest, cohort_names, full_seat_entries


class LocalCapacityError(RuntimeError):
    """Stop only this disposable workflow before exhausting the host."""


def capacity_guard(current, inactive_file, limit, available, *, minimum_available=768 * 1024**2):
    if min(current, inactive_file, available) < 0 or limit <= 0 or inactive_file > current:
        raise ValueError('Invalid memory accounting')
    if minimum_available < 768 * 1024**2:
        raise ValueError('Local reserve cannot be lower than 768 MiB')
    unreclaimable = current - inactive_file
    if unreclaimable * 100 >= limit * 88 or available < minimum_available:
        raise LocalCapacityError('Insufficient local memory headroom; no production failure inferred')
    return unreclaimable


def complete(report):
    phases = report.get('cohort_phases', [])
    fault = report.get('fault', {})
    recovery = report.get('recovered_fault', {})
    expected = set(cohort_names(40))
    return bool(report.get('status') == 'passed' and report.get('processes_stopped') is True
        and not report.get('shutdown_errors') and report.get('parallel_workers') == 4
        and report.get('physical_hosts') == 1 and report.get('production_clock_unchanged') is True
        and [p.get('candidate_count') for p in phases] == [30, 40]
        and all(p.get('status') == 'passed' and p.get('agreement')
            and set(cohort_names(p['candidate_count'])) <= set(p.get('queue_entries', []))
            and set(cohort_names(p['candidate_count'])) <= set(p.get('finalized_production', {}).get('counts', {}))
            and all(int(n) > 0 for n in p.get('finalized_production', {}).get('counts', {}).values())
            for p in phases)
        and {len(e['schedule']['producers']) for e in report.get('committee_events', [])} == set(range(1, 22))
        and recovery.get('missing', {}).get('zero_windows', 0) in (1, 2)
        and not recovery.get('missing', {}).get('banned', True)
        and recovery.get('recovered', {}).get('zero_windows') == 0
        and not recovery.get('recovered', {}).get('banned', True)
        and fault.get('candidate_after', {}).get('banned') is True
        and fault.get('candidate_after', {}).get('zero_windows') == 3
        and len(set(fault.get('candidate_after', {}).get('evidence_slots', []))) == 3
        and len(fault.get('actual_post_ban_entries', [])) >= 2
        and fault.get('node') not in fault.get('actual_post_ban_entries', [])
        and int(fault.get('preserved_unpaid_blocks', 0)) > 0
        and set(report.get('processes', {})) == expected
        and len({p['pid'] for p in report['processes'].values()}) == 40
        and report.get('final_synchronization'))


class LocalCohortTest(LargeMultiNodeTest):
    names = cohort_names(40)
    wallet_unlock_seconds = 30 * 3600
    # These methods use ordinary chain RPC and assertions only. The class does
    # NOT inherit CloudMultiNodeTest: no ssh(), remote(), or cloud provisioning.
    exact_ranking = CohortCloudTest.exact_ranking
    add_candidates = CohortCloudTest.add_candidates
    wait_candidate_sync = CohortCloudTest.wait_candidate_sync
    finalize_phase = CohortCloudTest.finalize_phase
    recover_before_ban = CohortCloudTest.recover_before_ban
    tests = CohortCloudTest.tests
    port = CohortCloudTest.port

    def __init__(self, build, contracts, state_parent):
        super().__init__(build, contracts, state_parent)
        self.http_port, self.p2p_port = 28000, 28001
        self.url = 'http://127.0.0.1:28000'
        self.ports = {self.http_port, self.p2p_port}
        low, high = map(int, Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())
        if any(low <= p <= high for p in range(28000, 28100)):
            raise RuntimeError('Disposable listener range overlaps ephemeral range')
        for p in (self.http_port, self.p2p_port):
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', p))
        self.registered, self.expected_units, self.phase_deadline = ['eosio'], {'eosio': 1000000}, None
        self.next_capacity_sample = 0
        self.report.update(scope=__doc__, endpoint=self.url, physical_hosts=1, parallel_workers=4,
            engine='jit', cohort_phases=[], recovered_fault={}, resource_samples=[],
            requested_candidate_phases=[30, 40], capacity_boundary_210_not_tested=True,
            actual_24_hour_payout_not_tested=True, tps_not_tested=True,
            local_cohort_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())

    def resources(self, label):
        root = Path('/sys/fs/cgroup')
        limit = int((root/'memory.max').read_text())
        stat = dict(line.split() for line in (root/'memory.stat').read_text().splitlines())
        current = int((root/'memory.current').read_text())
        mem = dict((line.split()[0].rstrip(':'), int(line.split()[1])*1024)
            for line in Path('/proc/meminfo').read_text().splitlines() if len(line.split()) == 3)
        sample = dict(label=label, utc=datetime.now(timezone.utc).isoformat(), current=current,
            limit=limit, inactive_file=min(current, int(stat.get('inactive_file', 0))), available=mem['MemAvailable'])
        sample['unreclaimable'] = current - sample['inactive_file']
        sample['components'] = {k: int(stat.get(k, 0)) for k in
            ('anon', 'file', 'file_mapped', 'file_dirty', 'active_file', 'kernel', 'pagetables')}
        sample['memory_events'] = {k: int(v) for k, v in
            (line.split() for line in (root/'memory.events').read_text().splitlines())}
        self.report['resource_samples'].append(sample)
        self.checkpoint()
        capacity_guard(current, sample['inactive_file'], limit, sample['available'], minimum_available=2 * 1024**3)
        self.next_capacity_sample = time.monotonic() + 30

    def start_peer(self, name, restart=False):
        self.resources('Before starting disposable peer ' + name)
        return super().start_peer(name, restart)

    def spawn(self, command, log_name):
        if command[0] == str(self.nodeos):
            command = [*command, '--wasm-runtime', 'eos-vm-jit', '--experimental-parallel-engine', 'jit',
                '--experimental-parallel-validation-threads', '4', '--experimental-parallel-validation-batch-size', '8',
                '--experimental-parallel-production']
            for name in ('eosio.token', 'eosio.system'):
                digest = hashlib.sha256((self.contracts/name/(name+'.wasm')).read_bytes()).hexdigest()
                command += ['--experimental-parallel-code-hash', digest]
        return super().spawn(command, log_name)

    @staticmethod
    def stop(process):
        # Persisting a long reversible fork on a shared laptop can exceed 15s.
        # Keep nonzero exits fatal; this changes only disposable test patience.
        return LargeMultiNodeTest.stop(process, shutdown_timeout=90)

    def observe(self):
        if time.monotonic() >= self.next_capacity_sample:
            self.resources('Periodic local capacity guard')
        result = super().observe()
        if self.phase_deadline is not None and time.monotonic() > self.phase_deadline:
            raise RuntimeError('Bounded real-clock local cohort phase exhausted its budget')
        return result

    def fault_test(self):
        # The inherited workflow actually stops/restarts one independently keyed
        # process and scans finalized timestamps, assets and historical rewards.
        super().fault_test()
        down = self.report['fault']['node']
        first = len(self.report['committee_events'])
        self.wait_observing('Two actual post-ban local single-seat replacements',
            lambda _i, _m, r: r['stage'] == 0 and
                len(full_seat_entries(self.report['committee_events'][first:])) >= 2, 1500)
        entries = full_seat_entries(self.report['committee_events'][first:])
        self.check(down not in entries and self.candidates()[down]['banned'] and
            self.producer(down)['unpaid_blocks'] == self.report['fault']['preserved_unpaid_blocks'],
            'Banned online/re-registered account excluded during two actual later seat replacements')
        self.report['fault']['actual_post_ban_entries'] = entries

    def additional_topology_fault(self):
        self.check(True, 'One physical host: no claim of independent host-failure testing')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--programs', type=Path, required=True)
    parser.add_argument('--contracts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    build = args.output/'build'; (build/'programs').mkdir(parents=True)
    alias = Path(tempfile.mkdtemp(prefix='pv1-cohort-'))/'b'; alias.symlink_to(build.resolve())
    for name in ('nodeos', 'cleos', 'keosd', 'helios-util'):
        target = args.programs.resolve()/(name+'.elf')
        if not target.is_file(): raise FileNotFoundError(target)
        directory = build/'programs'/name; directory.mkdir(); (directory/name).symlink_to(target)
    state = args.output/'state'; state.mkdir()
    def stop(_signum, _frame): raise KeyboardInterrupt('Stop only disposable local cohort processes')
    signal.signal(signal.SIGTERM, stop)
    test = LocalCohortTest(alias, args.contracts.resolve(), state)
    code = test.execute()
    report = dict(status='passed' if code == 0 and complete(test.report) else 'failed', release_ready=False,
        scope=__doc__, report=str((test.root/'report.json').resolve()),
        finished_utc=datetime.now(timezone.utc).isoformat())
    (args.output/'report.json').write_text(json.dumps(report, indent=2))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__': raise SystemExit(main())
