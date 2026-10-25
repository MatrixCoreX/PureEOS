#!/usr/bin/env python3
"""Real-clock 30 -> 40 candidate rotation on two disposable, matching hosts.

Every account has its own K1/BLS keys, nodeos process and ledger. Only 21 seats
are active. This is a queue/finality/failure test, not a TPS benchmark or proof
of 40 independent physical failure domains. No setprods or clock acceleration.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import time

from pureeos_multihost_cloud_test import CloudMultiNodeTest, validate_listener_ports


def cohort_names(count):
    if count not in (2, 22, 30, 40):
        raise ValueError('Unsupported disposable test cohort')
    # Keep established nodea..nodez names, then use valid EOS account letters.
    ordinary = ['node' + chr(97 + i) for i in range(26)]
    ordinary += ['nodea' + chr(97 + i) for i in range(26)]
    return ('eosio', *ordinary[:count - 1])


def check_exact_ranking(rows, total, expected):
    owners = [row['owner'] for row in rows]
    actual = {row['owner']: int(row['total_votes']) for row in rows}
    if len(owners) != len(set(owners)) or actual != expected or int(total) != sum(expected.values()):
        raise AssertionError('Exact integer vote amounts/total/unique accounts disagree')
    if owners != sorted(expected, key=lambda name: (-expected[name], name)):
        raise AssertionError('Vote ranking or deterministic equal-vote tie order disagrees')


def full_seat_entries(events):
    entries = []
    for event in events:
        rotation = event['rotation']
        if len(event['schedule']['producers']) == 21 and rotation.get('outgoing'):
            entries.append(rotation['incoming'])
    return entries


class CohortCloudTest(CloudMultiNodeTest):
    names = cohort_names(40)
    # Disposable-suite lifetime only. This never changes protocol slot lengths.
    peer_runtime_seconds = 100800
    wallet_unlock_seconds = 103680

    def __init__(self, args):
        args.node_count = 40
        super().__init__(args)
        self.configure_cohort_state()

    def configure_cohort_state(self):
        # Local ephemeral bind(0) discovery is not a reservation. Use disjoint
        # low listener ranges on BOTH hosts, including coordinator loopback RPC.
        self.http_port = 28000
        self.url = 'http://127.0.0.1:28000'
        self.ports.add(self.http_port)
        self.report['endpoint'] = self.url
        validate_listener_ports([self.http_port, *range(28001, 28100)], tuple(map(int,
            Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', self.http_port))
        self.registered = ['eosio']
        self.expected_units = {'eosio': 1000000}
        self.phase_deadline = None
        self.report.update(scope=__doc__, cohort_phases=[], recovered_fault={},
            cohort_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            requested_candidate_phases=[30, 40], capacity_boundary_210_not_tested=True,
            actual_24_hour_payout_not_tested=True, tps_not_tested=True)

    def port(self):
        for port in range(28001, 28100):
            if port in self.ports:
                continue
            with socket.socket() as probe:
                try:
                    probe.bind(('127.0.0.1', port))
                except OSError:
                    continue
            self.ports.add(port)
            return port
        raise RuntimeError('No unused non-ephemeral task listener remains')

    def observe(self):
        state = super().observe()
        if self.phase_deadline is not None and time.monotonic() > self.phase_deadline:
            raise RuntimeError('Bounded real-clock cohort phase exhausted its budget')
        return state

    def exact_ranking(self, label):
        ranked = self.ranked_all()
        check_exact_ranking(ranked['rows'], ranked['total_producer_vote_amount'], self.expected_units)
        self.report['rankings'].append(dict(label=label, expected_units=dict(self.expected_units), rpc=ranked))
        self.check(True, label + ': exact minimum-unit votes, total and deterministic rank')

    def add_candidates(self, count):
        for index in range(len(self.registered), count):
            name = self.names[index]
            self.start_peer(name)
            self.wait_candidate_sync(name)
            self.register(name)
            self.stake(name, 100 + index, 0)
            self.tx(['system', 'voteproducer', 'approve', name, name])
            self.expected_units[name] = (100 + index) * 10000
            self.registered.append(name)
            self.observe()
            self.checkpoint()  # Persist actual admission, not only producer-policy changes.
        pids = self.process_evidence()
        self.check(len(pids) == count and len({p['pid'] for p in pids.values()}) == count,
                   f'{count} genuinely independent nodeos processes joined the same chain')
        self.exact_ranking(f'{count} registered candidates')
        self.resources(f'{count} processes, before complete queue coverage')

    def wait_candidate_sync(self, name):
        """Bound late joins by chain length, while detecting genuine stalls.

        The final gate is still synced(): same chain, close LIVE LIBs and the
        same independently fetched irreversible block ID. Progress is never a
        substitute for final agreement, nor permission to register a candidate.
        """
        leader = self.node_rpc('eosio', 'get_info')
        if leader['chain_id'] != self.report['chain_id']:
            raise AssertionError('Coordinator chain ID mismatch before peer sync')
        budget = min(1800, max(180, int(leader['head_block_num']) // 25 + 180))
        started = time.monotonic()
        evidence = dict(node=name, status='running', timeout_seconds=budget,
            stalled_seconds=180, target_lib_at_start=leader['last_irreversible_block_num'],
            target_head_at_start=leader['head_block_num'], observations=[])
        self.report.setdefault('admission_sync', []).append(evidence)
        high_peer = -1
        high_leader = int(leader['head_block_num'])
        last_peer = last_leader = started
        next_notice = started

        def ready():
            nonlocal high_peer, high_leader, last_peer, last_leader, next_notice
            now = time.monotonic()
            current = self.node_rpc('eosio', 'get_info')
            peer = self.node_rpc(name, 'get_info')
            if any(row['chain_id'] != self.report['chain_id'] for row in (current, peer)):
                raise AssertionError('Peer chain ID mismatch during catch-up')
            if int(peer['head_block_num']) > high_peer:
                high_peer = int(peer['head_block_num']); last_peer = now
            if int(current['head_block_num']) > high_leader:
                high_leader = int(current['head_block_num']); last_leader = now
            if now >= next_notice:
                observation = dict(elapsed_seconds=round(now-started, 3),
                    peer_head=peer['head_block_num'], peer_lib=peer['last_irreversible_block_num'],
                    leader_head=current['head_block_num'], leader_lib=current['last_irreversible_block_num'])
                evidence['observations'].append(observation)
                print('SYNC: ' + json.dumps(dict(node=name, **observation)), flush=True)
                self.checkpoint(); next_notice = now + 20
            if abs(int(current['last_irreversible_block_num']) - int(peer['last_irreversible_block_num'])) <= 4:
                agreement = self.synced(['eosio', name])
                if agreement:
                    return agreement
            if now-last_peer >= 180:
                raise RuntimeError(name + ' P2P catch-up stopped advancing for 180 seconds')
            if now-last_leader >= 180:
                raise RuntimeError('Coordinator stopped advancing during peer catch-up')
            return False

        try:
            result = self.wait_for(name + ' original-chain P2P sync', ready, budget)
            evidence.update(status='passed', agreement=result)
            return result
        except (Exception, KeyboardInterrupt) as error:
            evidence.update(status='failed', error=self.redact(str(error)))
            raise
        finally:
            evidence['elapsed_seconds'] = round(time.monotonic()-started, 3)
            self.checkpoint()

    def finalize_phase(self, count, start_block, first_event):
        names = set(self.registered)

        def covered(_info, members, rotation):
            entries = full_seat_entries(self.report['committee_events'][first_event:])
            coverage=dict(candidate_count=count,unique_reentered=len(names & set(entries)),
                total_replacements=len(entries),missing=sorted(names-set(entries)))
            if self.report.get('queue_coverage') != coverage:
                self.report['queue_coverage']=coverage
                self.checkpoint()
            if len(entries) > count * 3 and not names <= set(entries):
                raise AssertionError('Candidate starvation: three cohort lengths without complete queue coverage')
            return len(members) == 21 and rotation['stage'] == 0 and names <= set(entries)

        self.wait_observing(f'{count}: every candidate re-enters the full committee, complete FIFO coverage',
                            covered, max(18000, count * 600))
        self.wait_lib(self.rpc('get_info')['head_block_num'] + 12, 180)
        end = self.rpc('get_info')['last_irreversible_block_num']
        blocks = self.scan_finalized(start_block, end, f'{count}: independently scanned irreversible production')
        self.check(names <= set(blocks['counts']), f'All {count} candidates actually produced irreversible blocks')
        self.check(all(not c['banned'] and int(c['zero_windows']) == 0 for c in self.candidates().values()),
                   f'{count}: no false blacklist or zero-output strike among healthy candidates')
        agreement = self.wait_for(f'{count} independent ledgers agree on a common irreversible ID',
                                  lambda: self.synced(self.registered), 240)
        entries = full_seat_entries(self.report['committee_events'][first_event:])
        self.report['cohort_phases'].append(dict(candidate_count=count, status='passed',
            queue_entries=entries, entry_counts=dict(Counter(entries)),
            finalized_production=blocks, agreement=agreement,
            completed_utc=datetime.now(timezone.utc).isoformat()))
        self.checkpoint()

    def recover_before_ban(self):
        def incoming(_i, _m, rotation):
            n = rotation['incoming']
            return n if rotation['stage'] == 1 and n != 'eosio' else False
        victim = self.wait_observing('ordinary returning entrant for recoverable fault', incoming, 900)
        self.report['recovered_fault'] = dict(node=victim)
        # Disable the healthy FIFO-only assertion during explicitly injected faults.
        self.report['fault'] = dict(recoverable_only=True, node=victim)
        self.stop(self.peers[victim]['process'])
        self.peers[victim]['stopped'] = True
        missing = self.wait_observing('one complete finalized zero-output personal window',
            lambda _i, _m, _r: self.candidates()[victim]
                if int(self.candidates()[victim]['zero_windows']) >= 1 else False, 600)
        self.check(not missing['banned'] and int(missing['zero_windows']) < 3,
                   'One complete missed personal window is not a permanent ban')
        self.report['recovered_fault']['missing'] = missing
        self.start_peer(victim, restart=True)
        self.peers[victim]['stopped'] = False
        self.wait_observing('recovered entrant produces and clears consecutive misses',
            lambda _i, _m, r: r['stage'] == 0 and int(self.candidates()[victim]['zero_windows']) == 0, 1200)
        self.check(not self.candidates()[victim]['banned'], 'Recovery before three misses preserves rotation eligibility')
        self.report['recovered_fault']['recovered'] = self.candidates()[victim]
        self.report['fault'] = {}
        self.checkpoint()

    def fault_test(self):
        # The original 22-node test also does the independent finalized-window
        # scan, balance/ledger preservation and real restart/unregister/register.
        super().fault_test()
        down = self.report['fault']['node']
        first_event = len(self.report['committee_events'])
        self.wait_observing('two actual post-ban single-seat replacements',
            lambda _i, _m, r: r['stage'] == 0 and
                len(full_seat_entries(self.report['committee_events'][first_event:])) >= 2, 1500)
        entries = full_seat_entries(self.report['committee_events'][first_event:])
        self.check(down not in entries and self.candidates()[down]['banned'] and
                   self.producer(down)['unpaid_blocks'] == self.report['fault']['preserved_unpaid_blocks'],
                   'Banned online/re-registered account excluded during two actual later seat replacements')
        self.report['fault']['actual_post_ban_entries'] = entries

    def tests(self):
        # Unlike legacy multi-signing fixtures, bootstrap signs only for eosio.
        self.tx(['system', 'voteproducer', 'approve', 'alice', 'eosio'])
        self.action('deposit', {'owner': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.action('buyrex', {'from': 'alice', 'amount': '100.0000 SYS'}, 'alice')
        self.observe()
        for count in (30, 40):
            self.phase_deadline = time.monotonic() + (21600 if count == 30 else 39600)
            self.report['current_phase']=dict(candidate_count=count,started_utc=datetime.now(timezone.utc).isoformat())
            first = self.rpc('get_info')['last_irreversible_block_num'] + 1
            event = len(self.report['committee_events'])
            self.add_candidates(count)
            if count == 40:
                # Minimum-unit update, plus equal-vote deterministic tie sorting.
                n = self.names[-1]
                self.action('delegatebw', {'from': n, 'receiver': n,
                    'stake_net_quantity': '0.0001 SYS', 'stake_cpu_quantity': '0.0000 SYS', 'transfer': False}, n)
                self.expected_units[n] += 1
                for earlier in self.names[-3:-1]:
                    delta = self.expected_units[n] - self.expected_units[earlier]
                    quantity = f'{delta // 10000}.{delta % 10000:04d} SYS'
                    self.action('delegatebw', {'from': earlier, 'receiver': earlier,
                        'stake_net_quantity': quantity, 'stake_cpu_quantity': '0.0000 SYS', 'transfer': False}, earlier)
                    self.expected_units[earlier] += delta
                self.exact_ranking('40 candidates after minimum-unit update and three-way tie')
            self.finalize_phase(count, first, event)
        self.phase_deadline = None
        self.check(set(len(e['schedule']['producers']) for e in self.report['committee_events']) == set(range(1, 22)),
                   'Observed every actual committee size 1..21, never more than one seat changed')
        self.recover_before_ban()
        self.fault_test()
        self.report['final_synchronization'] = self.wait_for('All 40 agree after actual failure/restart',
            lambda: self.synced(self.names), 240)
        self.check_resource_split('After 30 -> 40 candidate rotation and faults')
        self.resources('Final 40-process state, one permanently banned account')
        self.additional_topology_fault()

    def additional_topology_fault(self):
        """Optional topology-specific failure, without weakening the common suite."""


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'contracts-dir', 'state-parent', 'peer-key', 'peer-known-hosts'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--peer-ip', required=True)
    parser.add_argument('--coordinator-ip', required=True)
    args = parser.parse_args()
    def stop(_signum, _frame):
        raise KeyboardInterrupt('Stopping only disposable cohort test processes')
    signal.signal(signal.SIGTERM, stop)
    raise SystemExit(CohortCloudTest(args).execute())


if __name__ == '__main__':
    main()
