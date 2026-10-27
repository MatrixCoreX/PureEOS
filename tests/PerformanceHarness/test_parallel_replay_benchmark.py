#!/usr/bin/env python3
import os
import unittest
from parallel_replay_benchmark import proc_usage, replay_timing, stages_since_process_start, summarize


class ReplayAccountingTests(unittest.TestCase):
    def test_subsecond_timer_uses_actual_denominator_not_rounded_seconds(self):
        count, elapsed = replay_timing("replayed 12 blocks in 0 seconds, 3.28263636363636380 ms/block")
        self.assertEqual(count, 12)
        self.assertAlmostEqual(elapsed, .036109)

    def test_long_timer(self):
        self.assertEqual(replay_timing("replayed 41 blocks in 2 seconds, 50.0 ms/block"), (41, 2.0))

    def test_invalid_or_ambiguous_timer_rejected(self):
        for value in ("", "replayed 1 blocks in 0 seconds, 0 ms/block",
                      "replayed 12 blocks in 4 seconds, 1 ms/block",
                      "replayed 12 blocks in 0 seconds, nan ms/block",
                      "replayed 12 blocks in 0 seconds, 1 ms/block\n" * 2):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                replay_timing(value)

    def test_actual_process_high_water_accounting(self):
        usage = proc_usage(os.getpid())
        self.assertGreater(usage["peak_rss_kib_through_sample"], 0)
        self.assertGreaterEqual(usage["cpu_seconds_through_sample"], 0)

    def test_summary_uses_all_repeats_not_best_sample(self):
        rows = [{"workload": "independent", "mode": mode, "replay_seconds": value,
                 "peak_rss_kib_through_sample": 100 + value, "cpu_seconds_through_sample": value}
                for mode, values in (("old", (2, 4, 8)), ("new-2", (1, 2, 6))) for value in values]
        summary = summarize(rows)["independent"]
        self.assertEqual(summary["new-2"]["runs"], 3)
        self.assertEqual(summary["new-2"]["replay_speed_ratio_vs_old"], 2)
        self.assertEqual(summary["new-2"]["peak_rss_kib_max"], 106)
        self.assertEqual(summary["old"]["replay_seconds_max"], 8)

    def test_stage_scrape_preserves_counters_and_fixed_histograms(self):
        text = '\n'.join((
            'nodeos_stage_duration_seconds_bucket{stage="parallel_execute",le="0.001"} 3',
            'nodeos_stage_duration_seconds_bucket{stage="parallel_execute",le="+Inf"} 3',
            'nodeos_stage_duration_seconds_sum{stage="parallel_execute"} 0.002',
            'nodeos_stage_duration_seconds_count{stage="parallel_execute"} 3'))
        stage = stages_since_process_start(text)["stages"]["parallel_execute"]
        self.assertEqual(stage["attempt_count"], 3)
        self.assertEqual(stage["inclusive_seconds"], .002)
        self.assertEqual(stage["p99_upper_bound_us"], 1000)


if __name__ == "__main__":
    unittest.main()
