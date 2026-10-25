import unittest
from parallel_saturation_metrics import utilization


def counter(ticks, start=1):
    return dict(user_ticks=ticks, system_ticks=0, start_ticks=start)


def sample(second, busy=100):
    return {'at_ns': int(second * 1e9), 'clock_ticks_per_second': 100,
            'logical_cpu_ticks': {f'cpu{i}': [busy * second, 0, 0, (100-busy)*second, 0, 0, 0, 0] for i in range(2)},
            'processes': {'9': counter(busy * second)},
            'scheduler': {'threads': {'9': {'10': counter(busy * second)}}}}


class SaturationMetricTests(unittest.TestCase):
    def test_full_cpu_is_measured_not_inferred_from_thread_count(self):
        result = utilization([sample(i) for i in range(4)], 0, 3_000_000_000)
        self.assertEqual(result['status'], 'complete')
        self.assertEqual(result['mean_host_busy_percent'], 100)
        self.assertEqual(result['longest_host_at_least_90_percent_seconds'], 3)
        self.assertEqual(result['per_process_mean_cores']['9'], 1)
        self.assertEqual(result['peak_tasks_using_at_least_5_percent_in_one_interval'], 1)

    def test_edges_excluded_and_low_occupancy_not_called_saturated(self):
        result = utilization([sample(i, 50) for i in range(5)], 500_000_000, 3_500_000_000)
        self.assertEqual(result['observed_seconds'], 2)
        self.assertEqual(result['mean_host_busy_percent'], 50)
        self.assertEqual(result['host_at_least_90_percent_seconds'], 0)

    def test_pid_reuse_missing_topology_and_counter_reset_fail_closed(self):
        for kind in ('pid', 'cpu', 'reset'):
            before, after = sample(1), sample(2)
            if kind == 'pid':
                after['processes']['9']['start_ticks'] = 2
            elif kind == 'cpu':
                after['logical_cpu_ticks'].pop('cpu0')
            else:
                after['logical_cpu_ticks']['cpu0'][0] = 0
            result = utilization([before, after], 0, 3_000_000_000)
            self.assertEqual(result['status'], 'incomplete')
            self.assertEqual(result['missing_intervals'], 1)

    def test_stale_task_is_not_attributed_to_reused_tid(self):
        before, after = sample(1), sample(2)
        after['scheduler']['threads']['9']['10']['start_ticks'] = 2
        result = utilization([before, after], 0, 3_000_000_000)
        self.assertEqual(result['top_task_mean_cores'], [])


if __name__ == '__main__':
    unittest.main()
