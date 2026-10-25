#!/usr/bin/env python3
"""New-architecture capacity evidence tests; no old binary or chain required."""
from copy import deepcopy
import json
import unittest
from unittest.mock import Mock, patch

from parallel_capacity import (LocalReadOnlyRPC, NoRedirect, account_names, capture,
                               summarize_receipts, validate_parameters)


CHAIN, HEAD, LIB = 'a' * 64, 'b' * 64, 'c' * 64


def info():
    return {'chain_id': CHAIN, 'head_block_num': 12, 'head_block_id': HEAD,
            'last_irreversible_block_num': 10, 'last_irreversible_block_id': LIB}


def parameters():
    return {'chain_config': {'max_block_cpu_usage': 200000, 'max_transaction_cpu_usage': 150000,
                            'min_transaction_cpu_usage': 100, 'max_block_net_usage': 1048576},
            'wasm_config': {'max_pages': 528}}


def receipt(index=1, status='executed', cpu=100, words=3):
    return {'status': status, 'cpu_usage_us': cpu, 'net_usage_words': words, 'trx': f'{index:064x}'}


def block():
    return {'id': LIB, 'block_num': 10, 'transactions': [receipt()]}


class FakeRPC:
    def __init__(self, override=None):
        self.calls, self.override = [], override

    def __call__(self, method, payload=None):
        self.calls.append((method, payload))
        value = {'get_info': info, 'get_consensus_parameters': parameters, 'get_block': block,
                 'get_account': lambda: {'account_name': payload['account_name'], 'head_block_num': 12,
                                        'cpu_limit': {'available': 10000}, 'net_limit': {'available': 20000}},
                 'get_code_hash': lambda: {'account_name': payload['account_name'], 'code_hash': 'd' * 64}}[method]()
        return self.override(len(self.calls), method, value) if self.override else value


class ReceiptAccountingTests(unittest.TestCase):
    def test_all_statuses_and_net_words_are_counted_without_calling_them_tps(self):
        value = block()
        value['transactions'] = [receipt(i, status, i * 100, i) for i, status in enumerate(
            ('executed', 'soft_fail', 'hard_fail', 'delayed', 'expired'), 1)]
        result = summarize_receipts(value, 100)
        self.assertEqual(result['receipt_count'], 5)
        self.assertEqual(result['receipt_cpu_us'], 1500)
        self.assertEqual(result['receipt_net_bytes'], 120)
        self.assertEqual(set(result['statuses'].values()), {1})
        self.assertEqual(result['equal_to_supplied_minimum'], 1)
        self.assertEqual(result['below_supplied_minimum'], 0)
        self.assertNotIn('tps', result)
        self.assertNotIn('remaining_cpu_us', result)

    def test_packed_and_id_receipts_empty_and_below_floor_observations(self):
        value = block()
        value['transactions'][0]['trx'] = {'id': f'{1:064x}', 'transaction': {}}
        self.assertEqual(summarize_receipts(value, 101)['below_supplied_minimum'], 1)
        self.assertIsNone(summarize_receipts(value)['equal_to_supplied_minimum'])
        value['transactions'] = []
        self.assertEqual(summarize_receipts(value)['receipt_cpu_us'], 0)

    def test_malformed_receipts_never_silently_disappear(self):
        invalid = []
        for field, values in (('cpu_usage_us', (-1, True, '100', 1.5, 2**32, None)),
                              ('net_usage_words', (-1, False, '3', 2**32)),
                              ('status', ('unknown', {}, None)), ('trx', (None, {}, 'bad'))):
            for value in values:
                item = block()
                item['transactions'][0][field] = value
                invalid.append(item)
        duplicate = block()
        duplicate['transactions'] *= 2
        invalid.append(duplicate)
        missing = block()
        del missing['transactions']
        invalid.append(missing)
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                summarize_receipts(value)

    def test_exact_uint32_cpu_net_boundaries_and_supplied_floor(self):
        value = block()
        value['transactions'] = [receipt(cpu=2**32 - 1, words=2**32 - 1)]
        self.assertEqual(summarize_receipts(value)['receipt_net_bytes'], (2**32 - 1) * 8)
        for floor in (0, -1, True, 1.5, 2**32):
            with self.subTest(floor=floor), self.assertRaises(ValueError):
                summarize_receipts(value, floor)


