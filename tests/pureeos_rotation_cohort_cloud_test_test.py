"""Pure 30/40-cohort assertions: never create VMs, keys or node processes."""
import re
import unittest
from unittest.mock import patch

from pureeos_multihost_cloud_test import balanced_candidates, validate_listener_ports
from pureeos_rotation_cohort_cloud_test import CohortCloudTest, cohort_names, check_exact_ranking, full_seat_entries


class CohortControlTests(unittest.TestCase):
    def test_all_account_names_are_valid_and_independent(self):
        names = cohort_names(40)
        self.assertEqual(len(names), 40)
        self.assertEqual(len(set(names)), 40)
        self.assertEqual(names[:22], ('eosio', *('node' + chr(97 + i) for i in range(21))))
        self.assertTrue(all(re.fullmatch('[a-z1-5.]{1,12}', n) for n in names))
        self.assertEqual(names[-1], 'nodeam')

    def test_balanced_thirty_and_forty_layouts(self):
        for count in (30, 40):
            selected, remote = balanced_candidates(cohort_names(40), count)
            self.assertEqual(len(selected), count)
            self.assertEqual(len(remote), count // 2)
            self.assertNotIn('eosio', remote)

    def test_unsupported_count_cannot_create_an_accidental_large_cohort(self):
        for count in (0, 21, 31, 41, 210):
            with self.assertRaises(ValueError): cohort_names(count)
            with self.assertRaises(ValueError): balanced_candidates(cohort_names(40), count)

    def test_existing_twenty_two_fixture_cannot_silently_claim_forty(self):
        with self.assertRaises(ValueError): balanced_candidates(cohort_names(22), 40)

    def test_twenty_peer_listener_pairs_are_nonoverlapping_and_not_ephemeral(self):
        ports = [29000, *range(29111, 29131), *range(29201, 29221)]
        validate_listener_ports(ports, (32768, 60999))
        self.assertEqual(len(ports), 41)

    def test_local_listeners_are_unique_and_outside_ephemeral_allocation(self):
        owner = CohortCloudTest.__new__(CohortCloudTest)
        owner.ports = {28000, 29000}
        with patch('pureeos_rotation_cohort_cloud_test.socket.socket'):
            ports = [owner.port() for _ in range(38)]
        self.assertEqual(ports, list(range(28001, 28039)))
        validate_listener_ports([28000, 29000, *ports], (32768, 60999))

    def test_local_listener_exhaustion_is_fatal_not_an_ephemeral_fallback(self):
        owner = CohortCloudTest.__new__(CohortCloudTest)
        owner.ports = set(range(28001, 28100))
        with self.assertRaises(RuntimeError): owner.port()

    def test_exact_vote_minimum_unit_and_ties_need_no_float(self):
        expected = {'nodea': 2**60 + 1, 'nodeb': 2**60 + 1, 'nodec': 2**60}
        rows = [{'owner': n, 'total_votes': str(expected[n])} for n in expected]
        check_exact_ranking(rows, str(sum(expected.values())), expected)

    def test_duplicate_or_wrong_total_or_fraction_truncation_is_rejected(self):
        expected = {'nodea': 1000001, 'nodeb': 1000000}
        correct = [{'owner': n, 'total_votes': str(v)} for n, v in expected.items()]
        cases = [(correct + [correct[0]], 2000001), (correct, 2000000),
                 ([{'owner': 'nodea', 'total_votes': '1000000'}, correct[1]], 2000000)]
        for rows, total in cases:
            with self.subTest(rows=rows, total=total), self.assertRaises(AssertionError):
                check_exact_ranking(rows, total, expected)

    def test_reversed_equal_vote_tie_is_rejected(self):
        expected = {'nodea': 5, 'nodeb': 5}
        with self.assertRaises(AssertionError):
            check_exact_ranking([{'owner': 'nodeb', 'total_votes': 5},
                                 {'owner': 'nodea', 'total_votes': 5}], 10, expected)

    def test_initial_growth_is_not_a_complete_full_committee_queue_cycle(self):
        events = [dict(schedule=dict(producers=[{}] * size),
                       rotation=dict(incoming='nodea', outgoing=outgoing))
                  for size, outgoing in ((20, ''), (21, ''), (21, 'eosio'))]
        self.assertEqual(full_seat_entries(events), ['nodea'])


if __name__ == '__main__':
    unittest.main()
