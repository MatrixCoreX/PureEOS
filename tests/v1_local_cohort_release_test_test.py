"""Offline controls only; these do NOT run a chain or forty processes."""
import unittest

from pureeos_multihost_cloud_test import CloudMultiNodeTest
from v1_local_cohort_release_test import (
    LocalCapacityError, LocalCohortTest, capacity_guard, cohort_names, complete,
)


def example():
    return dict(status='passed', processes_stopped=True, parallel_workers=4, physical_hosts=1,
        production_clock_unchanged=True,
        cohort_phases=[dict(candidate_count=count, status='passed', agreement={'common_lib': 123},
            queue_entries=list(cohort_names(count)),
            finalized_production={'counts': dict.fromkeys(cohort_names(count), 12)}) for count in (30, 40)],
        committee_events=[{'schedule': {'producers': list(range(n))}} for n in range(1, 22)],
        recovered_fault={'missing': {'zero_windows': 1, 'banned': False},
            'recovered': {'zero_windows': 0, 'banned': False}},
        fault=dict(node='nodea', candidate_after=dict(banned=True, zero_windows=3, evidence_slots=[10, 20, 30]),
            actual_post_ban_entries=['nodeb', 'nodec'], preserved_unpaid_blocks=120),
        processes={n: {'pid': i+10} for i, n in enumerate(cohort_names(40))},
        final_synchronization={'common_block_id': 'abc'})


class LocalCohortControls(unittest.TestCase):
    def test_complete_fixture(self): self.assertTrue(complete(example()))

    def test_no_cloud_transport_in_local_mro(self):
        self.assertNotIn(CloudMultiNodeTest, LocalCohortTest.__mro__)
        self.assertFalse(hasattr(LocalCohortTest, 'ssh'))
        self.assertFalse(hasattr(LocalCohortTest, 'remote'))

    def test_failed_partial_or_running_rejected(self):
        for value in ('failed', 'running', 'interrupted'):
            with self.subTest(value=value):
                r = example(); r['status'] = value; self.assertFalse(complete(r))

    def test_missing_phase_or_queue_member_rejected(self):
        r = example(); r['cohort_phases'].pop(); self.assertFalse(complete(r))
        r = example(); r['cohort_phases'][1]['queue_entries'].pop(); self.assertFalse(complete(r))

    def test_missing_or_zero_finalized_output_rejected(self):
        for value in (0, -1):
            r = example(); r['cohort_phases'][1]['finalized_production']['counts']['nodea'] = value
            self.assertFalse(complete(r))
        r = example(); del r['cohort_phases'][1]['finalized_production']['counts']['nodea']
        self.assertFalse(complete(r))

    def test_all_intermediate_seats_required(self):
        r = example(); r['committee_events'].pop(10); self.assertFalse(complete(r))
        r = example(); r['committee_events'].append({'schedule': {'producers': list(range(22))}})
        self.assertFalse(complete(r))

    def test_own_process_shutdown_and_distinct_pids_required(self):
        r = example(); r['processes_stopped'] = False; self.assertFalse(complete(r))
        r = example(); r['processes']['nodea']['pid'] = r['processes']['eosio']['pid']
        self.assertFalse(complete(r))
        r = example(); r['shutdown_errors'] = ['test error']; self.assertFalse(complete(r))

    def test_no_physical_host_or_parallel_evidence_inflation(self):
        r = example(); r['physical_hosts'] = 40; self.assertFalse(complete(r))
        r = example(); r['parallel_workers'] = 0; self.assertFalse(complete(r))
        r = example(); r['production_clock_unchanged'] = False; self.assertFalse(complete(r))

    def test_recovery_must_clear_before_third_miss(self):
        r = example(); r['recovered_fault']['missing']['zero_windows'] = 3; self.assertFalse(complete(r))
        r = example(); r['recovered_fault']['recovered']['zero_windows'] = 1; self.assertFalse(complete(r))

    def test_blacklist_requires_three_distinct_windows(self):
        r = example(); r['fault']['candidate_after']['evidence_slots'] = [10, 10, 20]
        self.assertFalse(complete(r))
        r = example(); r['fault']['candidate_after']['zero_windows'] = 2; self.assertFalse(complete(r))

    def test_blacklisted_account_never_readmitted_and_history_preserved(self):
        r = example(); r['fault']['actual_post_ban_entries'] = ['nodea', 'nodec']; self.assertFalse(complete(r))
        r = example(); r['fault']['actual_post_ban_entries'] = ['nodeb']; self.assertFalse(complete(r))
        r = example(); r['fault']['preserved_unpaid_blocks'] = 0; self.assertFalse(complete(r))

    def test_final_agreement_required(self):
        r = example(); r['final_synchronization'] = None; self.assertFalse(complete(r))

    def test_capacity_does_not_treat_reclaimable_cache_as_occupied_heap(self):
        self.assertEqual(capacity_guard(900, 500, 1000, 1024**3), 400)

    def test_capacity_protects_local_host(self):
        for values in ((880, 0, 1000, 1024**3), (1, 0, 1000, 767*1024**2)):
            with self.subTest(values=values), self.assertRaises(LocalCapacityError): capacity_guard(*values)

    def test_invalid_memory_accounting_is_not_a_pass(self):
        for values in ((-1, 0, 1000, 1024**3), (1, 2, 1000, 1024**3), (1, 0, 0, 1024**3)):
            with self.subTest(values=values), self.assertRaises(ValueError): capacity_guard(*values)

    def test_larger_reserve_stops_before_other_work_is_exhausted(self):
        with self.assertRaises(LocalCapacityError):
            capacity_guard(1, 0, 1000, 1024**3, minimum_available=2 * 1024**3)
        with self.assertRaises(ValueError):
            capacity_guard(1, 0, 1000, 1024**3, minimum_available=0)


if __name__ == '__main__': unittest.main()
