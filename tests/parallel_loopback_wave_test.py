import unittest
from unittest.mock import Mock,patch
from parallel_loopback_wave import push_packed_wave


class CorrectnessWaveTests(unittest.TestCase):
    def test_preconnected_four_clients_return_in_input_order_and_close(self):
        clients=[]
        for index in range(4):
            client=Mock();response=client.getresponse.return_value
            response.status=202;response.read.return_value=('{"transaction_id":"'+str(index)+'"}').encode()
            clients.append(client)
        with patch('parallel_loopback_wave.http.client.HTTPConnection',side_effect=clients):
            result=push_packed_wave('http://127.0.0.1:9999',[{}]*4)
        self.assertEqual([r['transaction_id'] for r in result],list('0123'))
        for client in clients:client.connect.assert_called_once();client.close.assert_called_once()

    def test_external_authenticated_and_partial_targets_never_connect(self):
        for url,count in (('http://example.com:99',4),('https://127.0.0.1:99',4),
            ('http://name@127.0.0.1:99',4),('http://127.0.0.1:99/path',4),('http://127.0.0.1:99',3)):
            with patch('parallel_loopback_wave.http.client.HTTPConnection') as client:
                with self.assertRaises(ValueError):push_packed_wave(url,[{}]*count)
                client.assert_not_called()

    def test_connect_failure_closes_all_created_clients(self):
        first,second=Mock(),Mock();second.connect.side_effect=OSError('connection failed')
        with patch('parallel_loopback_wave.http.client.HTTPConnection',side_effect=[first,second]):
            with self.assertRaises(OSError):push_packed_wave('http://127.0.0.1:99',[{}]*4)
        first.close.assert_called_once();second.close.assert_called_once()

    def test_business_rejection_cannot_become_a_successful_wave(self):
        clients=[Mock() for _ in range(4)]
        for client in clients:
            client.getresponse.return_value.status=500
            client.getresponse.return_value.read.return_value=b'{"error":"rejected"}'
        with patch('parallel_loopback_wave.http.client.HTTPConnection',side_effect=clients):
            with self.assertRaisesRegex(RuntimeError,'wave rejected'):push_packed_wave('http://127.0.0.1:99',[{}]*4)
        for client in clients:client.close.assert_called_once()


if __name__=='__main__':unittest.main()
