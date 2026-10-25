"""Deployment gates/configuration only. Never start a node, SSH or buy a VM."""
from copy import deepcopy
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from run_observer_admission_smoke import ObserverAdmissionSmoke
from pureeos_large_multinode_local_test import LargeMultiNodeTest
from v1_candidate_deployment_test import PROGRAMS, MIN_START_SECONDS, bounded_runtime, check_candidate


class DeploymentControls(unittest.TestCase):
    def test_original_deadline_always_keeps_twenty_minute_evidence_reserve(self):
        for remaining in (MIN_START_SECONDS,3600,5400,18000):
            runtime=bounded_runtime(remaining)
            self.assertGreaterEqual(runtime,1500)
            self.assertLessEqual(runtime,5400)
            self.assertGreaterEqual(remaining-runtime,1200)
        for remaining in (-1,0,1200,MIN_START_SECONDS-1):
            with self.assertRaises(AssertionError):bounded_runtime(remaining)

    def setUp(self):
        self.proof = dict(status='passed', candidate_test_status='passed')
        names = ['configure', 'build-node-controller'] + [f'{name}-round-{n}'
            for n in range(1,4) for name in
            ('access-hints','worker-signals','immutable-input','controller','worker','blacklist-and-history')]
        names += [f'leaf-overlap-stability-{n}' for n in range(1, 11)]
        names += [f'immutable-input-integration-probe-{n}' for n in range(1,4)]
        self.core = dict(status='passed', jobs=[dict(name=n,exit_code=0) for n in names],
            binaries_sha256={f'programs/{name}/{name}':'digest' for name in PROGRAMS})

    def test_complete_three_round_core_can_advance_to_artifact_verification(self):
        check_candidate(self.proof, self.core)

    def test_archived_failure_or_incomplete_build_is_not_a_deployment_pass(self):
        for key, value in (('status','running'),('candidate_test_status','failed')):
            proof={**self.proof,key:value}
            with self.assertRaises(AssertionError):check_candidate(proof,self.core)
        with self.assertRaises(AssertionError):check_candidate(self.proof,{**self.core,'status':'running'})

    def test_every_required_round_is_mandatory(self):
        for i in range(len(self.core['jobs'])):
            candidate=deepcopy(self.core);candidate['jobs'].pop(i)
            with self.assertRaises(AssertionError):check_candidate(self.proof,candidate)

    def test_failed_required_job_or_missing_program_cannot_advance(self):
        candidate=deepcopy(self.core);candidate['jobs'][3]['exit_code']=1
        with self.assertRaises(AssertionError):check_candidate(self.proof,candidate)
        for name in PROGRAMS:
            candidate=deepcopy(self.core);candidate['binaries_sha256'].pop(f'programs/{name}/{name}')
            with self.assertRaises(AssertionError):check_candidate(self.proof,candidate)

    def test_parallel_flags_apply_to_every_node_but_never_wallet(self):
        owner=ObserverAdmissionSmoke.__new__(ObserverAdmissionSmoke)
        owner.nodeos=Path('/test/programs/nodeos');owner.parallel_workers=4
        with tempfile.TemporaryDirectory() as temporary:
            owner.contracts=Path(temporary)
            for name in ('eosio.token','eosio.system'):
                directory=owner.contracts/name;directory.mkdir()
                (directory/(name+'.wasm')).write_bytes(b'test-only-wasm-config-fixture')
            with patch.object(LargeMultiNodeTest,'spawn') as spawn:
                owner.spawn([str(owner.nodeos),'--data-dir','/test/nodea'],'nodea.log')
                command=spawn.call_args.args[0]
                self.assertEqual(command[command.index('--experimental-parallel-validation-threads')+1],'4')
                self.assertEqual(command[command.index('--wasm-runtime')+1],'eos-vm-jit')
                self.assertIn('--experimental-parallel-production',command)
                self.assertEqual(command.count('--experimental-parallel-code-hash'),2)
                self.assertNotIn('--max-block-cpu-usage',command)
                owner.spawn(['/test/keosd'],'wallet.log')
                self.assertEqual(spawn.call_args.args[0],['/test/keosd'])

    def test_zero_workers_preserves_explicit_serial_configuration(self):
        owner=ObserverAdmissionSmoke.__new__(ObserverAdmissionSmoke)
        owner.nodeos=Path('/test/nodeos');owner.parallel_workers=0
        with patch.object(LargeMultiNodeTest,'spawn') as spawn:
            owner.spawn([str(owner.nodeos)],'serial.log')
            self.assertEqual(spawn.call_args.args[0],[str(owner.nodeos)])


if __name__=='__main__':unittest.main()
