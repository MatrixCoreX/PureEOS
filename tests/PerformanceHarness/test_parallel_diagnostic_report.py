import unittest
from parallel_diagnostic_report import counter_delta, window_summary, block_saturation


class DiagnosticsTests(unittest.TestCase):
    def test_quota_saturation_separates_empty_blocks_and_actual_configuration(self):
        rows = [{'transactions': 0, 'receipt_cpu_us': 0},
                {'transactions': 1, 'receipt_cpu_us': 199},
                {'transactions': 1, 'receipt_cpu_us': 100}]
        out = block_saturation(rows, 200)
        self.assertEqual(out['nonempty_blocks'], 2)
        self.assertEqual(out['transactions'], 2)
        self.assertEqual(out['mean_receipt_cpu_us_per_transaction'], 149.5)
        self.assertIsNone(block_saturation([], 200)['mean_receipt_cpu_us_per_transaction'])
        self.assertEqual(out['nonempty_at_least_99_percent_receipt_cpu'], 1)
        self.assertEqual(block_saturation(rows, 400)['nonempty_at_least_99_percent_receipt_cpu'], 0)
        with self.assertRaises(ValueError):
            block_saturation(rows, 0)

    def test_reused_or_reset_process_is_not_counted(self):
        a = {'start_ticks': 1, 'user_ticks': 10, 'system_ticks': 2}
        self.assertIsNone(counter_delta(a, {**a, 'start_ticks': 2}, ('user_ticks', 'system_ticks')))
        self.assertIsNone(counter_delta(a, {**a, 'user_ticks': 9}, ('user_ticks', 'system_ticks')))
        self.assertEqual(counter_delta(a, {**a, 'user_ticks': 12}, ('user_ticks', 'system_ticks')), 2)

    def test_window_excludes_warmup_and_drain_and_does_not_call_all_busy_background(self):
        def sample(i):
            def proc(name, ticks):
                return {'name': name, 'start_ticks': 1, 'user_ticks': ticks, 'system_ticks': 0}
            return {'at_ns': i * 1000000000, 'clock_ticks_per_second': 100,
                'cpu_ticks': [i * 400, 0, 0, i * 400, 0, 0, 0, 0, i * 100, 0],
                'frequency_khz': {'cpu0': 1000000}, 'collection_ns': 1000000,
                'scheduler': {'kernel_schedstats': 0, 'threads': {}, 'process_cpu': {
                    '1': proc('node', i * 100), '2': proc('node', i * 100),
                    '3': proc('python', i * 10), '4': proc('background', i * 25)}}}
        out = window_summary([sample(i) for i in range(5)], 1000000000, 4000000000,
                             {'producer': 1, 'validator': 2}, 3)
        self.assertEqual(out['samples'], 3)
        self.assertEqual(out['sampled_seconds'], 2)
        self.assertEqual(out['host_idle_percent'], 50)
        self.assertEqual(out['host_busy_cpu_seconds_excluding_iowait'], 8)
        self.assertEqual(out['observed_process_cpu_seconds']['producer'], 2)
        self.assertEqual(out['observed_process_cpu_seconds']['other'], .5)
        self.assertFalse(out['scheduler_wait_available'])

    def test_missing_measurement_is_explicit(self):
        self.assertFalse(window_summary([], 0, 1, {}, 1)['available'])


if __name__ == '__main__':
    unittest.main()