class CaptureTests(unittest.TestCase):
    def test_complete_bounded_read_sequence_and_no_historical_parameter_assumption(self):
        rpc = FakeRPC()
        result = capture(rpc, CHAIN, ['alice'], ['eosio'])
        self.assertEqual(result['status'], 'stable_observation_not_atomic')
        self.assertEqual([method for method, _ in rpc.calls], ['get_info', 'get_consensus_parameters',
            'get_block', 'get_account', 'get_code_hash', 'get_consensus_parameters', 'get_info'])
        self.assertEqual(rpc.calls[2][1], {'block_num_or_id': LIB})
        self.assertIsNone(result['finalized_anchor_receipts']['supplied_historical_minimum_cpu_us'])
        self.assertIn('Pending state', result['warning'])
        for entry in result['observations']:
            self.assertLessEqual(entry['started_ns'], entry['finished_ns'])

    def test_changed_chain_head_lib_or_configuration_retains_failure_without_retry(self):
        for field, value in (('chain_id', 'e' * 64), ('head_block_id', 'e' * 64),
                             ('head_block_num', 13), ('last_irreversible_block_id', 'e' * 64),
                             ('last_irreversible_block_num', 11)):
            def modify(index, method, result):
                if index == 5:
                    result[field] = value
                return result
            with self.subTest(field=field):
                rpc = FakeRPC(modify)
                result = capture(rpc, CHAIN)
                self.assertEqual(result['status'], 'invalid')
                self.assertEqual(len(rpc.calls), 5)
                self.assertEqual(result['observations'][-1]['response'][field], value)
        for field in ('chain_config', 'wasm_config'):
            def modify(index, method, result):
                if index == 4:
                    result[field]['new_field'] = 1
                return result
            self.assertEqual(capture(FakeRPC(modify), CHAIN)['status'], 'invalid')

    def test_wrong_initial_chain_stops_before_other_reads(self):
        rpc = FakeRPC()
        result = capture(rpc, 'f' * 64)
        self.assertEqual(result['status'], 'invalid')
        self.assertEqual(len(rpc.calls), 1)

    def test_rpc_error_retains_partial_order_but_not_arbitrary_exception_secrets(self):
        def fail(index, method, value):
            if index == 3:
                raise OSError('secret-token-must-not-appear')
            return value
        result = capture(FakeRPC(fail), CHAIN)
        self.assertEqual(result['error_type'], 'OSError')
        self.assertEqual(result['failed_observation'], 3)
        self.assertEqual(len(result['observations']), 3)
        self.assertIn('finished_ns', result['observations'][-1])
        self.assertNotIn('secret-token', json.dumps(result))

    def test_wrong_anchor_account_and_code_responses_fail_closed(self):
        for target, field, value in (('get_block', 'id', HEAD), ('get_block', 'block_num', 11),
                ('get_account', 'account_name', 'bob'), ('get_account', 'head_block_num', 13),
                ('get_account', 'cpu_limit', None), ('get_code_hash', 'account_name', 'bob'),
                ('get_code_hash', 'code_hash', 'bad')):
            def modify(index, method, result):
                if method == target:
                    result[field] = value
                return result
            with self.subTest(target=target, field=field):
                self.assertEqual(capture(FakeRPC(modify), CHAIN, ['alice'], ['eosio'])['status'], 'invalid')

    def test_selections_are_bounded_before_any_rpc(self):
        for names in (['alice'] * 2, ['alice.'], ['A'], ['a' * 13], ['../keys'], ['a'] * 17):
            rpc = FakeRPC()
            with self.subTest(names=names), self.assertRaises(ValueError):
                capture(rpc, CHAIN, names)
            self.assertEqual(rpc.calls, [])
        self.assertEqual(account_names(['eosio.token', 'alice']), ['eosio.token', 'alice'])

    def test_parameters_require_real_typed_values_not_defaults(self):
        self.assertEqual(validate_parameters(parameters()), parameters())
        for field, value in (('max_block_cpu_usage', 150100), ('max_transaction_cpu_usage', 99),
                              ('min_transaction_cpu_usage', 0), ('min_transaction_cpu_usage', True),
                              ('max_block_net_usage', 0)):
            item = parameters()
            item['chain_config'][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                validate_parameters(item)
        with self.assertRaises(ValueError):
            validate_parameters({})


class TransportTests(unittest.TestCase):
    def test_only_explicit_loopback_origins(self):
        for endpoint in ('http://127.0.0.1:8888', 'http://[::1]:8888/'):
            LocalReadOnlyRPC(endpoint)
        for endpoint in ('https://127.0.0.1:8888', 'http://localhost:8888', 'http://127.0.0.1',
                         'http://192.0.2.1:8888', 'http://user:password@127.0.0.1:8888',
                         'http://127.0.0.1:8888/api', 'http://127.0.0.1:8888/?token=x',
                         'http://127.0.0.1:8888/#token', 'http://127.0.0.1:99999'):
            with self.subTest(endpoint=endpoint), self.assertRaises(ValueError):
                LocalReadOnlyRPC(endpoint)

    def test_write_methods_and_redirects_are_rejected(self):
        rpc = LocalReadOnlyRPC('http://127.0.0.1:8888')
        rpc.opener = Mock()
        for method in ('push_transaction', 'push_block', 'setparams', '../producer/pause'):
            with self.subTest(method=method), self.assertRaises(ValueError):
                rpc(method)
        rpc.opener.open.assert_not_called()
        with self.assertRaises(ValueError):
            NoRedirect().redirect_request(None, None, 302, '', {}, 'http://192.0.2.1')

    def test_response_size_status_and_shape_are_bounded(self):
        rpc = LocalReadOnlyRPC('http://127.0.0.1:8888')
        for body, status, valid in ((b'{}', 200, True), (b'[]', 200, False),
                                     (b'{}', 201, False), (b' ' * 101, 200, False)):
            response = Mock(status=status)
            response.read.return_value = body
            rpc.opener = Mock()
            rpc.opener.open.return_value.__enter__ = Mock(return_value=response)
            rpc.opener.open.return_value.__exit__ = Mock(return_value=False)
            with self.subTest(status=status, body=body), patch('parallel_capacity.MAX_RESPONSE_BYTES', 100):
                if valid:
                    self.assertEqual(rpc('get_info'), {})
                    response.read.assert_called_once_with(101)
                else:
                    with self.assertRaises(ValueError):
                        rpc('get_info')


if __name__ == '__main__':
    unittest.main()
