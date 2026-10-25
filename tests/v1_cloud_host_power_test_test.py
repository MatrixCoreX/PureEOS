"""Offline proof validation only; these do not claim a provider VM was stopped."""
from copy import deepcopy
import json
import inspect
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import v1_cloud_host_power_test as power


class DurableMarkerControls(unittest.TestCase):
    def test_real_file_and_parent_directory_are_synced_and_existing_evidence_is_immutable(self):
        with tempfile.TemporaryDirectory(prefix='aic-marker-') as temporary:
            path = Path(temporary)/'marker with spaces.json'
            content = dict(chain_id='a'*64, nonce='b'*32)
            calls = []
            original = os.fsync
            def sync(fd):
                mode = os.fstat(fd).st_mode
                calls.append('file' if stat.S_ISREG(mode) else 'directory' if stat.S_ISDIR(mode) else 'unexpected')
                return original(fd)
            with patch.object(os, 'fsync', side_effect=sync):
                power.write_durable_marker(path, content)
            self.assertEqual(calls, ['file', 'directory'])
            self.assertEqual(json.loads(path.read_text()), content)
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                power.write_durable_marker(path, {'changed': True})
            self.assertEqual(path.read_bytes(), before)

    def test_actual_remote_script_is_self_contained_and_sync_failure_is_not_ignored(self):
        with tempfile.TemporaryDirectory(prefix='aic-marker-script-') as temporary:
            path = Path(temporary)/'remote marker.json'
            content = {'chain_id': 'a'*64, 'test': '中文'}
            script = ('import json,sys\n' + inspect.getsource(power.write_durable_marker)
                      + '\nwrite_durable_marker(sys.argv[1], json.loads(sys.argv[2]))\n')
            result = subprocess.run([sys.executable, '-I', '-B', '-c', script, str(path), json.dumps(content)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(path.read_text()), content)
            with patch.object(os, 'fsync', side_effect=OSError('simulated durable write failure')):
                with self.assertRaisesRegex(OSError, 'durable write failure'):
                    power.write_durable_marker(Path(temporary)/'failing.json', content)


def fixture():
    requests = [dict(request_id=f'{i+1:032x}', nonce='a'*32, action=a,
                    instance_id='00000000-0000-0000-0000-000000000002')
                for i, a in enumerate(('halt', 'start'))]
    receipts = [{**r, 'status': 'confirmed', 'provider_confirmed': True,
                 'power_status': 'stopped' if r['action'] == 'halt' else 'running'} for r in requests]
    return dict(host_power=dict(requests=requests, receipts=receipts,
        vm_shutdown_not_process_stop=True,
        boot_id_before='10000000-0000-0000-0000-000000000001',
        boot_id_after='10000000-0000-0000-0000-000000000002',
        data_marker_preserved=True, coordinator_pid_before=101, coordinator_pid_after=101,
        native_replay_requested=True, finalizer_safety_separate_from_state=True, no_safety_files_removed=True,
        original_block_log_backup_preserved=True,
        durable_recovery_records_before=57, durable_recovery_blocks_restored=53,
        recovery_journal_kept_active=True,
        quiet_samples=[dict(elapsed_seconds=i*3, head=120, lib=100, unpaid_blocks=12) for i in range(7)],
        halted_head_block_num=120,
        before=dict(head_block_num=120), after=dict(last_irreversible_block_num=140),
        recovered_agreement=dict(common_lib=140), unpaid_after_recovery=14,
        same_chain_id=True, same_signer_and_data_paths=True, no_under21_blacklist=True))


class PowerProofControls(unittest.TestCase):
    def accepted(self, report):
        with patch.object(power, 'complete_cloud', return_value=True):
            return power.complete_power(report)

    def test_complete_extension_proof_requires_original_cloud_acceptance(self):
        self.assertTrue(self.accepted(fixture()))
        with patch.object(power, 'complete_cloud', return_value=False):
            self.assertFalse(power.complete_power(fixture()))

    def test_peer_catchup_cannot_replace_actual_journal_recovery(self):
        for key in ('durable_recovery_records_before', 'durable_recovery_blocks_restored'):
            for value in (None, False, True, 0, -1, '57', 57.0):
                report = fixture(); report['host_power'][key] = value
                self.assertFalse(self.accepted(report), (key, value))
        report = fixture(); report['host_power']['recovery_journal_kept_active'] = False
        self.assertFalse(self.accepted(report))

    def test_pre_shutdown_inflight_progress_cannot_be_claimed_as_post_boot_finality(self):
        report = fixture()
        for sample in report['host_power']['quiet_samples']:
            sample['head'] = 139
        report['host_power']['halted_head_block_num'] = 139
        self.assertFalse(self.accepted(report))
        report['host_power']['halted_head_block_num'] = 120
        self.assertFalse(self.accepted(report))

    def test_missing_changed_boot_or_preserved_data_refused(self):
        for key, value in (('boot_id_after', fixture()['host_power']['boot_id_before']), ('boot_id_after', ''),
                           ('data_marker_preserved', False), ('vm_shutdown_not_process_stop', False),
                           ('coordinator_pid_after', 102), ('same_chain_id', False),
                           ('same_signer_and_data_paths', False), ('no_under21_blacklist', False)):
            r=fixture();r['host_power'][key]=value
            with self.subTest(key=key): self.assertFalse(self.accepted(r))

    def test_replay_and_preserved_finalizer_safety_are_required(self):
        for key in ('native_replay_requested','finalizer_safety_separate_from_state','no_safety_files_removed',
                    'original_block_log_backup_preserved'):
            r=fixture();r['host_power'][key]=False
            with self.subTest(key=key):self.assertFalse(self.accepted(r))

    def test_quiet_interval_must_be_long_enough_without_finality_or_reward_growth(self):
        for kind in ('short', 'few', 'lib', 'reward'):
            r=fixture();quiet=r['host_power']['quiet_samples']
            if kind=='short': quiet[-1]['elapsed_seconds']=14
            elif kind=='few': r['host_power']['quiet_samples']=quiet[:5]
            elif kind=='lib': quiet[-1]['lib']+=1
            else: quiet[-1]['unpaid_blocks']+=1
            with self.subTest(kind=kind): self.assertFalse(self.accepted(r))

    def test_recovery_needs_new_common_finality_and_actual_production(self):
        for kind in ('agreement', 'lib', 'pay'):
            r=fixture();p=r['host_power']
            if kind=='agreement': p['recovered_agreement']['common_lib']=132
            elif kind=='lib': p['after']['last_irreversible_block_num']=132
            else: p['unpaid_after_recovery']=12
            with self.subTest(kind=kind): self.assertFalse(self.accepted(r))

    def test_receipts_are_bound_to_request_target_nonce_action_and_result(self):
        for key, value in (('request_id', 'other'), ('nonce', 'other'), ('instance_id', 'other'),
                           ('action', 'other'), ('power_status', 'running'),
                           ('status', 'pending'), ('provider_confirmed', False)):
            r=fixture();r['host_power']['receipts'][0][key]=value
            with self.subTest(key=key): self.assertFalse(self.accepted(r))

    def test_missing_repeated_and_reversed_actions_refused(self):
        for kind in ('missing', 'same-id', 'reverse'):
            r=fixture();p=r['host_power']
            if kind=='missing': p['receipts'].pop()
            elif kind=='same-id': p['requests'][1]['request_id']=p['requests'][0]['request_id']
            else: p['requests'].reverse();p['receipts'].reverse()
            with self.subTest(kind=kind): self.assertFalse(self.accepted(r))

    def test_malformed_time_and_ledger_evidence_cannot_pass(self):
        for field, value in (('elapsed_seconds', float('nan')), ('elapsed_seconds', '18'),
                             ('lib', None), ('lib', True), ('unpaid_blocks', -1)):
            r=fixture();r['host_power']['quiet_samples'][-1][field]=value
            with self.subTest(field=field,value=value): self.assertFalse(self.accepted(r))


class ProviderTransitionControls(unittest.TestCase):
    def bare_test(self, root):
        test = object.__new__(power.HostPowerTest)
        test.args = SimpleNamespace(power_control_root=root, task_nonce='a'*32,
            peer_instance_id='00000000-0000-0000-0000-000000000002')
        test.root = root
        test.report = {'host_power': {'requests': [], 'receipts': []}}
        test.checkpoint = Mock()
        test.node, test.wallet = Mock(pid=101), Mock(pid=102)
        test.node.poll.return_value = test.wallet.poll.return_value = None
        peer = Mock(pid='peer:201')
        peer.poll.side_effect = power.PeerCommandError(255, 'expected VM shutdown')
        test.peers = {'nodea': {'process': peer}}
        return test, peer

    def test_halt_receipt_can_arrive_after_peer_is_expected_offline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); test, peer = self.bare_test(root)
            def confirm(_seconds):
                request = json.loads((root/'request.json').read_text())
                (root/'response.json').write_text(json.dumps({**request, 'status': 'confirmed',
                    'power_status': 'stopped', 'provider_confirmed': True}))
            with patch.object(power.time, 'sleep', side_effect=confirm):
                receipt = test.request_power('halt')
            self.assertTrue(power.valid_receipt(receipt, test.report['host_power']['requests'][0]))
            peer.poll.assert_not_called()
            self.assertGreaterEqual(test.node.poll.call_count, 2)
            self.assertGreaterEqual(test.wallet.poll.call_count, 2)

    def test_expected_peer_outage_does_not_hide_local_node_or_wallet_exit(self):
        for child in ('node', 'wallet'):
            with self.subTest(child=child), tempfile.TemporaryDirectory() as temporary:
                test, peer = self.bare_test(Path(temporary))
                getattr(test, child).poll.return_value = 1
                predicate = Mock(return_value=True)
                with self.assertRaisesRegex(RuntimeError, 'Coordinator child'):
                    test.wait_power_receipt('test receipt', predicate)
                predicate.assert_not_called(); peer.poll.assert_not_called()

    def test_reboot_ssh_timeout_retries_without_accepting_host_key_change(self):
        test = object.__new__(power.HostPowerTest)
        for error in (subprocess.TimeoutExpired('ssh', 20), power.PeerCommandError(255, 'Connection refused')):
            test.ssh = Mock(side_effect=error)
            self.assertFalse(test.peer_boot_ready())
        for message in ('Host key verification failed', 'REMOTE HOST IDENTIFICATION HAS CHANGED'):
            test.ssh = Mock(side_effect=power.PeerCommandError(255, message))
            with self.assertRaises(power.PeerCommandError): test.peer_boot_ready()


if __name__ == '__main__': unittest.main()
