import io
import json
import unittest
import urllib.error
from unittest.mock import patch
import stability_guard as g


class ReadRetryTest(unittest.TestCase):
    def setUp(self):
        g.RPC_READ_RETRIES.clear()

    def response(self):
        return io.BytesIO(json.dumps({'rows': [], 'more': False}).encode())

    def http_error(self, code):
        return urllib.error.HTTPError('http://127.0.0.1', code, 'fixture', {}, io.BytesIO(b'fixture ABI deadline'))

    def test_ABI_500_retries_same_read_then_succeeds(self):
        with patch.object(g.urllib.request, 'urlopen', side_effect=[self.http_error(500), self.response()]) as call, patch.object(g.time, 'sleep') as sleep:
            self.assertEqual(g.rpc('get_table_rows', {'table': 'rotcands'})['rows'], [])
            self.assertEqual(call.call_count, 2)
            self.assertEqual(call.call_args_list[0].args[0].data, call.call_args_list[1].args[0].data)
            sleep.assert_called_once_with(.15)
            self.assertEqual(len(g.RPC_READ_RETRIES), 1)

    def test_retry_exhaustion_never_returns_partial_success(self):
        with patch.object(g.urllib.request, 'urlopen', side_effect=[self.http_error(500) for _ in range(3)]) as call, patch.object(g.time, 'sleep'):
            self.assertRaises(urllib.error.HTTPError, g.rpc, 'get_info')
            self.assertEqual(call.call_count, 3)

    def test_nontransient_refusal_not_retried(self):
        with patch.object(g.urllib.request, 'urlopen', side_effect=self.http_error(400)) as call, patch.object(g.time, 'sleep') as sleep:
            self.assertRaises(urllib.error.HTTPError, g.rpc, 'get_info')
            call.assert_called_once()
            sleep.assert_not_called()

    def test_timeout_retries_without_changing_target(self):
        with patch.object(g.urllib.request, 'urlopen', side_effect=[TimeoutError(), self.response()]) as call, patch.object(g.time, 'sleep'):
            g.rpc('get_info', port=8890)
            self.assertEqual([c.args[0].full_url for c in call.call_args_list], ['http://127.0.0.1:8890/v1/chain/get_info'] * 2)

    def test_broadcast_never_allowed(self):
        with patch.object(g.urllib.request, 'urlopen') as call:
            self.assertRaises(AssertionError, g.rpc, 'push_transaction', {})
            call.assert_not_called()

    def test_retry_telemetry_bounded(self):
        replies = [x for _ in range(12) for x in [self.http_error(503), self.response()]]
        with patch.object(g.urllib.request, 'urlopen', side_effect=replies), patch.object(g.time, 'sleep'):
            for _ in range(12):
                g.rpc('get_info')
        self.assertEqual(len(g.RPC_READ_RETRIES), 8)


if __name__ == '__main__':
    unittest.main()
