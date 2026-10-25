import unittest
from copy import deepcopy
from types import SimpleNamespace
from unittest.mock import patch
import parallel_mixed_stress_benchmark as benchmark
from parallel_mixed_stress_benchmark import per_kind_finality
from parallel_mixed_stress_workload import (balanced_profile, independent_light_profile, independent_transfer_profile, check_independent_rows,
                                         check_ledger, expected_effects, units)


class MixedStressLedgerTests(unittest.TestCase):
    def test_global_transfers_never_reuse_either_account_across_corpus(self):
        for count in (64, 1024, 8192):
            manifest = dict(mode='token', hot_percent=0, transfer_pairs=count, globally_unique_accounts=True)
            entries = [dict(id=str(i), kind='token', action='transfer', actions=1, units=1,
                            contention='independent', **{'from': 'src' + str(i), 'to': 'dst' + str(i)})
                       for i in range(count)]
            result = independent_transfer_profile(manifest, entries)
            self.assertTrue(result['globally_unique_accounts'])
            self.assertEqual(result['distinct_accounts'], 2 * count)
        for change in ({'from': 'src0'}, {'to': 'dst0'}, {'to': 'src1'}, {'id': '0'}, {'action': 'issue'}):
            altered = deepcopy(entries); altered[1].update(change)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                independent_transfer_profile(manifest, altered)
        for altered in (entries[:-1], entries + [entries[0]]):
            with self.assertRaises(RuntimeError):
                independent_transfer_profile(manifest, altered)

    def test_global_light_calls_also_have_distinct_authorized_callers(self):
        manifest = dict(unique_business_rows=True, unique_row_count=64, mode='simple', hot_percent=0,
                        loop_iterations=0, complex_roots=1, transfer_pairs=64, globally_unique_accounts=True)
        entries = [dict(id=str(i), row_key=i + 1, kind='simple', scope='simple', actions=1,
                        action='step', units=0, contention='independent', **{'from': 'src' + str(i)})
                   for i in range(64)]
        self.assertTrue(independent_light_profile(manifest, entries)['globally_unique_callers'])
        for change in ({'from': 'src0'}, {'action': 'run'}, {'row_key': 1}):
            altered = deepcopy(entries); altered[1].update(change)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                independent_light_profile(manifest, altered)

    def test_independent_transfer_pairs_do_not_share_source_or_destination_accounts(self):
        manifest = dict(mode='token', hot_percent=0, transfer_pairs=1024)
        entries = [dict(kind='token', actions=1, units=1, contention='independent',
                        **{'from': 'a' + str(i % 1024), 'to': 'b' + str(i % 1024)}) for i in range(4096)]
        self.assertEqual(independent_transfer_profile(manifest, entries)['distinct_accounts'], 2048)
        for index, change in ((0, {'to': 'a0'}), (1024, {'from': 'a1'}), (0, {'actions': 2})):
            altered = deepcopy(entries); altered[index].update(change)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                independent_transfer_profile(manifest, altered)
    def test_independent_light_profile_requires_globally_unique_business_keys(self):
        manifest = dict(unique_business_rows=True, unique_row_count=64, mode='simple', hot_percent=0,
                        loop_iterations=0, complex_roots=1)
        entries = [dict(id=str(i), row_key=i + 1, kind='simple', scope='simple', actions=1,
                        units=0, contention='independent') for i in range(64)]
        self.assertEqual(independent_light_profile(manifest, entries)['unique_business_rows'], 64)
        for change in (dict(row_key=1), dict(actions=2), dict(kind='complex'), dict(contention='shared')):
            altered = deepcopy(entries); altered[1].update(change)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                independent_light_profile(manifest, altered)

    def test_independent_row_pagination_checks_every_row_and_executed_subset(self):
        from collections import Counter
        changes = Counter({('simple', 2): 1})
        pages = [{'rows': [{'id': '1', 'value': '10'}, {'id': 2, 'value': 11}], 'more': True},
                 {'rows': [{'id': 3, 'value': 10}], 'more': False}]
        arguments = []
        def rpc(method, args):
            arguments.append(args)
            return pages[len(arguments) - 1]
        self.assertEqual(check_independent_rows(SimpleNamespace(rpc=rpc), 3, changes), 2)
        self.assertEqual([args['lower_bound'] for args in arguments], ['1', '3'])
        for rows in ([{'id': 1, 'value': 11}], [], [{'id': 2, 'value': 10}]):
            with self.subTest(rows=rows), self.assertRaises(RuntimeError):
                check_independent_rows(SimpleNamespace(rpc=lambda *a: {'rows': rows, 'more': False}), 3, changes)

    def test_balanced_mix_checks_exact_counts_and_real_row_overlap(self):
        manifest = dict(mode='balanced', hot_percent=50, complex_roots=16,
                        scopes=[str(i) for i in range(128)])
        entries = [dict(kind='simple' if i % 4 < 2 else 'complex',
                        contention='shared' if i % 2 == 0 else 'independent',
                        scope=manifest['scopes'][0 if i % 2 == 0 else 1 + (i // 4) % 127],
                        actions=1 if i % 4 < 2 else 16) for i in range(1024)]
        result = balanced_profile(manifest, entries)
        self.assertEqual(set(result['counts'].values()), {256})
        for change in (dict(kind='simple'), dict(scope='0'), dict(contention='shared'), dict(actions=1)):
            altered = deepcopy(entries)
            altered[3].update(change)
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                balanced_profile(manifest, altered)

    def fixture(self):
        manifest = {'initial_balances': {'alice': 10000, 'bob': 1000,
                    'eosio.stake': 0, 'eosio.ram': 0, 'eosio.fees': 0},
                    'loop_iterations': 0, 'scopes': ['one']}
        kinds = ['token', 'systemstake', 'systemram', 'simple', 'complex']
        entries = [{'id': str(i), 'kind': kind, 'from': 'alice', 'to': 'bob',
                    'units': 201 if kind == 'systemram' else 2 if i < 3 else 0,
                    'actions': 2 if kind == 'complex' else 1, 'scope': 'one'}
                   for i, kind in enumerate(kinds)]
        return manifest, entries

    def test_fixed_point_assets_include_negative_zero(self):
        self.assertEqual(units('1.0001 SYS'), 10001)
        self.assertEqual(units('-0.0001 SYS'), -1)
        for value in ['1.0 SYS', '1.0000 EOS', 'nan SYS', '1.00001 SYS']:
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                units(value)

    def test_all_business_types_conserve_and_round_ram_fee_up(self):
        manifest, entries = self.fixture()
        balances, rows, stakes = expected_effects(manifest, entries, {e['id'] for e in entries})
        self.assertEqual(balances, {'alice': 9793, 'bob': 1002, 'eosio.stake': 4,
                                   'eosio.ram': 199, 'eosio.fees': 2})
        self.assertEqual(rows['simple', 'one'], 1)
        self.assertEqual(rows['test', 'one'], 4)
        self.assertEqual(stakes['alice'], 2)

    def test_only_canonical_executed_subset_changes_ledger(self):
        manifest, entries = self.fixture()
        balances, rows, stakes = expected_effects(manifest, entries, {'0'})
        self.assertEqual(balances['alice'], 9998)
        self.assertEqual(balances['bob'], 1002)
        self.assertFalse(rows or stakes)

    def test_fc_large_integer_strings_are_exact_not_float_or_concatenation(self):
        manifest, entries = self.fixture()
        manifest['initial_balances']['alice'] = '9007199254740993123'
        manifest['initial_balances']['bob'] = '1000'
        balances, _, _ = expected_effects(manifest, entries, {'0'})
        self.assertEqual(balances['alice'], 9007199254740993121)
        self.assertEqual(sum(balances.values()), 9007199254740994123)

    def test_duplicate_unknown_and_bad_entries_fail_closed(self):
        manifest, entries = self.fixture()
        with self.assertRaises(RuntimeError):
            expected_effects(manifest, entries + [entries[0]], {'0'})
        with self.assertRaises(RuntimeError):
            expected_effects(manifest, entries, {'unknown'})
        for change in [dict(kind='unknown'), dict(units=-1), dict(to='unknown')]:
            changed = [{**entries[0], **change}]
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                expected_effects(manifest, changed, {'0'})

    def test_finality_partition_keeps_complete_branch_but_not_other_business_sends(self):
        from test_finality_tps import evidence, event
        events = evidence()
        events += [event('sent', 1.5, id='y'), event('ack', 1.6, id='y', accepted=True)]
        events[5]['receipts'].append({'id': 'y', 'status': 'executed'})
        result = per_kind_finality(events, [{'id': 'x', 'kind': 'token'}, {'id': 'y', 'kind': 'complex'}])
        self.assertEqual(set(result), {'token', 'complex'})
        self.assertEqual(sum(row['finalized_success_unique'] for row in result.values()), 2)
        self.assertEqual(result['token']['finalized_actions_per_second'], .3)
        self.assertEqual(result['complex']['finalized_success_tps'], .1)
        self.assertTrue(all(not row['cohort_unresolved_at_end'] for row in result.values()))

    def test_live_ledger_checks_balances_rows_resources_and_split_independently(self):
        manifest, entries = self.fixture()
        manifest.update(users=['alice'], initial_stake_units={'alice': 100})
        executed = {e['id'] for e in entries}
        balances, rows, stakes = expected_effects(manifest, entries, executed)
        asset = lambda value: f'{value // 10000}.{value % 10000:04d} SYS'
        tables = {('eosio.token', owner, 'accounts'): [{'balance': asset(value)}]
                  for owner, value in balances.items()}
        tables['eosio.token', 'SYS', 'stat'] = [{'supply': asset(sum(balances.values()))}]
        for contract in ('simple', 'test'):
            tables[contract, 'one', 'rows'] = [{'value': 10 + rows[contract, 'one']}]
        tables['eosio', 'alice', 'userres'] = [{'ram_bytes': 100, **{key: asset(100 + stakes['alice'])
                for key in ('net_weight', 'cpu_weight')}}]
        tables['eosio', 'eosio', 'respolicy'] = [{'net_weight': 100, 'cpu_weight': 200}]
        tables['eosio', '0', 'powup.state'] = [{'net': {'weight': 900}, 'cpu': {'weight': 1800}}]
        def node(data):
            return SimpleNamespace(rpc=lambda method, args: {'rows': data[args['code'], args['scope'], args['table']]})
        self.assertEqual(check_ledger(node(tables), manifest, entries, executed)['application_rows_checked'], 2)
        changes = [(('eosio.token', 'alice', 'accounts'), 'balance', '0.0000 SYS'),
                   (('test', 'one', 'rows'), 'value', 0),
                   (('eosio', 'alice', 'userres'), 'cpu_weight', '0.0000 SYS'),
                   (('eosio.token', 'SYS', 'stat'), 'supply', '0.0000 SYS')]
        for key, field, bad in changes:
            damaged = deepcopy(tables); damaged[key][0][field] = bad
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                check_ledger(node(damaged), manifest, entries, executed)
        damaged = deepcopy(tables)
        damaged['eosio', '0', 'powup.state'][0]['cpu']['weight'] = 1799
        with self.assertRaises(RuntimeError):
            check_ledger(node(damaged), manifest, entries, executed)

    def test_large_sender_rate_and_corpus_bounds_are_separate(self):
        files = ['--' + name for name in ('nodeos', 'fixture-exporter', 'presigned-sender',
                    'quiet-gate', 'output-parent', 'system-contracts', 'simple-wasm', 'complex-wasm')]
        base = ['stress'] + [value for flag in files for value in (flag, '/unused-test-path')]
        for tail in (['--rate', '32769'], ['--rate', '16384', '--seconds', '7', '--complex-roots', '1'],
                     ['--rate', '8192', '--seconds', '6', '--complex-roots', '100']):
            with self.subTest(tail=tail), patch('sys.argv', base + tail), patch.object(benchmark, 'preflight') as check:
                with self.assertRaises(RuntimeError):
                    benchmark.main()
                check.assert_not_called()
        # Exact maximum is accepted by argument bounds, then deliberately stop
        # at preflight: this unit test never starts a node or writes a corpus.
        valid = ['--rate', '32768', '--seconds', '4', '--warmup', '0', '--complex-roots', '1']
        with patch('sys.argv', base + valid), patch.object(benchmark, 'preflight', side_effect=LookupError) as check:
            with self.assertRaises(LookupError):
                benchmark.main()
            check.assert_called_once()

    def test_saturation_requires_explicit_optin_and_balanced_mix_requires_exact_half(self):
        files = ['--' + name for name in ('nodeos', 'fixture-exporter', 'presigned-sender',
                    'quiet-gate', 'output-parent', 'system-contracts', 'simple-wasm', 'complex-wasm')]
        base = ['stress'] + [value for flag in files for value in (flag, '/unused-test-path')]
        for tail in (['--rate', '64', '--test-block-cpu-us', '6000000'],
                     ['--rate', '64', '--mode', 'balanced'],
                     ['--rate', '65', '--seconds', '1', '--warmup', '0', '--mode', 'balanced', '--hot-percent', '50']):
            with self.subTest(tail=tail), patch('sys.argv', base + tail), patch.object(benchmark, 'preflight') as check:
                with self.assertRaises(RuntimeError):
                    benchmark.main()
                check.assert_not_called()
        valid = ['--rate', '64', '--mode', 'balanced', '--hot-percent', '50',
                 '--test-block-cpu-us', '6000000', '--saturation-diagnostic']
        with patch('sys.argv', base + valid), patch.object(benchmark, 'preflight', side_effect=LookupError) as check:
            with self.assertRaises(LookupError):
                benchmark.main()
            check.assert_called_once()

    def test_unique_light_rows_and_larger_net_have_explicit_bounds(self):
        files = ['--' + name for name in ('nodeos', 'fixture-exporter', 'presigned-sender',
                    'quiet-gate', 'output-parent', 'system-contracts', 'simple-wasm', 'complex-wasm')]
        base = ['stress'] + [value for flag in files for value in (flag, '/unused-test-path')]
        valid = ['--rate', '64', '--mode', 'simple', '--iterations', '0', '--complex-roots', '1',
                 '--unique-business-rows', '--state-db-mb', '256', '--test-block-net-bytes', '8388608',
                 '--saturation-diagnostic']
        for tail in (['--rate', '64', '--test-block-net-bytes', '8388608'],
                     ['--rate', '64', '--unique-business-rows'],
                     valid + ['--hot-percent', '1'], valid + ['--iterations', '1']):
            with self.subTest(tail=tail), patch('sys.argv', base + tail), patch.object(benchmark, 'preflight') as check:
                with self.assertRaises(RuntimeError):
                    benchmark.main()
                check.assert_not_called()
        with patch('sys.argv', base + valid), patch.object(benchmark, 'preflight', side_effect=LookupError) as check:
            with self.assertRaises(LookupError):
                benchmark.main()
            check.assert_called_once()


if __name__ == '__main__':
    unittest.main()
