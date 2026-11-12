import unittest

from v1_local_retest_metrics import cpu_delta, explicit_absence


class MetricsControls(unittest.TestCase):
    def test_daemon_permission_or_other_name_error_is_not_proof_of_absence(self):
        self.assertFalse(explicit_absence(1, 'permission denied', 'expected'))
        self.assertFalse(explicit_absence(1, 'Cannot connect to Docker daemon', 'expected'))
        self.assertFalse(explicit_absence(1, 'No such object: expected-other', 'expected'))
        self.assertTrue(explicit_absence(1, 'Error response from daemon: No such container: expected\n', 'expected'))
        self.assertFalse(explicit_absence(0, 'No such object: expected', 'expected'))

    def test_guest_is_not_counted_twice(self):
        delta = cpu_delta([0]*10, [20, 0, 10, 50, 5, 0, 0, 15, 100, 0])
        self.assertEqual(delta['total_ticks'], 100)
        self.assertEqual(delta['steal_percent'], 15)
        self.assertEqual(delta['busy_percent'], 45)
        self.assertEqual(delta['iowait_percent'], 5)

    def test_resets_or_nonpositive_interval_not_invented_as_idle(self):
        self.assertIsNone(cpu_delta([0]*8, [0]*8))
        self.assertIsNone(cpu_delta([1]*8, [0]*8))

    def test_short_counter_record_refused(self):
        with self.assertRaises(ValueError): cpu_delta([0], [1])


if __name__ == '__main__': unittest.main()
