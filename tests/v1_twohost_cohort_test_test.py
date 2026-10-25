"""Reject incomplete cloud-runtime evidence; creates no VMs or node processes."""
import copy
import unittest
from v1_twohost_cohort_test import runtime_flags, require_parallel_flags, require_host_resources, complete_cloud, cohort_names


class RuntimeProofTests(unittest.TestCase):
    def argv(self):
        return ['nodeos','--wasm-runtime','eos-vm-jit','--experimental-parallel-engine','jit',
            '--experimental-parallel-validation-threads','4','--experimental-parallel-validation-batch-size','8',
            '--experimental-parallel-production','--experimental-parallel-code-hash','aaa',
            '--experimental-parallel-code-hash','bbb']

    def test_actual_arguments_preserve_multiple_contract_hashes(self):
        flags=runtime_flags(self.argv())
        require_parallel_flags(flags,['bbb','aaa'])
        self.assertEqual(flags['code_hashes'],['aaa','bbb'])

    def test_private_and_unrelated_arguments_are_not_reported(self):
        flags=runtime_flags(self.argv()+['--signature-provider','PRIVATE_TEST_INPUT','--config-dir','PRIVATE_DIR'])
        self.assertNotIn('PRIVATE',str(flags))

    def test_missing_duplicate_and_different_worker_arguments_fail(self):
        with self.assertRaises(ValueError):runtime_flags(self.argv()+['--wasm-runtime'])
        with self.assertRaises(ValueError):runtime_flags(self.argv()+['--wasm-runtime','eos-vm'])
        for value in ('0','1','16'):
            argv=self.argv();argv[argv.index('--experimental-parallel-validation-threads')+1]=value
            with self.subTest(value=value),self.assertRaises(AssertionError):
                require_parallel_flags(runtime_flags(argv),['aaa','bbb'])

    def test_serial_engine_missing_production_and_wrong_whitelist_fail(self):
        for removed in ('--experimental-parallel-production','--experimental-parallel-engine'):
            argv=self.argv();index=argv.index(removed)
            del argv[index:index+(1 if removed.endswith('production') else 2)]
            with self.subTest(removed=removed),self.assertRaises(AssertionError):
                require_parallel_flags(runtime_flags(argv),['aaa','bbb'])
        with self.assertRaises(AssertionError):require_parallel_flags(runtime_flags(self.argv()),['aaa','ccc'])

    def smoke(self):
        return dict(status='passed',processes_stopped=True,parallel_workers=4,vm_hosts=2,
            final_synchronization={'common_block_id':'abc'},actual_parallel_runtime_verified=True,
            processes={'eosio':{'pid':111},'nodea':{'pid':'peer:222'}},reward_checks={'early_claim_rejected':True},
            resource_samples=[{'hosts':{n:self.resource() for n in ('a','b')}}])

    def resource(self):
        return dict(status='complete',probes={'oom':{'oom_detected':False},
            'memory':{'memory_bytes':{'available':2*1024**3}}})

    def test_complete_both_host_samples_with_memory_reserve_required(self):
        hosts={n:self.resource() for n in ('a','b')};require_host_resources(hosts)
        for mutation in ('missing-peer','incomplete','oom','missing-oom','low-memory'):
            values=copy.deepcopy(hosts)
            if mutation=='missing-peer':values.pop('b')
            elif mutation=='incomplete':values['b']['status']='incomplete'
            elif mutation=='oom':values['b']['probes']['oom']['oom_detected']=True
            elif mutation=='missing-oom':values['b']['probes']['oom']['oom_detected']=None
            else:values['b']['probes']['memory']['memory_bytes']['available']-=1
            with self.subTest(mutation=mutation),self.assertRaises(AssertionError):require_host_resources(values)

    def test_incomplete_resource_evidence_cannot_close_smoke(self):
        for value in ([],[{}],[{'hosts':{'a':self.resource()}}]):
            report=self.smoke();report['resource_samples']=value
            with self.subTest(value=value):self.assertFalse(complete_cloud(report,'smoke'))

    def test_smoke_requires_two_actual_identified_nodes_and_clean_shutdown(self):
        report=self.smoke();self.assertTrue(complete_cloud(report,'smoke'))
        for update in ({'status':'running'},{'processes_stopped':False},{'shutdown_errors':['forced kill']},
                {'actual_parallel_runtime_verified':False},{'parallel_workers':0},{'final_synchronization':{}},
                {'processes':{'eosio':{},'other':{}}},{'reward_checks':{}}):
            altered=copy.deepcopy(report);altered.update(update)
            with self.subTest(update=update):self.assertFalse(complete_cloud(altered,'smoke'))

    def test_short_smoke_and_unknown_phase_never_close_full_cohort(self):
        self.assertFalse(complete_cloud(self.smoke(),'cohort'))
        self.assertFalse(complete_cloud(self.smoke(),'unknown'))

    def cohort(self):
        report=self.smoke();names=cohort_names(40)
        report.update(processes={name:{'pid':index+1000} for index,name in enumerate(names)},
            cohort_phases=[dict(candidate_count=count,status='passed',agreement={'common_block_id':'fixture'},
                queue_entries=cohort_names(count),
                finalized_production={'counts':{name:1 for name in cohort_names(count)}}) for count in (30,40)],
            committee_events=[{'schedule':{'producers':names[:count]}} for count in range(1,22)],
            recovered_fault={'missing':{'zero_windows':1,'banned':0},'recovered':{'zero_windows':0,'banned':0}},
            fault={'node':'nodea','candidate_after':{'banned':1,'zero_windows':3,'evidence_slots':[12,36,60]},
                'actual_post_ban_entries':['nodeb','nodec'],'preserved_unpaid_blocks':12})
        return report

    def test_full_cohort_accepts_both_abi_boolean_representations(self):
        for value in (True,1):
            report=self.cohort();report['fault']['candidate_after']['banned']=value
            with self.subTest(value=value):self.assertTrue(complete_cloud(report,'cohort'))

    def test_other_truthy_values_are_not_abi_ban_evidence(self):
        for value in (False,0,None,2,1.0,'1','true',[],{'banned':True}):
            report=self.cohort();report['fault']['candidate_after']['banned']=value
            with self.subTest(value=value):self.assertFalse(complete_cloud(report,'cohort'))

    def test_full_cohort_still_rejects_missing_and_inconsistent_completion_evidence(self):
        mutations=[lambda r:r['fault'].update(actual_post_ban_entries=['nodeb']),
            lambda r:r['fault'].update(actual_post_ban_entries=['nodea','nodeb']),
            lambda r:r['fault'].update(preserved_unpaid_blocks=0),
            lambda r:r['fault']['candidate_after'].update(evidence_slots=[12,12,60]),
            lambda r:r['recovered_fault']['recovered'].update(zero_windows=1),
            lambda r:r['cohort_phases'][1].update(queue_entries=cohort_names(40)[:-1]),
            lambda r:r['processes']['nodea'].update(pid=r['processes']['eosio']['pid'])]
        for index,mutate in enumerate(mutations):
            report=self.cohort();mutate(report)
            with self.subTest(mutation=index):self.assertFalse(complete_cloud(report,'cohort'))


if __name__=='__main__':unittest.main()
