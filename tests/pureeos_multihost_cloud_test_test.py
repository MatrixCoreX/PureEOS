"""Control-plane unit tests: no cloud resources or subprocesses are created."""
import unittest
import socket
from pathlib import Path
from unittest.mock import Mock
from unittest.mock import patch
from pureeos_multihost_cloud_test import (RemoteProcess, CloudMultiNodeTest, PeerCommandError,
    balanced_candidates, validate_listener_ports, COORDINATOR_P2P_PORT, PEER_HTTP_BASE, PEER_P2P_BASE)
from pureeos_multihost_cloud_test import peer_service_limits,check_listener_binding


class FakeOwner:
    def __init__(self):
        self.calls = []
        self.state = 'ActiveState=active\nResult=success\n'

    def ssh(self, args, **kwargs):
        self.calls.append((args, kwargs))
        if 'MainPID' in args:
            return '1234\n'
        if args[1] == 'stop':
            self.state = 'ActiveState=inactive\nResult=success\n'
            return ''
        return self.state


class RemoteProcessTests(unittest.TestCase):
    def test_native_replay_reuses_peer_paths_without_genesis_or_destructive_flags(self):
        owner=CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        owner.remote_names=('nodea',);owner.root=Path('/private-fixture/case')
        data=Path('/private-peer/case/nodea')
        owner.peers={'nodea':dict(root=data,starts=1)};owner.parallel_workers=0
        with patch.object(owner,'ssh') as ssh,patch('pureeos_multihost_cloud_test.RemoteProcess',return_value=Mock(pid='peer:1234')):
            owner.start_peer('nodea',restart=True,replay=True)
        command=ssh.call_args.args[0]
        self.assertEqual(command[command.index('--data-dir')+1],str(data/'data'))
        self.assertEqual(command[command.index('--config-dir')+1],str(data/'config'))
        self.assertIn('--replay-blockchain',command)
        for wrong in ('--genesis-json','--delete-all-blocks','--hard-replay-blockchain'):
            self.assertNotIn(wrong,command)

    def test_replay_cannot_apply_to_fresh_or_nonremote_peers(self):
        owner=CloudMultiNodeTest.__new__(CloudMultiNodeTest);owner.remote_names=('nodea',)
        for name,restart,replay in [('nodea',False,True),('eosio',True,True),('nodea',True,1)]:
            with self.subTest(name=name,restart=restart,replay=replay),self.assertRaises(ValueError):
                owner.start_peer(name,restart=restart,replay=replay)
        with self.assertRaises(ValueError):owner.start_peer('nodea',restart=True,replay=True,hard_replay=True)

    def test_hard_replay_is_explicit_and_never_requests_arbitrary_block_truncation(self):
        owner=CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        owner.remote_names=('nodea',);owner.root=Path('/private-fixture/case')
        owner.peers={'nodea':dict(root=Path('/private-peer/case/nodea'),starts=1)};owner.parallel_workers=0
        with patch.object(owner,'ssh') as ssh,patch('pureeos_multihost_cloud_test.RemoteProcess',return_value=Mock(pid='peer:1234')):
            owner.start_peer('nodea',restart=True,hard_replay=True)
        command=ssh.call_args.args[0]
        self.assertIn('--hard-replay-blockchain',command)
        for wrong in ('--genesis-json','--delete-all-blocks','--replay-blockchain','--truncate-at-block'):
            self.assertNotIn(wrong,command)

    def test_port_preflight_rejects_an_actual_listener(self):
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            server.bind(('127.0.0.1',0));server.listen()
            with self.assertRaises(OSError):check_listener_binding('127.0.0.1',server.getsockname()[1])

    def test_port_preflight_accepts_own_closed_time_wait_connection(self):
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
            server.bind(('127.0.0.1',0));server.listen();port=server.getsockname()[1]
            with socket.create_connection(('127.0.0.1',port),timeout=2) as client:
                connection,_=server.accept()
                with connection:
                    connection.shutdown(socket.SHUT_WR)
                    self.assertEqual(client.recv(1),b'')
        check_listener_binding('127.0.0.1',port)

    def test_declared_twenty_eight_hour_cohort_is_accepted(self):
        limits=peer_service_limits(100800, 1200, 90)
        self.assertIn('--property=RuntimeMaxSec=100800',limits)
        self.assertIn('--property=MemoryMax=1200M',limits)
        self.assertIn('--property=TimeoutStopSec=90',limits)

    def test_peer_lifetime_memory_and_shutdown_limits_remain_bounded(self):
        for args in [(59,450,15),(129601,450,15),(60,128,15),(60,4097,15),(60,450,14),(60,450,91),(True,450,15)]:
            with self.subTest(args=args),self.assertRaises(ValueError):peer_service_limits(*args)

    def test_full_layout_is_unique_and_outside_linux_ephemeral_range(self):
        ports = [COORDINATOR_P2P_PORT] + [base + i for base in (PEER_HTTP_BASE, PEER_P2P_BASE) for i in range(11)]
        validate_listener_ports(ports, (32768, 60999))
        self.assertEqual(len(ports), 23)

    def test_old_rpc_ports_and_nonstandard_ephemeral_overlaps_are_rejected(self):
        for ports, ephemeral in [([39210], (32768, 60999)), ([29201], (29000, 30000))]:
            with self.subTest(ports=ports), self.assertRaises(ValueError):
                validate_listener_ports(ports, ephemeral)

    def test_duplicate_and_invalid_listener_configuration_is_rejected(self):
        for ports, ephemeral in [([29201, 29201], (32768, 60999)), ([22], (32768, 60999)),
                                  ([29201], (60999, 32768))]:
            with self.subTest(ports=ports), self.assertRaises(ValueError):
                validate_listener_ports(ports, ephemeral)

    def test_two_actual_nodes_are_one_per_vm(self):
        selected, remote = balanced_candidates(CloudMultiNodeTest.names, 2)
        self.assertEqual(selected, ('eosio', 'nodea'))
        self.assertEqual(remote, ('nodea',))

    def test_twenty_two_simulated_nodes_are_eleven_per_vm(self):
        selected, remote = balanced_candidates(CloudMultiNodeTest.names, 22)
        self.assertEqual(len(selected), 22)
        self.assertEqual(len(remote), 11)
        self.assertEqual(len(set(selected) - set(remote)), 11)
        self.assertNotIn('eosio', remote)

    def test_unapproved_phase_size_is_rejected(self):
        with self.assertRaises(ValueError): balanced_candidates(CloudMultiNodeTest.names, 210)

    def test_identity_is_namespaced_not_a_local_pid(self):
        process = RemoteProcess(FakeOwner(), 'test-unit')
        self.assertEqual(process.pid, 'peer:1234')

    def test_running_state_and_poll_cache(self):
        owner = FakeOwner()
        process = RemoteProcess(owner, 'test-unit')
        with patch('pureeos_multihost_cloud_test.time.monotonic', return_value=10):
            self.assertIsNone(process.poll())
            calls = len(owner.calls)
            self.assertIsNone(process.poll())
            self.assertEqual(len(owner.calls), calls)

    def test_clean_stop_is_zero_and_cached(self):
        owner = FakeOwner()
        process = RemoteProcess(owner, 'test-unit')
        process.terminate()
        self.assertEqual(process.wait(timeout=1), 0)
        self.assertEqual(process.returncode, 0)
        calls = len(owner.calls)
        self.assertEqual(process.poll(), 0)
        self.assertEqual(len(owner.calls), calls)

    def test_oom_never_counts_as_clean_exit(self):
        owner = FakeOwner()
        process = RemoteProcess(owner, 'test-unit')
        owner.state = 'ActiveState=failed\nResult=oom-kill\n'
        self.assertEqual(process.poll(), 1)

    def test_missing_unit_never_counts_as_success(self):
        owner = FakeOwner()
        process = RemoteProcess(owner, 'test-unit')
        owner.state = 'ActiveState=inactive\nResult=\n'
        self.assertEqual(process.poll(), 1)

    def test_nonrunning_creation_rejected(self):
        owner = FakeOwner()
        with patch.object(owner, 'ssh', return_value='0\n'):
            with self.assertRaises(RuntimeError):
                RemoteProcess(owner, 'test-unit')

    def test_rpc_startup_connection_errors_are_bounded_wait_inputs(self):
        owner = CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        for code in (7, 28):
            with self.subTest(code=code), patch.object(owner, 'ssh', side_effect=PeerCommandError(code, 'test')):
                with self.assertRaises(OSError):
                    owner.read_rpc('ssh+peer://127.0.0.1:39101', 'get_info')

    def test_http_and_ssh_identity_failures_are_not_transient_rpc(self):
        owner = CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        for code in (22, 255):
            with self.subTest(code=code), patch.object(owner, 'ssh', side_effect=PeerCommandError(code, 'test')):
                with self.assertRaises(PeerCommandError):
                    owner.read_rpc('ssh+peer://127.0.0.1:39101', 'get_info')

    def test_successful_peer_json_is_returned(self):
        owner = CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        with patch.object(owner, 'ssh', return_value='{"head_block_num":12}'):
            self.assertEqual(owner.read_rpc('ssh+peer://127.0.0.1:39101', 'get_info'), {'head_block_num': 12})

    def test_first_lib_also_requires_real_peer_to_coordinator_tcp(self):
        owner = CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        owner._network_checked = False
        owner.coordinator_ip, owner.report = '203.0.113.1', {}
        owner.p2p_port = COORDINATOR_P2P_PORT
        with patch('pureeos_multihost_cloud_test.LargeMultiNodeTest.wait_lib', return_value={'lib': 17}), \
             patch.object(owner, 'ssh', return_value='ok\n') as probe:
            self.assertEqual(owner.wait_lib(17), {'lib': 17})
            self.assertTrue(owner._network_checked)
            self.assertEqual(owner.report['peer_to_coordinator_p2p_tcp_preflight'], 'passed')
            owner.wait_lib(18)
            self.assertEqual(probe.call_count, 1)

    def test_failed_tcp_preflight_cannot_pass_initial_lib_gate(self):
        owner = CloudMultiNodeTest.__new__(CloudMultiNodeTest)
        owner._network_checked = False
        owner.coordinator_ip, owner.report = '203.0.113.1', {}
        owner.p2p_port = COORDINATOR_P2P_PORT
        with patch('pureeos_multihost_cloud_test.LargeMultiNodeTest.wait_lib', return_value={'lib': 17}), \
             patch.object(owner, 'ssh', return_value=''):
            with self.assertRaises(RuntimeError): owner.wait_lib(17)
            self.assertFalse(owner._network_checked)


if __name__ == '__main__':
    unittest.main()
