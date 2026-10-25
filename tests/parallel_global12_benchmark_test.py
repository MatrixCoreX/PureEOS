import unittest
from parallel_global12_benchmark import compare, configuration_matrix, pipeline_timing


class IndependentGlobal12Tests(unittest.TestCase):
    def test_matrix_has_three_staged_rounds_and_two_live_pipeline_controls(self):
        matrix = configuration_matrix()
        self.assertEqual(len(matrix), 8)
        for mode in (0, 12):
            self.assertEqual(sorted(round_ for round_, producer, validator in matrix if round_ > 0 and producer == validator == mode), [1, 2, 3])
        self.assertEqual(set(matrix[-2:]), {(0, 0, 0), (0, 12, 12)})
        self.assertEqual(configuration_matrix(True), [(1, 0, 0), (1, 12, 12)])

    def test_timing_uses_wall_completion_not_block_slot_and_excludes_bootstrap(self):
        producer = ('<6>info 2026-10-19T01:00:00.000 ld-linux- controller log ] Received block aaa... #5 @ old [trxs: 9, elapsed: 1 us, applying time: 2 us]\n'
                    '<6>info 2026-10-19T02:00:00.000 ld-linux- controller log ] Produced block abc... #101 @ slot [trxs: 64, cpu: 1 us, elapsed: 3 us, producing time: 5 us]\n')
        validator = '<6>info 2026-10-19T02:00:06.700 ld-linux- controller log ] Received block abc... #101 @ slot [trxs: 64, cpu: 1 us, elapsed: 7 us, applying time: 11 us, latency: 0 ms]\n'
        result = pipeline_timing(producer, validator, 100, 64)
        self.assertEqual(result['business_blocks'], 1)
        self.assertEqual(result['validator_lag_last_business_block_ms'], 6700)
        self.assertEqual(result['validator']['sum_reported_applying_time_us'], 11)
        for bad in (validator.replace('abc', 'def'), validator.replace('trxs: 64', 'trxs: 63'), validator * 2):
            with self.assertRaises(RuntimeError): pipeline_timing(producer, bad, 100, 64)

    def test_compare_keeps_zero_window_tps_but_uses_declared_whole_cohort_metric(self):
        samples = [dict(round=i, pipeline=label, valid=True, finality={'finalized_success_tps': 0},
                        corpus_completion={'tps': tps}, runtime_cpu_evidence={'status': 'eligible'})
                   for i in (1, 2, 3) for label, tps in [('wasm0-0', 100), ('wasm12-12', 130)]]
        result = compare(samples)
        self.assertEqual(result['ratio_parallel_vs_serial'], 1.3)
        self.assertTrue(result['repeatable_20_percent_whole_cohort_gain'])
        samples[0]['runtime_cpu_evidence']['status'] = 'inconclusive'
        self.assertFalse(compare(samples)['repeatable_20_percent_whole_cohort_gain'])
        samples[0]['valid'] = False
        with self.assertRaises(RuntimeError): compare(samples)


if __name__ == '__main__': unittest.main()
