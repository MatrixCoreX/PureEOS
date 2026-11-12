"""Pure four-host topology/configuration tests; no VMs or sockets created."""
from pathlib import Path
import json
import hashlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from pureeos_large_multinode_local_test import LargeMultiNodeTest
from pureeos_multihost_cloud_test import PeerCommandError
from pureeos_fourhost_rotation_cloud_test import placement, safe_fault_group, peer_endpoint, HostProcess, FourHostRotationTest
from pureeos_rotation_cohort_cloud_test import cohort_names


class FourHostTests(unittest.TestCase):
    def test_forty_balances_ten_per_host_and_thirty_at_most_eight(self):
        names = cohort_names(40)
        layout = placement(names)
        self.assertEqual([sum(layout[n] == r for n in names) for r in 'abcd'], [10]*4)
        self.assertEqual([sum(layout[n] == r for n in names[:30]) for r in 'abcd'], [8,8,7,7])
        self.assertEqual(layout['eosio'], 'a')

    def test_invalid_placement_is_rejected(self):
        for names in (cohort_names(30), ('eosio',)*40, ('nodea',*cohort_names(40)[1:])):
            with self.assertRaises(ValueError): placement(names)

    def test_fault_group_keeps_native_quorum_and_never_stops_coordinator(self):
        names = cohort_names(40)
        layout = placement(names)
        size, role, affected = safe_fault_group(names[:21], layout)
        self.assertIn(role, 'bcd')
        self.assertEqual(size, len(affected))
        self.assertGreaterEqual(21-size, 15)

    def test_fault_is_not_allowed_if_no_peer_group_can_be_lost_safely(self):
        members = tuple('n'+str(i) for i in range(21))
        layout = {n:'bcd'[i//7] for i,n in enumerate(members)}
        self.assertIsNone(safe_fault_group(members, layout))
        self.assertIsNone(safe_fault_group(members[:20], layout))

    def test_exact_rpc_host_routing(self):
        for role in 'bcd':
            self.assertEqual(peer_endpoint(f'ssh+peer-{role}://127.0.0.1:29210'),
                             (role,'http://127.0.0.1:29210'))

    def test_rpc_cannot_escape_peer_loopback_allowlist(self):
        for url in ('ssh+peer-a://127.0.0.1:29201','ssh+peer-b://8.8.8.8:29201',
                    'ssh+peer-c://127.0.0.1:22','ssh+peer-d://127.0.0.1:29201/path',
                    'ssh+peer-b://root@127.0.0.1:29201','https://127.0.0.1:29201'):
            with self.subTest(url=url), self.assertRaises(ValueError): peer_endpoint(url)

    def test_rpc_is_routed_to_the_exact_host_namespace(self):
        owner=FourHostRotationTest.__new__(FourHostRotationTest)
        with patch.object(owner,'ssh_host',return_value='{"head_block_num": 12}') as ssh:
            self.assertEqual(owner.read_rpc('ssh+peer-c://127.0.0.1:29202','get_info')['head_block_num'],12)
        self.assertEqual(ssh.call_args.args[0],'c')
        self.assertEqual(ssh.call_args.args[1][-1],'http://127.0.0.1:29202/v1/chain/get_info')

    def test_only_connection_errors_are_retryable_not_http_or_identity_failures(self):
        owner=FourHostRotationTest.__new__(FourHostRotationTest)
        for code in (7,28,22,255):
            with self.subTest(code=code), patch.object(owner,'ssh_host',side_effect=PeerCommandError(code,'test')):
                with self.assertRaises(OSError if code in (7,28) else PeerCommandError):
                    owner.read_rpc('ssh+peer-d://127.0.0.1:29201','get_info')

    def test_pids_are_namespaced_across_hosts_even_if_kernel_pids_equal(self):
        owner = FourHostRotationTest.__new__(FourHostRotationTest)
        with patch.object(owner,'ssh_host',return_value='1234\n'):
            pids = [HostProcess(owner,r,'test-unit').pid for r in 'bcd']
        self.assertEqual(len(set(pids)), 3)

    def test_sparse_backbone_survives_loss_of_one_representative_host(self):
        owner = FourHostRotationTest.__new__(FourHostRotationTest)
        owner.hosts={r:{'main_ip':f'192.0.2.{i+1}'} for i,r in enumerate('abcd')}
        for role in 'abcd':
            peers=owner.backbone(role)
            self.assertEqual(len(peers),3)
            self.assertNotIn(owner.hosts[role]['main_ip']+':29111',peers)
        self.assertIn('192.0.2.1:29000',owner.backbone('d'))

    def test_private_node_command_explicitly_overrides_twenty_five_client_default(self):
        owner=LargeMultiNodeTest.__new__(LargeMultiNodeTest)
        owner.nodeos=Path('/test/nodeos')
        owner.build=Path('/test/build')
        owner.root=Path('/test/chain')
        owner.state_root=Path('/test/state')
        command=['/test/nodeos','--data-dir','/test/chain/data','--config-dir','/test/chain/config']
        with patch('pureeos_large_multinode_local_test.LocalTest.spawn',return_value='fake') as spawn:
            self.assertEqual(owner.spawn(command,'test.log'),'fake')
        launched=spawn.call_args.args[0]
        self.assertEqual(launched[launched.index('--max-clients')+1],'128')
        self.assertEqual(launched[launched.index('--p2p-max-nodes-per-host')+1],'64')

    def test_complete_idle_samples_on_all_four_hosts_do_not_abort_chain_test(self):
        owner=FourHostRotationTest.__new__(FourHostRotationTest)
        owner.report={'resource_samples':[]}
        sample={'version':1,'utc':'test','status':'complete','probes':{'oom':{'status':'empty','oom_detected':False}}}
        with patch('pureeos_fourhost_rotation_cloud_test.collect_host_metrics',return_value=sample), \
             patch.object(owner,'ssh_host',return_value=json.dumps(sample)), patch.object(owner,'checkpoint'):
            owner.resources('idle hosts')
        self.assertEqual(owner.report['resource_evidence_status'],'complete')
        self.assertEqual(set(owner.report['resource_samples'][0]['hosts']),set('abcd'))

    def test_missing_probe_evidence_is_recorded_separately_from_chain_failure(self):
        owner=FourHostRotationTest.__new__(FourHostRotationTest)
        owner.report={'resource_samples':[]}
        good={'version':1,'utc':'test','status':'complete','probes':{}}
        bad={'version':1,'utc':'test','status':'incomplete','probes':{'oom':{'status':'error','stderr':'Permission denied'}}}
        with patch('pureeos_fourhost_rotation_cloud_test.collect_host_metrics',return_value=bad), \
             patch.object(owner,'ssh_host',return_value=json.dumps(good)), patch.object(owner,'checkpoint'):
            owner.resources('partial evidence')
        self.assertEqual(owner.report['resource_evidence_status'],'incomplete')
        self.assertEqual(len(owner.report['resource_collection_errors']),1)
        self.assertEqual(owner.report['resource_collection_errors'][0]['role'],'a')

    def remote_start_fixture(self):
        owner=FourHostRotationTest.__new__(FourHostRotationTest)
        owner.names=cohort_names(40)
        owner.placement=placement(owner.names)
        owner.peer_root=Path('/peer/test')
        owner.root=Path('/coordinator/test')
        owner.peers={}
        owner.hosts={r:{'main_ip':f'192.0.2.{i+1}'} for i,r in enumerate('abcd')}
        owner.accounts={n:{'Public key':'fake-pub','Private key':'fake-private'}
                        for n in ('nodea','nodeb','nodec')}
        owner.bls=owner.accounts
        return owner

    def test_remote_launch_uses_remote_verified_genesis_not_coordinator_path(self):
        digest=hashlib.sha256(b'{}').hexdigest()
        for name, role in (('nodea','b'),('nodeb','c'),('nodec','d')):
            with self.subTest(role=role):
                owner=self.remote_start_fixture()
                with patch.object(Path,'read_text',return_value='{}'), \
                     patch.object(owner,'ssh_host',side_effect=[digest+'\n','']) as ssh, \
                     patch('pureeos_fourhost_rotation_cloud_test.HostProcess'):
                    owner.start_peer(name)
                launched=ssh.call_args_list[1].args[1]
                self.assertEqual(ssh.call_args_list[1].args[0],role)
                self.assertEqual(launched[launched.index('--genesis-json')+1],f'/peer/test/{name}/genesis.json')
                self.assertNotIn('/coordinator/test/genesis.json',launched)
                self.assertIn('--property=MemoryMax=900M',launched)
                self.assertIn('--property=MemorySwapMax=256M',launched)

    def test_actual_remote_copy_program_writes_matching_genesis_and_backbone(self):
        owner=self.remote_start_fixture()
        digest=hashlib.sha256(b'{}').hexdigest()
        with tempfile.TemporaryDirectory(prefix='pureeos-genesis-copy-') as temporary:
            owner.peer_root=Path(temporary)
            with patch.object(Path,'read_text',return_value='{}'), \
                 patch.object(owner,'ssh_host',side_effect=[digest+'\n','']) as ssh, \
                 patch('pureeos_fourhost_rotation_cloud_test.HostProcess'):
                owner.start_peer('nodea')
            call=ssh.call_args_list[0]
            program=call.args[1][-1]
            result=subprocess.run([sys.executable,'-c',program],input=call.kwargs['input_text'],
                                  capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),digest)
            self.assertEqual((owner.peer_root/'nodea/genesis.json').read_bytes(),b'{}')
            config=(owner.peer_root/'nodea/config/config.ini').read_text()
            self.assertIn('max-clients = 128\n',config)
            for endpoint in owner.backbone('b'):
                self.assertIn('p2p-peer-address = '+endpoint+'\n',config)

    def test_restart_never_reinitializes_genesis(self):
        owner=self.remote_start_fixture()
        owner.peers['nodea']={'root':Path('/peer/test/nodea'),'starts':1}
        with patch.object(owner,'ssh_host',return_value='') as ssh, \
             patch('pureeos_fourhost_rotation_cloud_test.HostProcess'):
            owner.start_peer('nodea',restart=True)
        self.assertNotIn('--genesis-json',ssh.call_args.args[1])
        self.assertEqual(owner.peers['nodea']['starts'],2)

    def test_incorrect_remote_genesis_digest_prevents_unit_creation(self):
        owner=self.remote_start_fixture()
        with patch.object(Path,'read_text',return_value='{}'), \
             patch.object(owner,'ssh_host',return_value='wrong') as ssh, \
             patch('pureeos_fourhost_rotation_cloud_test.HostProcess') as process:
            with self.assertRaises(RuntimeError):owner.start_peer('nodea')
        self.assertEqual(ssh.call_count,1)
        process.assert_not_called()


if __name__=='__main__':unittest.main()
