import copy
import unittest
from pureeos_rotation_cohort_cloud_test import cohort_names
from pureeos_rotation_lifecycle import completion_errors, monitor_action


def complete_report():
    names=cohort_names(40)
    agreement=lambda count:dict(common_block_id='verified',infos={n:{} for n in cohort_names(count)})
    return dict(status='passed',fourhost_script_sha256='expected',vm_hosts=4,processes_stopped=True,
        processes={n:{'pid':str(i)} for i,n in enumerate(names)},
        cohort_phases=[dict(candidate_count=c,status='passed',queue_entries=list(cohort_names(c)),
            finalized_production={'counts':{n:12 for n in cohort_names(c)}},agreement=agreement(c)) for c in (30,40)],
        committee_events=[{'schedule':{'producers':[{}]*i}} for i in range(1,22)],
        recovered_fault=dict(node='nodea',missing=dict(zero_windows=1,banned=False),
            recovered=dict(zero_windows=0,banned=False)),
        fault=dict(node='nodeb',candidate_after=dict(banned=True,zero_windows=3,evidence_slots=[1,2,3]),
            actual_post_ban_entries=['nodec','noded'],preserved_unpaid_blocks=42,finalized_blocks=[{}]),
        host_group_outage=dict(role='c',stopped=['nodej'],threshold=15,surviving_finalizers=16),
        final_synchronization=agreement(40),resource_evidence_status='complete',
        resource_samples=[{'hosts':{r:dict(status='complete',probes={'oom':{'oom_detected':False}}) for r in 'abcd'}}])


class LifecycleTests(unittest.TestCase):
    def test_only_complete_report_is_eligible(self):
        self.assertEqual(completion_errors(complete_report(),'expected'),[])
        self.assertEqual(monitor_action('inactive',complete_report(),'expected'),'collect-verified-archives')

    def test_running_chain_is_never_stopped_by_monitor_restart(self):
        for state in ('active','activating','deactivating'):
            for report in (None,{},complete_report()):
                self.assertEqual(monitor_action(state,report,'expected'),'observe')

    def test_disconnected_or_missing_report_only_retries(self):
        for state in ('unknown','inactive','not-found'):
            self.assertEqual(monitor_action(state,None,'expected'),'retry-observation')

    def test_all_nodes_produced_is_not_complete_queue_coverage(self):
        r=complete_report();r['cohort_phases'][0]['queue_entries']=list(cohort_names(30))[:13]
        self.assertEqual(monitor_action('inactive',r,'expected'),'needs-repair')

    def test_every_required_stage_is_independently_gated(self):
        for field in ('cohort_phases','recovered_fault','fault','host_group_outage','final_synchronization',
                      'resource_samples','committee_events','processes'):
            with self.subTest(field=field):
                r=complete_report();r.pop(field)
                self.assertTrue(completion_errors(r,'expected'))

    def test_source_partial_status_and_shutdown_errors_reject(self):
        for changes in ({'status':'interrupted'},{'status':'running'},
                        {'fourhost_script_sha256':'old'},{'processes_stopped':False},
                        {'shutdown_errors':['unclean']},{'vm_hosts':2}):
            r=complete_report();r.update(changes)
            self.assertTrue(completion_errors(r,'expected'))

    def test_forty_phase_and_actual_production_are_required(self):
        r=complete_report();r['cohort_phases'].pop();self.assertTrue(completion_errors(r,'expected'))
        r=complete_report();r['cohort_phases'][1]['finalized_production']['counts'].pop('nodeam')
        self.assertTrue(completion_errors(r,'expected'))

    def test_metrics_errors_oom_and_missing_host_reject(self):
        for change in ('oom','host','incomplete'):
            r=complete_report();hosts=r['resource_samples'][0]['hosts']
            if change=='oom':hosts['b']['probes']['oom']['oom_detected']=True
            elif change=='host':hosts.pop('d')
            else:hosts['c']['status']='incomplete'
            self.assertTrue(completion_errors(r,'expected'))

    def test_repeat_audit_is_read_only_and_deterministic(self):
        r=complete_report();before=copy.deepcopy(r)
        self.assertEqual(completion_errors(r,'expected'),completion_errors(r,'expected'))
        self.assertEqual(r,before)

    def test_incomplete_ban_or_failed_retry_exclusion_reject(self):
        r=complete_report();r['fault']['candidate_after']['evidence_slots']=[1,1,2]
        self.assertTrue(completion_errors(r,'expected'))
        r=complete_report();r['fault']['actual_post_ban_entries']=['nodeb','nodec']
        self.assertTrue(completion_errors(r,'expected'))


if __name__=='__main__':unittest.main()
