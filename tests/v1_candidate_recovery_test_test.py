"""Pure completion gate tests, not recovery chain evidence."""
from copy import deepcopy
import unittest
from v1_candidate_recovery_test import complete


class RecoveryGateTests(unittest.TestCase):
    def setUp(self):
        cases=('duplicate-once-only','atomic-failure-and-identical-retry',
               'expired-tapos-signature-rejections','invalid-asset-rollback','rex-selection-rollback',
               'bounded-malformed-rpc','expired-dedup-snapshot-agreement','truncated-snapshot-refused',
               'snapshot-restore-and-signed-tail','dirty-refusal-and-checked-replay',
               'same-symbol-different-contract','parallel-independent-wave')
        row=dict(status='passed',processes_stopped=True,parallel_workers=4,
                 parallel_evidence=dict(production=dict(adopted=1),validation=dict(adopted=1)),
                 edge_cases=[dict(name=n) for n in cases])
        self.report=dict(status='passed',parallel_workers=4,rounds=[{**deepcopy(row),'round':n} for n in range(1,4)])

    def test_complete_three_rounds(self):self.assertTrue(complete(self.report))

    def test_missing_case_round_or_actual_adoption_is_not_a_pass(self):
        for n in range(3):
            for mode in ('production','validation'):
                bad=deepcopy(self.report);bad['rounds'][n]['parallel_evidence'][mode]['adopted']=0
                self.assertFalse(complete(bad))
            for case in range(len(self.report['rounds'][n]['edge_cases'])):
                bad=deepcopy(self.report);bad['rounds'][n]['edge_cases'].pop(case)
                self.assertFalse(complete(bad))
        bad=deepcopy(self.report);bad['rounds'].pop();self.assertFalse(complete(bad))
        bad=deepcopy(self.report);bad['rounds'][2]['round']=2;self.assertFalse(complete(bad))

    def test_failed_shutdown_or_serial_profile_cannot_pass(self):
        for key,value in (('processes_stopped',False),('status','failed'),('parallel_workers',0)):
            bad=deepcopy(self.report);bad['rounds'][1][key]=value
            self.assertFalse(complete(bad))
        self.assertFalse(complete({**self.report,'parallel_workers':0}))


if __name__=='__main__':unittest.main()
