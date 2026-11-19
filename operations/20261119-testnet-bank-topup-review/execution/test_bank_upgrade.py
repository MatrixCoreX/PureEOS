"""Sender/governance safety tests; no real RPC, keys or chain submissions."""
import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error

import bank_upgrade as b

def state():
    return {**b.fields(b.OLD, b.NEW), 'sealed': True, 'developer': 'eosio.dev', 'pending': True,
            'review_seconds': 1800, 'proposed_at': '2026-11-19T08:00:00.000',
            'execute_after': '2026-11-19T08:30:00.000', 'proposal_id': 6,
            'effective_voter_count': 23, 'execution_blocked_no_electorate': False,
            'effective_veto_count': 0, 'required_vetoes': 16}

class GovernanceTests(unittest.TestCase):
    def test_expected_proposal(self):
        b.verify_proposal(state(), b.OLD, b.NEW, 6)

    def test_tampered_payload(self):
        s = state(); s['new_code_hash'] = '00' * 32
        with self.assertRaises(AssertionError): b.verify_proposal(s, b.OLD, b.NEW)

    def test_wrong_id(self):
        with self.assertRaises(AssertionError): b.verify_proposal(state(), b.OLD, b.NEW, 7)

    def test_short_review_rejected(self):
        s = state(); s['execute_after'] = '2026-11-19T08:29:59.500'
        with self.assertRaises(AssertionError): b.verify_proposal(s, b.OLD, b.NEW)

    def test_native_veto_threshold_rejected(self):
        s = state(); s['effective_veto_count'] = 16
        with self.assertRaises(AssertionError): b.verify_proposal(s, b.OLD, b.NEW)

    def test_empty_electorate_rejected(self):
        s = state(); s['effective_voter_count'] = 0; s['execution_blocked_no_electorate'] = True
        with self.assertRaises(AssertionError): b.verify_proposal(s, b.OLD, b.NEW)

    def test_wall_clock_does_not_replace_irreversible_deadline(self):
        def rpc(method, data=None):
            return {'last_irreversible_block_num': 12} if method == 'get_info' else {'timestamp': '2026-11-19T08:29:59.500'}
        with patch.object(b, 'rpc', rpc), self.assertRaises(AssertionError):
            b.verify_proposal(state(), b.OLD, b.NEW, 6, mature=True)

    def test_irreversible_deadline_accepted(self):
        def rpc(method, data=None):
            return {'last_irreversible_block_num': 12} if method == 'get_info' else {'timestamp': '2026-11-19T08:30:00.000'}
        with patch.object(b, 'rpc', rpc): b.verify_proposal(state(), b.OLD, b.NEW, 6, mature=True)

    def test_restore_proposal_binds_reverse_hashes(self):
        a = b.proposal_actions(b.NEW, b.OLD)
        self.assertEqual([x['name'] for x in a], ['refreshgov', 'devpropose'])
        self.assertEqual(a[1]['data'], b.fields(b.NEW, b.OLD))

class WalletRPCTests(unittest.TestCase):
    def call(self, method, data):
        signer = b.Signer('synthetic', False)
        signer.socket = Path('/tmp/nonexistent-test-only.sock')
        class Response:
            status = 200
            def read(self): return b'[]'
        with patch.object(b.http.client.HTTPConnection, 'request') as request, patch.object(b.http.client.HTTPConnection, 'getresponse', return_value=Response()):
            signer.wallet(method, data)
            return request.call_args.args[2]

    def test_no_argument_wallet_call_has_empty_body(self):
        self.assertEqual(self.call('get_public_keys', []), '')

    def test_key_import_keeps_structured_body(self):
        self.assertEqual(json.loads(self.call('import_key', ['fixture', 'synthetic'])), ['fixture', 'synthetic'])


class SenderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.patcher = patch.object(b, 'ROOT', Path(self.temp.name)); self.patcher.start()
        self.closed = []
        outer = self
        class FakeSigner:
            def __init__(self, label, need_bank): self.private = b.ROOT / 'private' / label
            def __enter__(self): self.private.mkdir(parents=True); return self
            def __exit__(self, *error): outer.closed.append(True)
            def command(self, args):
                return json.dumps({'compression': 0, 'packed_trx': '0102'}) if args[0] == 'convert' else json.dumps({'signatures': ['synthetic'], 'expiration': '2030-01-01T00:00:00'})
        self.signer = patch.object(b, 'Signer', FakeSigner); self.signer.start()
        self.guard = patch.object(b, 'guard', return_value={'stats': {}}); self.guard_mock = self.guard.start()
        self.lock = patch.object(b, 'native_lock', contextlib.nullcontext); self.lock.start()
        self.rpc = patch.object(b, 'rpc', return_value={'last_irreversible_block_num': 10}); self.rpc.start()
        self.receipt = patch.object(b, 'receipt', return_value={'transaction_id': hashlib.sha256(bytes.fromhex('0102')).hexdigest()}); self.receipt_mock = self.receipt.start()
        self.actions = [{'name': 'devpropose'}]

    def tearDown(self):
        for p in [self.receipt, self.rpc, self.lock, self.guard, self.signer, self.patcher]: p.stop()
        self.temp.cleanup()

    def test_success_broadcast_once_and_signer_closed(self):
        result = {'transaction_id': hashlib.sha256(bytes.fromhex('0102')).hexdigest(), 'processed': {'receipt': {'status': 'executed'}}}
        with patch.object(b.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(result).encode())) as network:
            b.send('new', self.actions, b.OLD)
            self.assertEqual(network.call_count, 1)
            self.assertEqual(self.closed, [True])

    def test_http_failure_only_reconciles_same_id(self):
        e = urllib.error.HTTPError('synthetic', 503, 'test', None, io.BytesIO(b'fixture'))
        with patch.object(b.urllib.request, 'urlopen', side_effect=e) as network:
            b.send('new', self.actions, b.OLD)
            self.assertEqual(network.call_count, 1)
            self.receipt_mock.assert_called_once_with('new')
            self.assertEqual(b.load('transactions/new.json')['reported_error'], 'HTTP_503')
            self.assertEqual(self.closed, [True])

    def test_timeout_only_reconciles_no_second_broadcast(self):
        with patch.object(b.urllib.request, 'urlopen', side_effect=TimeoutError('synthetic')) as network:
            b.send('new', self.actions, b.OLD)
            self.assertEqual(network.call_count, 1)
            self.assertEqual(b.load('transactions/new.json')['reported_error'], 'AMBIGUOUS_TRANSPORT_OR_RESPONSE')

    def test_existing_packet_never_signs(self):
        b.save('transactions/new.json', {'transaction_id': 'immutable'})
        with self.assertRaises(AssertionError): b.send('new', self.actions, b.OLD)
        self.guard_mock.assert_not_called()
        self.assertEqual(self.closed, [])

    def test_existing_private_state_never_signs(self):
        (b.ROOT / 'private/new').mkdir(parents=True)
        with self.assertRaises(AssertionError): b.send('new', self.actions, b.OLD)
        self.guard_mock.assert_not_called()

    def test_completed_send_never_repeats(self):
        result = {'transaction_id': hashlib.sha256(bytes.fromhex('0102')).hexdigest(), 'processed': {'receipt': {'status': 'executed'}}}
        with patch.object(b.urllib.request, 'urlopen', return_value=io.BytesIO(json.dumps(result).encode())) as network:
            b.send('new', self.actions, b.OLD)
            with self.assertRaises(AssertionError): b.send('new', self.actions, b.OLD)
            self.assertEqual(network.call_count, 1)

class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.p = patch.object(b, 'ROOT', Path(self.temp.name)); self.p.start()
        b.save('transactions/case.json', {'status': 'BROADCAST_ONCE', 'start_LIB': 10, 'transaction_id': 'fixed'})

    def tearDown(self): self.p.stop(); self.temp.cleanup()

    def test_two_source_canonical_receipt(self):
        def rpc(method, data=None, port=8890):
            if method == 'get_info': return {'last_irreversible_block_num': 11}
            n = data['block_num_or_id']
            return {'id': str(n), 'previous': str(n-1), 'timestamp': '2026-11-19T08:00:00',
                    'transactions': [] if n == 10 else [{'status': 'executed', 'trx': {'id': 'fixed'}}]}
        with patch.object(b, 'rpc', rpc):
            result = b.receipt('case', timeout=1)
            self.assertTrue(result['two_source_canonical'])
            self.assertEqual(b.load('transactions/case.json')['status'], 'IRREVERSIBLE')

    def test_disagreeing_sources_rejected(self):
        def rpc(method, data=None, port=8890):
            if method == 'get_info': return {'last_irreversible_block_num': 11}
            n = data['block_num_or_id']
            return {'id': 'different' if port == 8888 else str(n), 'previous': str(n-1), 'timestamp': '2026-11-19T08:00:00',
                    'transactions': [] if n == 10 else [{'status': 'executed', 'trx': 'fixed'}]}
        with patch.object(b, 'rpc', rpc), self.assertRaises(AssertionError): b.receipt('case', timeout=1)

    def test_unknown_result_never_broadcasts(self):
        with patch.object(b, 'rpc', return_value={'id': '10'}), patch.object(b.urllib.request, 'urlopen') as network:
            with self.assertRaises(RuntimeError): b.receipt('case', timeout=0)
            network.assert_not_called()

if __name__ == '__main__': unittest.main()
