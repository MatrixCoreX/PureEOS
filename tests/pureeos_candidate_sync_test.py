"""Pure catch-up gate checks; no sockets, accounts or cloud machines created."""
import unittest
from unittest.mock import patch
from pureeos_rotation_cohort_cloud_test import CohortCloudTest


class CandidateSyncTests(unittest.TestCase):
    def owner(self, head=36000):
        owner = CohortCloudTest.__new__(CohortCloudTest)
        owner.report = dict(chain_id='expected')
        owner.checkpoint = lambda: None
        owner.redact = lambda text: text
        owner.node_rpc = lambda name, _: dict(chain_id='expected', head_block_num=head,
            last_irreversible_block_num=head-2)
        owner.synced = lambda names: dict(common_lib=head-2, common_block_id='agreed')
        return owner

    def test_live_canonical_agreement_required(self):
        owner=self.owner();owner.wait_for=lambda _, predicate, timeout:predicate()
        self.assertEqual(owner.wait_candidate_sync('nodead')['common_block_id'],'agreed')
        row=owner.report['admission_sync'][0]
        self.assertEqual(row['status'],'passed');self.assertEqual(row['timeout_seconds'],1620)

    def test_budget_is_bounded_for_short_and_very_long_chains(self):
        for head, expected in ((17,180),(10**8,1800)):
            owner=self.owner(head)
            def wait(_description,predicate,timeout):
                self.assertEqual(timeout,expected);return predicate()
            owner.wait_for=wait;owner.wait_candidate_sync('nodead')

    def test_progress_beyond_old_180_second_limit_is_not_a_pass(self):
        owner=self.owner();clock=[0.0];peer=[0];leader=[36000]
        owner.node_rpc=lambda name,_:dict(chain_id='expected',head_block_num=leader[0] if name=='eosio' else peer[0],
            last_irreversible_block_num=(leader[0] if name=='eosio' else peer[0])-2)
        owner.synced=lambda _:self.fail('Must not call agreement gate while far behind')
        def wait(_description,predicate,timeout):
            for elapsed in (0,100,200,300):
                clock[0]=elapsed;peer[0]+=1000;leader[0]+=200
                self.assertFalse(predicate())
            raise RuntimeError('bounded test ends without agreement')
        owner.wait_for=wait
        with patch('pureeos_rotation_cohort_cloud_test.time.monotonic',side_effect=lambda:clock[0]):
            with self.assertRaisesRegex(RuntimeError,'without agreement'):owner.wait_candidate_sync('nodead')
        self.assertEqual(owner.report['admission_sync'][0]['status'],'failed')

    def test_peer_stall_is_rejected_before_total_budget(self):
        owner=self.owner();clock=[0.0]
        owner.node_rpc=lambda name,_:dict(chain_id='expected',head_block_num=36000+int(clock[0]) if name=='eosio' else 100,
            last_irreversible_block_num=35000 if name=='eosio' else 98)
        def wait(_description,predicate,timeout):
            self.assertFalse(predicate());clock[0]=180;return predicate()
        owner.wait_for=wait
        with patch('pureeos_rotation_cohort_cloud_test.time.monotonic',side_effect=lambda:clock[0]):
            with self.assertRaisesRegex(RuntimeError,'stopped advancing'):owner.wait_candidate_sync('nodead')

    def test_wrong_chain_and_irreversible_disagreement_are_not_retryable(self):
        for wrong_chain in (True,False):
            owner=self.owner();owner.wait_for=lambda _,predicate,timeout:predicate()
            if wrong_chain:
                owner.node_rpc=lambda name,_:dict(chain_id='expected' if name=='eosio' else 'wrong',
                    head_block_num=36000,last_irreversible_block_num=35998)
            else:
                def disagree(_):raise AssertionError('irreversible mismatch')
                owner.synced=disagree
            with self.assertRaises(AssertionError):owner.wait_candidate_sync('nodead')
            self.assertEqual(owner.report['admission_sync'][0]['status'],'failed')

    def test_total_timeout_is_not_silently_retried(self):
        owner=self.owner()
        def wait(*_):raise RuntimeError('Timeout: nodead original-chain P2P sync')
        owner.wait_for=wait
        with self.assertRaisesRegex(RuntimeError,'Timeout'):owner.wait_candidate_sync('nodead')
        self.assertEqual(len(owner.report['admission_sync']),1)


if __name__ == '__main__':unittest.main()
