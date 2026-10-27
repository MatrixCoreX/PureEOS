import unittest
from types import SimpleNamespace
from parallel_pressure_metrics import corpus_completion, timestamp_ns, block_time_metrics
from test_finality_tps import evidence, event


class PressureMetricTests(unittest.TestCase):
    def test_complete_corpus_includes_warmup_and_drain_latency(self):
        events = evidence()
        events[3]['at_ns'] = 500000000
        result = corpus_completion(events)
        self.assertEqual(result['transactions'], 1)
        self.assertEqual(result['seconds'], 2.5)
        self.assertEqual(result['tps'], .4)

    def test_unique_sends_and_receipts_are_not_double_counted(self):
        events = evidence()
        events += [event('sent', 1.2, id='x')]
        self.assertEqual(corpus_completion(events)['tps'], .5)

    def test_missing_failed_and_future_finalization_fail_closed(self):
        events = evidence()
        events[5]['receipts'][0]['status'] = 'hard_fail'
        with self.assertRaises(RuntimeError):
            corpus_completion(events)
        events = evidence()
        events += [event('sent', 4, id='not-final')]
        with self.assertRaises(RuntimeError):
            corpus_completion(events)
        events = evidence()
        events[3]['at_ns'] = 4000000000
        with self.assertRaises(RuntimeError):
            corpus_completion(events)

    def test_block_timestamps_are_exact_and_timezone_aware(self):
        self.assertEqual(timestamp_ns('1970-01-01T00:00:00.500'), 500000000)
        self.assertEqual(timestamp_ns('1970-01-01T08:00:00.500+08:00'), 500000000)
        self.assertEqual(timestamp_ns('1970-01-01T00:00:01Z'), 1000000000)

    def test_chain_time_diagnostic_keeps_empty_slot_gaps_and_reports_future_lead(self):
        blocks = {'a': {'id': 'a', 'timestamp': '1970-01-01T00:00:00'},
                  'b': {'id': 'b', 'timestamp': '1970-01-01T00:00:01'}}
        node = SimpleNamespace(rpc=lambda method, payload: blocks[payload['block_num_or_id']])
        rows = [{'id': 'a', 'transactions': 10}, {'id': 'empty', 'transactions': 0},
                {'id': 'b', 'transactions': 20}]
        events = [{'type': 'block', 'id': 'a', 'at_ns': 1000000000},
                  {'type': 'block', 'id': 'b', 'at_ns': 500000000}]
        result = block_time_metrics(node, rows, events, {'unix_time_ns': 0, 'monotonic_ns': 0})
        self.assertEqual(result['chain_time_seconds'], 1.5)
        self.assertEqual(result['chain_time_tps'], 20)
        self.assertEqual(result['max_first_observed_block_lead_ms'], 500)


if __name__ == '__main__':
    unittest.main()
