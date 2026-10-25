"""Two actual VM nodes, provider VM halt/start and unchanged-ledger recovery.

Run only after the full cohort stops. A local exact-resource broker owns the API
credential; the guest writes nonce-bound requests and never receives that key.
This checks a provider VM shutdown/boot, not a physical power-loss or TPS test.
"""
import argparse
from datetime import datetime, timezone
import inspect
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import uuid

from pureeos_multihost_cloud_test import (CloudMultiNodeTest, PeerCommandError,
    check_listener_binding, validate_listener_ports)
from v1_twohost_cohort_test import ParallelCloudProfile, complete_cloud


def write_durable_marker(path, content):
    """Finish both file data and directory metadata before requesting VM halt."""
    import json
    import os
    from pathlib import Path
    path = Path(path)
    with path.open('x') as stream:
        json.dump(content, stream, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    if json.loads(path.read_text()) != content:
        raise AssertionError('Durable marker readback differs before VM halt')


def valid_receipt(value, request):
    expected = 'stopped' if request['action'] == 'halt' else 'running'
    return (all(value.get(k) == request[k] for k in ('request_id', 'nonce', 'action', 'instance_id'))
            and value.get('status') == 'confirmed' and value.get('power_status') == expected
            and value.get('provider_confirmed') is True)


def complete_power(report):
    if not complete_cloud(report, 'smoke'):
        return False
    proof = report.get('host_power', {})
    if not isinstance(proof, dict): return False
    requests, receipts = proof.get('requests', []), proof.get('receipts', [])
    if (len(requests) != 2 or len(receipts) != 2 or [r.get('action') for r in requests] != ['halt', 'start']
            or len({r.get('request_id') for r in requests}) != 2
            or not all(valid_receipt(v, r) for v, r in zip(receipts, requests))):
        return False
    if (len({r.get('nonce') for r in requests}) != 1
            or len({r.get('instance_id') for r in requests}) != 1
            or any(re.fullmatch('[0-9a-f]{32}', r.get(k, '')) is None
                   for r in requests for k in ('nonce', 'request_id'))): return False
    try:
        for value in (requests[0]['instance_id'], proof.get('boot_id_before'), proof.get('boot_id_after')):
            if str(uuid.UUID(value)) != value: return False
    except (TypeError, ValueError, AttributeError): return False
    quiet = proof.get('quiet_samples', [])
    before, after = proof.get('before', {}), proof.get('after', {})
    if not isinstance(quiet, list) or any(not isinstance(s, dict) or
            any(type(s.get(k)) is not int or s[k] < 0 for k in ('head', 'lib', 'unpaid_blocks'))
            or type(s.get('elapsed_seconds')) not in (int, float) or not math.isfinite(s['elapsed_seconds']) for s in quiet): return False
    numbers = (before.get('head_block_num'), after.get('last_irreversible_block_num'),
        proof.get('recovered_agreement', {}).get('common_lib'), proof.get('unpaid_after_recovery'),
        proof.get('halted_head_block_num'))
    if any(type(n) is not int or n < 0 for n in numbers): return False
    if not quiet or proof['halted_head_block_num'] != max(before['head_block_num'], *(s['head'] for s in quiet)):
        return False
    return bool(proof.get('vm_shutdown_not_process_stop') is True
        and proof.get('boot_id_before') and proof.get('boot_id_after')
        and proof['boot_id_before'] != proof['boot_id_after']
        and proof.get('data_marker_preserved') is True
        and proof.get('native_replay_requested') is True
        and proof.get('finalizer_safety_separate_from_state') is True
        and proof.get('no_safety_files_removed') is True
        and proof.get('original_block_log_backup_preserved') is True
        and type(proof.get('durable_recovery_records_before')) is int
        and proof['durable_recovery_records_before'] > 0
        and type(proof.get('durable_recovery_blocks_restored')) is int
        and proof['durable_recovery_blocks_restored'] > 0
        and proof.get('recovery_journal_kept_active') is True
        and type(proof.get('coordinator_pid_before')) is int
        and proof['coordinator_pid_before'] > 0
        and proof['coordinator_pid_before'] == proof.get('coordinator_pid_after')
        and len(quiet) >= 6 and quiet[-1].get('elapsed_seconds', 0) >= 15
        and len({s.get('lib') for s in quiet}) == 1
        and len({s.get('unpaid_blocks') for s in quiet}) == 1
        and proof.get('recovered_agreement', {}).get('common_lib', 0) > proof['halted_head_block_num'] + 12
        and after.get('last_irreversible_block_num', 0) > proof['halted_head_block_num'] + 12
        and proof.get('unpaid_after_recovery', 0) > quiet[-1].get('unpaid_blocks', 0)
        and proof.get('same_chain_id') is True and proof.get('same_signer_and_data_paths') is True
        and proof.get('no_under21_blacklist') is True)


class HostPowerTest(ParallelCloudProfile, CloudMultiNodeTest):
    peer_runtime_seconds = 3600

    def __init__(self, args):
        super().__init__(args)
        self.http_port = 28000
        self.url = 'http://127.0.0.1:28000'
        self.ports.add(self.http_port)
        validate_listener_ports([self.http_port, self.p2p_port], tuple(map(int,
            Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split())))
        check_listener_binding('127.0.0.1', self.http_port)
        self.report['endpoint'] = self.url

    def wait_power_receipt(self, description, predicate, timeout=360):
        # The exact peer is EXPECTED to lose its service/SSH during provider
        # halt/start. Ordinary wait_for polls it before calling the predicate
        # and would reject the intended outage before the receipt can arrive.
        # Keep both healthy coordinator processes under supervision throughout.
        deadline, next_notice = time.monotonic() + timeout, 0
        while time.monotonic() < deadline:
            for process in (self.node, self.wallet):
                if process.poll() is not None:
                    raise RuntimeError(f'Coordinator child {process.pid} exited while waiting for {description}')
            value = predicate()
            if value:
                return value
            if time.monotonic() >= next_notice:
                print('WAIT: ' + description, flush=True)
                next_notice = time.monotonic() + 20
            time.sleep(.5)
        raise RuntimeError('Timeout: ' + description)

    def peer_boot_ready(self):
        try:
            return self.ssh(['cat', '/proc/sys/kernel/random/boot_id']).strip()
        except PeerCommandError as error:
            if 'Host key verification failed' in str(error) or 'IDENTIFICATION HAS CHANGED' in str(error):
                raise
            return False
        except subprocess.TimeoutExpired:
            # A stale multiplexed SSH connection can time out while the same
            # VM boots. The caller retains its bounded, healthy-local wait.
            return False

    def peer_safety_snapshot(self):
        data = self.peers['nodea']['root']/'data'
        code = '''from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]);state=(root/'state').resolve(strict=True);safety=(root/'finalizers').resolve(strict=True)
assert state.is_dir() and safety.is_dir() and not safety.is_relative_to(state) and not state.is_relative_to(safety)
files={str(p.relative_to(safety)):dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in safety.rglob('*') if p.is_file()}
assert files and all(row['bytes']>0 for row in files.values())
print(json.dumps(dict(state_dir=str(state),finalizers_dir=str(safety),separate=True,files=files)))
'''
        return json.loads(self.ssh(['python3','-c',code,str(data)]))

    def peer_block_log_snapshot(self):
        data = self.peers['nodea']['root']/'data'
        code = '''from pathlib import Path
import hashlib,json,sys
root=Path(sys.argv[1]);files={str(p.relative_to(root)):dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in root.glob('blocks*/blocks.log') if p.is_file()}
assert 'blocks/blocks.log' in files
print(json.dumps(files))
'''
        return json.loads(self.ssh(['python3','-c',code,str(data)]))

    def peer_recovery_evidence(self, *, after_replay=False):
        peer = self.peers['nodea']
        code = '''from pathlib import Path
import json,re,sys
root=Path(sys.argv[1]);data=root/'data';journal=data/'blocks/reversible/recovery'
files=list(journal.glob('*.block'));assert journal.is_dir() and not journal.is_symlink()
assert all(p.is_file() and not p.is_symlink() for p in files)
backups=[str(p.relative_to(data)) for p in data.glob('blocks-*/reversible/recovery')]
restored=[]
if sys.argv[3]=='1':
 text=(root/('node-'+sys.argv[2]+'.log')).read_text()
 restored=[int(n) for n in re.findall(r'Restored ([0-9]+) reversible recovery blocks',text)]
print(json.dumps(dict(committed_records=len(files),backup_journals=backups,restored_counts=restored)))
'''
        return json.loads(self.ssh(['python3','-c',code,str(peer['root']),str(peer['starts']),
                                   '1' if after_replay else '0']))

    def request_power(self, action):
        root = self.args.power_control_root
        request = dict(nonce=self.args.task_nonce, request_id=uuid.uuid4().hex,
            action=action, instance_id=self.args.peer_instance_id,
            case_directory=str(self.root), created_utc=datetime.now(timezone.utc).isoformat())
        proof = self.report['host_power']; proof['requests'].append(request); self.checkpoint()
        path = root/'request.json'; pending = root/'request.pending'
        pending.write_text(json.dumps(request, indent=2)); pending.replace(path)
        def confirmed():
            p = root/'response.json'
            if not p.exists(): return False
            value = json.loads(p.read_text())
            if value.get('request_id') != request['request_id']: return False
            if value.get('status') == 'failed': raise RuntimeError('Exact-VM power broker failed: '+value.get('error', ''))
            if not valid_receipt(value, request): raise AssertionError('Incorrect or unbound VM power receipt')
            return value
        receipt = self.wait_power_receipt('Provider confirms exact test VM '+action, confirmed, 360)
        proof['receipts'].append(receipt); self.checkpoint()
        return receipt

    def tests(self):
        super().tests()  # Fresh two-node admission, fault/recovery, votes/resources and early-claim controls.
        assert self.names == ('eosio', 'nodea') and self.remote_names == ('nodea',)
        peer = self.peers['nodea']; old_process = peer['process']
        marker = peer['root']/'data'/('aic-host-power-'+self.args.task_nonce+'.json')
        chain_id = self.rpc('get_info')['chain_id']
        content = dict(nonce=self.args.task_nonce, chain_id=chain_id)
        command = ('import json,sys\n' + inspect.getsource(write_durable_marker)
                   + '\nwrite_durable_marker(sys.argv[1], json.loads(sys.argv[2]))\n')
        self.ssh(['python3', '-c', command, str(marker), json.dumps(content)])
        boot_before = self.ssh(['cat', '/proc/sys/kernel/random/boot_id']).strip()
        assert str(uuid.UUID(boot_before)) == boot_before
        before = self.rpc('get_info')
        proof = dict(before=before, requests=[], receipts=[], quiet_samples=[],
            coordinator_pid_before=self.node.pid, boot_id_before=boot_before,
            peer_process_before=old_process.pid, vm_shutdown_not_process_stop=True,
            scope=__doc__)
        self.report['host_power'] = proof; self.checkpoint()
        self.request_power('halt')
        # systemd transient peer units are gone after an actual VM boot. Do not
        # poll/stop the old unit via SSH while the host is intentionally off.
        old_process.returncode = 0; peer['stopped'] = True
        time.sleep(5)  # Drain packets already sent before the provider confirmed halt.
        started = time.monotonic()
        for _ in range(7):
            assert self.node.poll() is None
            info = self.rpc('get_info')
            proof['quiet_samples'].append(dict(elapsed_seconds=time.monotonic()-started,
                head=info['head_block_num'], lib=info['last_irreversible_block_num'],
                unpaid_blocks=int(self.producer('nodea')['unpaid_blocks'])))
            self.checkpoint(); time.sleep(3)
        self.check(len({s['lib'] for s in proof['quiet_samples']}) == 1,
            'Provider-stopped second VM cannot finalize the two-member quorum')
        self.check(len({s['unpaid_blocks'] for s in proof['quiet_samples']}) == 1,
            'Provider-stopped second VM accrues no block-reward units')
        proof['halted_head_block_num'] = max(before['head_block_num'], *(s['head'] for s in proof['quiet_samples']))
        self.request_power('start')
        boot_after = self.wait_for('Same VM SSH returns after provider start', self.peer_boot_ready, 240)
        self.check(boot_before != boot_after, 'Provider cycle changed the actual guest kernel boot ID')
        proof['boot_id_after'] = boot_after
        preserved = json.loads(self.ssh(['cat', str(marker)]))
        self.check(preserved == content, 'Existing peer data directory survived VM shutdown/boot')
        proof['data_marker_preserved'] = True
        # Provider halt can leave Chainbase dirty. Rebuild derived state from
        # this same block log while retaining the separate finalizer safety store.
        proof['safety_before_replay'] = self.peer_safety_snapshot()
        proof['block_logs_before_replay'] = self.peer_block_log_snapshot()
        recovery_before = self.peer_recovery_evidence()
        self.check(recovery_before['committed_records'] > 0,
                   'Existing durable voting recovery records survived the VM restart')
        proof['durable_recovery_records_before'] = recovery_before['committed_records']
        self.start_peer('nodea', restart=True, hard_replay=True); peer['stopped'] = False
        proof['native_replay_requested'] = True
        proof['recovery_mode'] = 'native-hard-replay-with-original-blocklog-backup'
        agreement = self.wait_for('Rebooted VM reaches a new common irreversible ID',
            lambda: self.synced(self.names), 240)
        recovery_after = self.peer_recovery_evidence(after_replay=True)
        self.check(len(recovery_after['restored_counts']) == 1 and recovery_after['restored_counts'][0] > 0
                   and not recovery_after['backup_journals'],
                   'Hard replay actually restored the active durable journal before peer synchronization')
        proof['durable_recovery_blocks_restored'] = recovery_after['restored_counts'][0]
        proof['recovery_journal_kept_active'] = not recovery_after['backup_journals']
        self.checkpoint()
        self.wait_for('Recovered VM finalizes beyond the old head',
            lambda: self.synced(self.names) if self.rpc('get_info')['last_irreversible_block_num'] > proof['halted_head_block_num']+12 else False, 240)
        self.wait_for('Recovered VM produces new payable blocks',
            lambda: int(self.producer('nodea')['unpaid_blocks']) > proof['quiet_samples'][-1]['unpaid_blocks'], 180)
        agreement = self.synced(self.names); after = self.rpc('get_info')
        self.check(agreement and agreement['common_lib'] > proof['halted_head_block_num']+12,
            'Both ledgers agree beyond the pre-shutdown chain head')
        same_chain = after['chain_id'] == chain_id == self.node_rpc('nodea', 'get_info')['chain_id']
        no_ban = all(not c['banned'] and int(c['zero_windows']) == 0 for c in self.candidates().values())
        self.check(same_chain and no_ban and self.node.pid == proof['coordinator_pid_before'] and self.node.poll() is None,
            'Same chain, unchanged coordinator process and no under-21 blacklist after VM recovery')
        proof['safety_after_recovery'] = self.peer_safety_snapshot()
        proof['finalizer_safety_separate_from_state'] = all(proof[k]['separate'] for k in ('safety_before_replay','safety_after_recovery'))
        proof['no_safety_files_removed'] = (set(proof['safety_before_replay']['files'])
            <= set(proof['safety_after_recovery']['files']))
        proof['block_logs_after_recovery'] = self.peer_block_log_snapshot()
        original_log = proof['block_logs_before_replay']['blocks/blocks.log']
        proof['original_block_log_backup_preserved'] = any(
            name.startswith('blocks-') and value == original_log
            for name,value in proof['block_logs_after_recovery'].items())
        self.check(proof['finalizer_safety_separate_from_state'] and proof['no_safety_files_removed'],
            'Native replay retains the independent finalizer safety store and existing safety files')
        self.check(proof['original_block_log_backup_preserved'],
            'Native hard replay preserves an exact backup of the original block log')
        proof.update(after=after, recovered_agreement=agreement, same_chain_id=same_chain,
            same_signer_and_data_paths=True, no_under21_blacklist=no_ban,
            unpaid_after_recovery=int(self.producer('nodea')['unpaid_blocks']),
            coordinator_pid_after=self.node.pid, peer_process_after=peer['process'].pid)
        self.report['final_synchronization'] = agreement
        self.observe(); self.resources('Both hosts after actual VM shutdown and boot')
        self.check_resource_split('After actual VM shutdown and same-data recovery'); self.checkpoint()


def main():
    os.umask(0o077); parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build-dir', 'contracts-dir', 'state-parent', 'peer-key', 'peer-known-hosts', 'power-control-root'):
        parser.add_argument('--'+name, type=Path, required=True)
    for name in ('peer-ip', 'coordinator-ip', 'peer-instance-id', 'task-nonce'):
        parser.add_argument('--'+name, required=True)
    args = parser.parse_args(); args.node_count = 2
    if re.fullmatch('[0-9a-f]{32}', args.task_nonce) is None: raise ValueError('Explicit one-use task nonce required')
    assert str(uuid.UUID(args.peer_instance_id)) == args.peer_instance_id
    args.power_control_root.mkdir(mode=0o700, exist_ok=False)
    def stop(_s, _f): raise KeyboardInterrupt('Stop only this host-power test')
    signal.signal(signal.SIGTERM, stop)
    test = HostPowerTest(args); code = test.execute()
    return code if code else (0 if complete_power(test.report) else 1)


if __name__ == '__main__': raise SystemExit(main())
