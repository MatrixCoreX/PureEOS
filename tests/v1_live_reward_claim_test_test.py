"""Offline real-clock gate controls; not a 24h execution result."""
import unittest
import inspect
from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import Mock

from v1_live_reward_claim_test import (FIXTURE_WITNESSES, LiveClaimTest, complete, due,
    fault_preflight_complete,partition_complete,signer_lines,signer_partition,isolated_fault_complete,
    ready_offline_identity,returning_identity_timeout)
from pureeos_large_multinode_local_test import LargeMultiNodeTest


def partition_proof(names):
    left,right,fault=signer_partition(names)
    return dict(primary=list(left),secondary=list(right),fault=list(fault),shared_keys=False,production_pause_guard_unchanged=True)


def isolated_proof():
    return dict(singleton_stopped=True,healthy_pids_unchanged=True,healthy_pids=[11,12],fault_pid=13)


class RealClaimControls(unittest.TestCase):
    def ready_state(self):
        names=LargeMultiNodeTest.names;active=list(names[:-1])
        return names,active,dict(stage=0,active=active,incoming='noden'),set(names)

    def test_active_historical_singleton_does_not_wait_for_another_full_reentry_cycle(self):
        self.assertEqual(ready_offline_identity(*self.ready_state()),'nodeq')

    def test_fault_requires_settled_membership_and_actual_historical_production(self):
        for mode in ('pending','never-produced','outside','duplicate'):
            names,members,rotation,observed=deepcopy(self.ready_state())
            if mode=='pending':rotation['stage']=1
            elif mode=='never-produced':observed.remove('nodeq')
            elif mode=='outside':members.remove('nodeq');members.append(names[-1]);rotation['active']=members
            else:rotation['active'][0]=rotation['active'][1]
            with self.subTest(mode=mode):self.assertFalse(ready_offline_identity(names,members,rotation,observed))

    def test_fault_keeps_enough_remaining_tenure_for_complete_missed_windows(self):
        for position in range(5):
            names,members,rotation,observed=self.ready_state();active=list(members);active.remove('nodeq');active.insert(position,'nodeq');rotation['active']=active
            with self.subTest(position=position):
                self.assertEqual(bool(ready_offline_identity(names,members,rotation,observed)),position>=4)

    def test_full_cohort_wait_budget_covers_a_complete_candidate_cycle(self):
        self.assertEqual(returning_identity_timeout(22),13200)
        self.assertGreater(returning_identity_timeout(22),22*3*21*12*.5)
        for count in (True,3,211):
            with self.subTest(count=count),self.assertRaises(ValueError):returning_identity_timeout(count)

    def test_two_disjoint_witnesses_cover_every_21_of_22_committee(self):
        names=LargeMultiNodeTest.names
        primary,secondary,fault=signer_partition(names)
        self.assertEqual((len(primary),len(secondary),len(fault)),(19,2,1))
        self.assertEqual(fault,('nodeq',))
        self.assertTrue(set(primary).isdisjoint(secondary))
        for absent in names:
            members=set(names)-{absent}
            self.assertTrue(members.intersection(secondary))
            # Removing only the fault process leaves a live external witness,
            # and at least 18 primary votes in every 21-of-22 roster.
            self.assertTrue((members-set(fault)).intersection(secondary))
            self.assertGreaterEqual(len(members.intersection(primary)),18)
    def test_witnesses_bootstrap_or_unknown_identity_cannot_be_fault_victim(self):
        for victim in ('eosio',*FIXTURE_WITNESSES,'unknown'):
            with self.assertRaises(AssertionError):signer_partition(LargeMultiNodeTest.names,victim)
        for names in (('eosio','nodea','nodec','noded'),('eosio','nodea','nodeb','nodeb')):
            with self.assertRaises(AssertionError):signer_partition(names)
    def test_primary_and_secondary_configure_different_producer_names_and_both_key_types(self):
        names=('eosio','nodea','nodeb','nodec');primary,secondary,fault=signer_partition(names,'nodec')
        accounts={n:{'Public key':'K1-'+n,'Private key':'synthetic-K1-'+n} for n in names}
        finalizers={n:{'Public key':'BLS-'+n,'Private key':'synthetic-BLS-'+n} for n in names}
        left,right,single=[signer_lines(group,accounts,finalizers) for group in (primary,secondary,fault)]
        self.assertEqual([s for s in left if s.startswith('producer-name')],['producer-name = eosio'])
        self.assertEqual([s for s in right if s.startswith('producer-name')],['producer-name = nodea','producer-name = nodeb'])
        self.assertFalse(set(left).intersection(right))
        self.assertFalse(any('nodec' in line for line in (*left,*right)))
        self.assertEqual([s for s in single if s.startswith('producer-name')],['producer-name = nodec'])
        self.assertFalse(set(single).intersection((*left,*right)))
        self.assertEqual(sum(s.startswith('signature-provider') for s in right),4)
        with self.assertRaises(AssertionError):signer_lines(('nodea','nodea'),accounts,finalizers)
    def test_topology_never_disables_or_extends_vote_pause_safety(self):
        for method in (LiveClaimTest.spawn,LiveClaimTest.configure_fixture_signers,LiveClaimTest.start_validator,LiveClaimTest.start_fault_signer):
            source=inspect.getsource(method)
            self.assertNotIn('--production-pause-vote-timeout-ms',source)
            self.assertNotIn('production-pause-vote-timeout-ms =',source)
        self.assertIn("signer_lines(secondary,self.accounts,self.bls)",inspect.getsource(LiveClaimTest.start_validator))
    def test_fault_stops_only_singleton_and_preserves_both_healthy_processes(self):
        primary=SimpleNamespace(pid=11,poll=Mock(return_value=None))
        witness=SimpleNamespace(pid=12,poll=Mock(return_value=None))
        fault=SimpleNamespace(pid=13,poll=Mock(return_value=None))
        def stop(child):
            self.assertIs(child,fault);fault.poll.return_value=0
        obj=SimpleNamespace(node=primary,validator=witness,fault_node=fault,
            stop=Mock(side_effect=stop),checkpoint=Mock(),report={},fault_expected_running=True)
        LiveClaimTest.stop_fault_signer(obj)
        obj.stop.assert_called_once_with(fault)
        self.assertFalse(obj.fault_expected_running)
        self.assertTrue(isolated_fault_complete(obj.report))
        self.assertEqual(obj.report['fault_isolation']['healthy_pids'],[11,12])
    def test_incomplete_or_shared_process_isolation_is_rejected(self):
        self.assertFalse(isolated_fault_complete({}))
        for key,value in [('singleton_stopped',False),('healthy_pids_unchanged',False),
                          ('healthy_pids',[11,11]),('fault_pid',11),('fault_pid',None)]:
            self.assertFalse(isolated_fault_complete({'fault_isolation':{**isolated_proof(),key:value}}))
    def test_fast_fault_preflight_is_not_a_24h_result(self):
        report=dict(status='passed',fault_preflight_only=True,real_time_24h=False,physical_hosts=1,
            processes_stopped=True,validator_stopped=True,fault_signer_stopped=True,
            fault_isolation=isolated_proof(),signer_partition=partition_proof(('eosio','nodea','nodeb','nodec')),
            fault_preflight=dict(omitted='nodec',agreement={'common_id':'a'},
                unpaid_before=10,unpaid_while_offline=10,unpaid_after_recovery=11,
                before={'head_block_num':100},after={'last_irreversible_block_num':149}))
        self.assertTrue(fault_preflight_complete(report));self.assertFalse(complete(report))
        for mode in ('shared','guard','no-recovery','paid-offline','no-finality','wrong-victim','not-stopped'):
            broken=deepcopy(report)
            if mode=='shared':broken['signer_partition']['shared_keys']=True
            if mode=='guard':broken['signer_partition']['production_pause_guard_unchanged']=False
            if mode=='no-recovery':broken['fault_preflight']['unpaid_after_recovery']=10
            if mode=='paid-offline':broken['fault_preflight']['unpaid_while_offline']=11
            if mode=='no-finality':broken['fault_preflight']['after']['last_irreversible_block_num']=148
            if mode=='wrong-victim':broken['fault_preflight']['omitted']='nodea'
            if mode=='not-stopped':broken['validator_stopped']=False
            self.assertFalse(fault_preflight_complete(broken))
    def test_both_chain_and_monotonic_must_exceed_day(self):
        start = '2026-10-22T01:00:00'
        self.assertFalse(due('2026-10-23T01:00:01', start, 100, 86500))
        self.assertFalse(due('2026-10-23T01:00:00', start, 100, 86501))
        self.assertFalse(due('2026-10-22T01:01:00', start, 100, 86501))
        self.assertTrue(due('2026-10-23T01:00:01', start, 100, 86501))

    def test_timezone_forms_compare_the_same_instant(self):
        self.assertTrue(due('2026-10-23T09:00:01+08:00', '2026-10-22T01:00:00Z', 0, 86401))

    def test_clock_jump_cannot_replace_real_elapsed_time(self):
        self.assertFalse(due('2036-10-23T01:00:00', '2026-10-22T01:00:00', 1000, 1010))

    def test_outer_pass_is_insufficient_without_both_real_payments(self):
        self.assertFalse(complete(dict(status='passed', processes_stopped=True, validator_stopped=True,
            real_time_24h=True, physical_hosts=1)))

    def test_finished_fixture_requires_all_evidence(self):
        r = dict(status='passed', processes_stopped=True, validator_stopped=True, real_time_24h=True,
            fault_signer_stopped=True,fault_isolation=isolated_proof(),
            signer_partition=partition_proof(LargeMultiNodeTest.names),
            physical_hosts=1, reward_claims=[dict(historical=h, transaction_id=str(i)*64, block_paid=1,
                agreement={'common_id': 'a'}) for i, h in enumerate((True, False))],
            latest_live_wait={'elapsed_seconds': {'a': 86401, 'b': 86402}})
        self.assertTrue(complete(r))
        r['latest_live_wait']['elapsed_seconds']['b'] = 86400; self.assertFalse(complete(r))
        r['latest_live_wait']['elapsed_seconds']['b'] = 86401
        r['reward_claims'][1]['block_paid'] = 0; self.assertFalse(complete(r))


if __name__ == '__main__': unittest.main()
