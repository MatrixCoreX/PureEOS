import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

import auto_replacement as runner
import auto_node_entry as entry
import replacement_chain as chain
import retired_unit_cleanup as unit_cleanup
from types import SimpleNamespace
from replacement_policy import admission, similar_name, job_id, validate_job, validate_actions, FUND_UNITS, RAM_UNITS, RESERVE_UNITS, MAX_DAILY_UNITS, MAX_TOTAL_UNITS
from replacement_store import save, load, advance_transaction


class PolicyTest(unittest.TestCase):
    def context(self):
        return {'eligible': 23, 'policy_count': 21, 'threshold': 15, 'recent': 21,
                'head_age': 1, 'LIB_age': 2, 'all_running': True, 'RAM': 2 * 1024**3,
                'swap_free': 40 * 1024**3, 'disk': 30 * 1024**3, 'bank_units': 400000000, 'cpu_pressure60': 30}

    def spend(self):
        return {'daily_jobs': 0, 'daily_units': 0, 'total_units': 0, 'since_last': 10000}

    def test_names_preserve_prefix_valid_and_distinct(self):
        values = [similar_name('openaimodels', x) for x in range(961)]
        self.assertEqual(len(set(values)), 961)
        self.assertTrue(all(v.startswith('openaimode') for v in values))
        self.assertTrue(all(len(v) == 12 and set(v) <= set('12345abcdefghijklmnopqrstuvwxyz') for v in values))

    def test_invalid_name_and_exhaustion_rejected(self):
        for name, ordinal in [('bad0name', 0), ('openaimodels', 961), ('openaimodels', -1)]:
            self.assertRaises(ValueError, similar_name, name, ordinal)

    def test_same_canonical_ban_has_one_job(self):
        self.assertEqual(job_id('openaimodels', 'a' * 64), job_id('openaimodels', 'a' * 64))
        self.assertNotEqual(job_id('openaimodels', 'a' * 64), job_id('openaimodels', 'b' * 64))

    def test_no_funds_stops_before_reservation(self):
        context = self.context()
        context['bank_units'] = 22799158
        self.assertEqual(admission(context, self.spend()), 'WAIT_BANK_FUNDS')

    def test_exact_bank_reserve_boundary(self):
        context = self.context()
        context['bank_units'] = FUND_UNITS + RAM_UNITS + RESERVE_UNITS
        self.assertEqual(admission(context, self.spend()), 'QUALIFIED_ONE')
        context['bank_units'] -= 1
        self.assertEqual(admission(context, self.spend()), 'WAIT_BANK_FUNDS')

    def test_native_safety_and_host_guards(self):
        for key, value in [('eligible', 20), ('policy_count', 19), ('threshold', 13), ('recent', 18),
                           ('head_age', 6), ('LIB_age', 21), ('all_running', False),
                           ('RAM', 1024**3-1), ('swap_free', 8*1024**3-1), ('disk', 25*1024**3-1)]:
            context = self.context()
            context[key] = value
            self.assertNotEqual(admission(context, self.spend()), 'QUALIFIED_ONE', key)

    def test_no_overfill(self):
        context = self.context()
        context['eligible'] = 25
        self.assertEqual(admission(context, self.spend()), 'TARGET_FILLED')

    def test_CPU_overcommit_defers_new_financial_work(self):
        context = self.context()
        context['cpu_pressure60'] = 80.1
        self.assertEqual(admission(context, self.spend()), 'WAIT_CPU_HEADROOM')

    def test_daily_total_and_cooldown_caps(self):
        for key, value, result in [('daily_jobs', 2, 'WAIT_DAILY_BUDGET'),
                                   ('daily_units', MAX_DAILY_UNITS, 'WAIT_DAILY_BUDGET'),
                                   ('total_units', MAX_TOTAL_UNITS, 'WAIT_TOTAL_BUDGET'),
                                   ('since_last', 3599, 'WAIT_ONE_HOUR_COOLDOWN')]:
            spend = self.spend()
            spend[key] = value
            self.assertEqual(admission(self.context(), spend), result)

    def test_financial_payload_tampering_rejected(self):
        args = ('openaimode11', 'fixture-account', 'fixture-production',
                {'Public key': 'fixture-bls', 'Proof of Possession': 'fixture-proof'}, 'fixture-peer', 1000100)
        for stage in ['create', 'fund', 'register']:
            original = validate_actions(stage, None, *args)
            tampered = copy.deepcopy(original)
            tampered[0]['authorization'][0]['actor'] = 'eosio'
            self.assertRaises(ValueError, validate_actions, stage, tampered, *args)
        fund = validate_actions('fund', None, *args)
        for key, value in [('to', 'unowned'), ('quantity', '14001.0000 AIC')]:
            tampered = copy.deepcopy(fund)
            tampered[0]['data'][key] = value
            self.assertRaises(ValueError, validate_actions, 'fund', tampered, *args)
        vote = validate_actions('register', None, *args)
        vote[-1]['data']['voter'] = 'unowned'
        self.assertRaises(ValueError, validate_actions, 'register', vote, *args)

    def test_corrupted_job_cannot_expand_budget_or_redirect_identity(self):
        job = {'id': job_id('openaimodels', 'a'*64), 'replaces': 'openaimodels', 'name': 'openaimode11',
               'ban_block_id': 'a'*64, 'ban_block_num': 100, 'ordinal': 0, 'cpu_units': 1000100,
               'reserved_units': FUND_UNITS + RAM_UNITS, 'stage': 'RESERVED'}
        validate_job(job)
        for key, value in [('id', '../outside'), ('name', 'unownedacc11'), ('cpu_units', 900000000),
                           ('reserved_units', 0), ('ordinal', 10), ('ban_block_num', 0), ('stage', 'UNKNOWN')]:
            tampered = copy.deepcopy(job)
            tampered[key] = value
            self.assertRaises(ValueError, validate_job, tampered)


class Backend:
    def __init__(self):
        self.signs = self.pushes = self.reads = self.verifications = 0
        self.receipt = None
        self.sign_error = self.push_error = None

    def sign(self, intent, directory):
        self.signs += 1
        if self.sign_error:
            raise self.sign_error
        return {'fixture': 'packet'}, 'a'*64, 10, 'b'*64

    def broadcast(self, packet, transaction_id):
        self.pushes += 1
        if self.push_error:
            raise self.push_error

    def reconcile(self, state):
        self.reads += 1
        return self.receipt, state['cursor'] + 1

    def verify_receipt(self, receipt):
        self.verifications += 1


class TransactionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / 'packet/STATE.json'
        self.backend = Backend()
        self.intent = {'actions': [{'fixture': 'owned action'}]}

    def tearDown(self):
        self.temp.cleanup()

    def test_once_broadcast_then_readonly_reconciliation(self):
        self.assertIsNone(advance_transaction(self.path, self.intent, self.backend))
        self.backend.receipt = {'transaction_id': 'a'*64, 'block_num': 12, 'block_id': 'c'*64}
        self.assertIsNotNone(advance_transaction(self.path, self.intent, self.backend))
        advance_transaction(self.path, self.intent, self.backend)
        self.assertEqual((self.backend.signs, self.backend.pushes, self.backend.verifications), (1, 1, 1))

    def test_timeout_preserves_ID_and_never_rebroadcasts(self):
        self.backend.push_error = TimeoutError('fixture ambiguous push')
        advance_transaction(self.path, self.intent, self.backend)
        for _ in range(3):
            advance_transaction(self.path, self.intent, self.backend)
        self.assertEqual((self.backend.signs, self.backend.pushes), (1, 1))
        self.assertEqual(load(self.path)['transaction_id'], 'a'*64)

    def test_crash_after_broadcast_marker_never_rebroadcasts(self):
        self.backend.push_error = KeyboardInterrupt()
        self.assertRaises(KeyboardInterrupt, advance_transaction, self.path, self.intent, self.backend)
        self.assertEqual(load(self.path)['status'], 'BROADCASTING_ONCE')
        advance_transaction(self.path, self.intent, self.backend)
        self.assertEqual((self.backend.signs, self.backend.pushes), (1, 1))

    def test_interrupted_signing_holds_without_new_signature(self):
        self.backend.sign_error = RuntimeError('fixture sign failure')
        self.assertRaises(RuntimeError, advance_transaction, self.path, self.intent, self.backend)
        self.backend.sign_error = None
        self.assertRaises(RuntimeError, advance_transaction, self.path, self.intent, self.backend)
        self.assertEqual((self.backend.signs, self.backend.pushes), (1, 0))

    def test_changed_intent_rejected_without_signing(self):
        advance_transaction(self.path, self.intent, self.backend)
        self.assertRaises(ValueError, advance_transaction, self.path, {'actions': []}, self.backend)
        self.assertEqual((self.backend.signs, self.backend.pushes), (1, 1))


class BootstrapTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.name = 'openaimode11'
        self.node = self.root / 'nodes' / self.name
        (self.node / 'data').mkdir(parents=True)
        (self.node / 'config').mkdir()
        snapshot = self.node / 'bootstrap-snapshot.bin'
        snapshot.write_bytes(b'public synthetic snapshot')
        (self.node / 'bootstrap.json').write_text(json.dumps({'name': self.name, 'snapshot': str(snapshot), 'SHA256': hashlib.sha256(snapshot.read_bytes()).hexdigest()}))

    def tearDown(self):
        self.temp.cleanup()

    def test_only_one_initial_attempt(self):
        self.assertIn('--snapshot', entry.arguments(self.name, self.root))
        self.assertRaises(RuntimeError, entry.arguments, self.name, self.root)

    def test_initialized_database_reused_without_snapshot(self):
        (self.node / 'data/state').mkdir()
        (self.node / 'data/state/chain_head.dat').write_bytes(b'fixture state')
        (self.node / 'bootstrap-complete').touch()
        self.assertNotIn('--snapshot', entry.arguments(self.name, self.root))
        (self.node / 'data/state/chain_head.dat').unlink()
        self.assertRaises(RuntimeError, entry.arguments, self.name, self.root)

    def test_changed_snapshot_never_executes(self):
        (self.node / 'bootstrap-snapshot.bin').write_bytes(b'changed')
        self.assertRaises(ValueError, entry.arguments, self.name, self.root)
        self.assertFalse((self.node / 'bootstrap-attempted').exists())


class FakeAdapter:
    def __init__(self):
        self.calls = []
        self.environment = PolicyTest().context()
        self.retired = [{'owner': 'openaimodels', 'ban_block_num': 100, 'ban_block_id': 'a'*64}]
        self.wait_stage = None

    def trust_checks(self):
        self.calls.append('trust')

    def context(self):
        return self.environment, self.retired

    def account_absent(self, name):
        self.calls.append('absent')
        return True

    def verify_original_ban(self, job):
        self.calls.append('ban')

    def prepare(self, job):
        self.calls.append('prepare')

    def health_before_step(self, job):
        self.calls.append('health')

    def transaction(self, job, stage):
        self.calls.append(stage)
        return None if stage == self.wait_stage else {'transaction_id': stage}

    def start_and_sync(self, job):
        self.calls.append('sync')
        return True

    def commit_roster(self, job):
        self.calls.append('roster')


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        save(self.root / 'AUTO_REPLACEMENT_CONFIG.json', {'enabled': True, 'bank': 'civicbankers', 'target_BPs': 25})
        self.adapter = FakeAdapter()

    def tearDown(self):
        self.temp.cleanup()

    def test_low_balance_creates_no_job_or_key(self):
        self.adapter.environment['bank_units'] = 22799158
        self.assertEqual(runner.run(self.adapter, self.root)['status'], 'WAIT_BANK_FUNDS')
        self.assertNotIn('prepare', self.adapter.calls)
        self.assertFalse(list((self.root / 'jobs').glob('*/STATE.json')))

    def test_full_workflow_keeps_only_owned_self_vote_adapter_boundary(self):
        result = runner.run(self.adapter, self.root)
        self.assertEqual(result['status'], 'PASS_ONE_AUTOMATIC_OWNED_REPLACEMENT')
        self.assertEqual([x for x in self.adapter.calls if x in ['prepare', 'create', 'fund', 'sync', 'register', 'roster']], ['prepare', 'create', 'fund', 'sync', 'register', 'roster'])
        self.assertEqual(load(next((self.root / 'jobs').glob('*/STATE.json')))['stage'], 'COMPLETE')

    def test_pending_transaction_resumes_same_job_without_prepare(self):
        self.adapter.wait_stage = 'fund'
        first = runner.run(self.adapter, self.root)
        self.assertEqual(first['stage'], 'fund')
        job = next((self.root / 'jobs').glob('*/STATE.json'))
        identity = load(job)['id']
        self.adapter.calls.clear()
        self.adapter.wait_stage = None
        result = runner.run(self.adapter, self.root)
        self.assertEqual(result['job'], identity)
        self.assertNotIn('prepare', self.adapter.calls)
        self.assertNotIn('create', self.adapter.calls)
        self.assertEqual(len(list((self.root / 'jobs').glob('*/STATE.json'))), 1)

    def test_completed_job_and_one_hour_cooldown_prevent_duplicate(self):
        runner.run(self.adapter, self.root)
        self.adapter.calls.clear()
        self.assertEqual(runner.run(self.adapter, self.root)['status'], 'WAIT_ONE_HOUR_COOLDOWN')
        self.assertNotIn('prepare', self.adapter.calls)

    def test_wrong_bank_and_disabled_policy_rejected(self):
        for config in [{'enabled': False, 'bank': 'civicbankers', 'target_BPs': 25},
                       {'enabled': True, 'bank': 'eosio', 'target_BPs': 25}]:
            save(self.root / 'AUTO_REPLACEMENT_CONFIG.json', config)
            self.assertRaises(RuntimeError, runner.run, self.adapter, self.root)
            self.assertNotIn('prepare', self.adapter.calls)


class RetirementReconciliationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.adapter = chain.ChainAdapter(self.root)
        self.inflight = {'status': 'NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP',
                         'target': 'openaimodels', 'watchdog': 'fixture-restore',
                         'event': 'guard-events/fixture'}
        self.receipt = {'status': 'PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED',
                        'ban_block_id': 'a'*64}
        self.nodes = {'openaimodels': {'unit': 'fixture.service'}}

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self, banned=True, receipt=True):
        if receipt:
            save(self.root / 'auto-retired/openaimodels.json', self.receipt)
        with patch.object(chain, 'G', self.root), patch.object(chain.guard, 'load', side_effect=lambda n: self.nodes if n == 'GUARD_NODE_MAP.json' else self.inflight), \
             patch.object(chain.guard, 'rows', return_value={'openaimodels': {'banned': banned}}), \
             patch('retire_banned_runtime.retire_confirmed') as retire, \
             patch('retired_unit_cleanup.cleanup'), \
             patch.object(chain.guard, 'save') as store, patch.object(chain.subprocess, 'run') as control:
            self.adapter.retire_pending_bans()
            return retire.call_args, store.call_args_list, control.call_args_list

    def test_confirmed_ban_closes_failed_recycle_without_node_restart(self):
        retired, writes, controls = self.invoke()
        self.assertEqual(self.inflight['status'], 'CLOSED_CANONICAL_BAN_RETIRED_NO_RESTART')
        self.assertTrue(any(x.args[0] == 'GUARD_INFLIGHT.json' for x in writes))
        self.assertEqual([c.args[0] for c in controls], [['systemctl', 'stop', 'fixture-restore.timer']])

    def test_missing_retirement_proof_never_closes_event(self):
        retired, writes, controls = self.invoke(receipt=False)
        self.assertFalse(writes)
        self.assertFalse(controls)

    def test_unbanned_node_not_closed_by_old_receipt(self):
        retired, writes, controls = self.invoke(banned=False)
        self.assertFalse(writes)
        self.assertFalse(controls)

    def test_shared_founder_handoff_receipt_not_accepted_as_deleted(self):
        self.receipt['status'] = 'STOPPED_SHARED_CHAIN_SOURCE_PRESERVED_REQUIRES_ROLE_HANDOFF'
        retired, writes, controls = self.invoke()
        self.assertFalse(writes)
        self.assertFalse(controls)

    def test_banned_new_node_not_yet_committed_to_roster_is_also_retired(self):
        save(self.root / 'jobs/new/NODE.json', {'name': 'openaimode11', 'unit': 'fixture-new.service'})
        with patch.object(chain, 'G', self.root), patch.object(chain.guard, 'load', side_effect=lambda n: self.nodes if n == 'GUARD_NODE_MAP.json' else {'status': 'PASS_ONE_GUARDED_RECYCLE'}), \
             patch.object(chain.guard, 'rows', return_value={'openaimodels': {'banned': True}, 'openaimode11': {'banned': True}}), \
             patch('retire_banned_runtime.retire_confirmed') as retire:
            with patch('retired_unit_cleanup.cleanup'):
                self.adapter.retire_pending_bans()
        self.assertIn('openaimode11', retire.call_args.args[1])


class AdapterBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.adapter = chain.ChainAdapter(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def test_owner_table_time_budget_overlap_advances_primary(self):
        pages = [{'rows': [{'owner': 'a'}], 'more': True, 'next_key': str(chain.guard.owner_primary_key('a'))},
                 {'rows': [{'owner': 'b'}], 'more': False}]
        with patch.object(chain.guard, 'rpc', side_effect=pages) as read:
            self.assertEqual([r['owner'] for r in self.adapter.table('producersv2')], ['a', 'b'])
            self.assertEqual(read.call_args_list[1].args[1]['lower_bound'], str(chain.guard.owner_primary_key('a') + 1))

    def test_duplicate_owner_page_not_accepted(self):
        pages = [{'rows': [{'owner': 'a'}], 'more': True, 'next_key': str(chain.guard.owner_primary_key('a'))},
                 {'rows': [{'owner': 'a'}], 'more': False}]
        with patch.object(chain.guard, 'rpc', side_effect=pages):
            self.assertRaises(RuntimeError, self.adapter.table, 'bpbonds')

    def test_unknown_table_and_incomplete_singleton_rejected(self):
        with patch.object(chain.guard, 'rpc') as read:
            self.assertRaises(ValueError, self.adapter.table, 'unreviewed')
            read.assert_not_called()
        with patch.object(chain.guard, 'rpc', return_value={'rows': [{'version': 1}], 'more': True}):
            self.assertRaises(RuntimeError, self.adapter.table, 'bpbondcfg')

    def test_wallet_command_uses_only_job_socket_and_stdin_secret(self):
        self.adapter.socket_path = self.root / 'private/k.sock'
        result = Mock(returncode=0, stdout='ok', stderr='')
        with patch.object(chain.subprocess, 'run', return_value=result) as call:
            self.adapter.command(['wallet', 'import', '-n', 'autotest'], 'fixture-secret\n')
        args = call.call_args.args[0]
        self.assertIn('unix://' + str(self.adapter.socket_path), args)
        self.assertNotIn('fixture-secret', args)
        self.assertEqual(call.call_args.kwargs['input'], 'fixture-secret\n')
        self.assertIn('--no-auto-keosd', args)

    def test_error_output_redacts_known_server_secret(self):
        self.adapter.socket_path = self.root / 'k.sock'
        self.adapter.secrets = ['fixture-secret']
        result = Mock(returncode=1, stdout='', stderr='failure fixture-secret')
        with patch.object(chain.subprocess, 'run', return_value=result):
            with self.assertRaises(RuntimeError) as error:
                self.adapter.command(['wallet', 'import'], 'fixture-secret\n')
        self.assertNotIn('fixture-secret', str(error.exception))

    def test_unbroadcast_signed_packet_never_enters_public_error(self):
        self.adapter.socket_path = self.root / 'k.sock'
        result = Mock(returncode=1, stdout='{"signatures":["FIXTURE_SIGNATURE"],"packed_trx":"FIXTURE_PACKET"}', stderr='fixture failure')
        with patch.object(chain.subprocess, 'run', return_value=result):
            with self.assertRaises(RuntimeError) as error:
                self.adapter.command(['push', 'transaction'])
        self.assertNotIn('FIXTURE_SIGNATURE', str(error.exception))
        self.assertNotIn('FIXTURE_PACKET', str(error.exception))
        self.assertIn('FIXTURE_PACKET', load(self.root / 'private/diagnostic/last-cli-error.json')['stdout'])

    def test_only_own_auxiliary_signer_child_is_closed(self):
        signer = Mock(pid=123, poll=Mock(return_value=None))
        self.adapter.signer = signer
        with patch.object(chain.subprocess, 'run') as system_control:
            self.adapter.__exit__(None, None, None)
        signer.terminate.assert_called_once()
        signer.wait.assert_called_once()
        system_control.assert_not_called()
        self.assertEqual(load(self.root / 'AUTO_SIGNER_CLOSED.json')['owned_auxiliary_PID'], 123)

    def test_receipt_verification_requires_transaction_in_matching_IR_block(self):
        receipt = {'block_num': 10, 'block_id': 'a'*64, 'transaction_id': 'b'*64}
        with patch.object(chain.guard, 'rpc', side_effect=[{'last_irreversible_block_num': 11}, {'id': 'a'*64, 'transactions': []}]):
            self.assertRaises(RuntimeError, self.adapter.verify_receipt, receipt)

    def test_reconciliation_is_bounded_and_preserves_scan_cursor(self):
        state = {'cursor': 1, 'transaction_id': 'b'*64}
        def read(method, data=None):
            return {'last_irreversible_block_num': 1000} if method == 'get_info' else {'id': 'a'*64, 'transactions': []}
        with patch.object(chain.guard, 'rpc', side_effect=read) as call:
            receipt, cursor = self.adapter.reconcile(state)
        self.assertIsNone(receipt)
        self.assertEqual(cursor, 129)
        self.assertEqual(call.call_count, 129)


class RetiredUnitCleanupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.units = self.root / 'systemd'
        self.units.mkdir()
        self.unit = 'pureeos-fixture-openaimodels.service'
        self.config = self.root / 'nodes/openaimodels/config'
        self.config.mkdir(parents=True)
        (self.units / self.unit).write_text('synthetic unit')
        self.drop = self.units / (self.unit + '.d')
        self.drop.mkdir()
        (self.drop / 'override.conf').write_text('synthetic dropin')
        (self.units / 'unrelated.service').write_text('protected')
        save(self.root / 'guard/auto-retired/openaimodels.json', {'status': 'PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED', 'ban_block_id': 'a'*64})
        self.g = SimpleNamespace(R=self.root / 'guard', rows=lambda: {'openaimodels': {'banned': True, 'ban_final_block': 10}},
                                 state=lambda u: {'MainPID': '0'}, now=lambda: 'fixture-time',
                                 rpc=lambda method, data=None, port=8888: {'last_irreversible_block_num': 20} if method == 'get_info' else {'id': 'a'*64})
        self.nodes = {'openaimodels': {'unit': self.unit, 'config_dir': str(self.config)}}

    def tearDown(self):
        self.temp.cleanup()

    def invoke(self):
        with patch.object(unit_cleanup.subprocess, 'run'):
            return unit_cleanup.cleanup(self.g, self.nodes, self.root / 'case', self.units)

    def test_only_exact_retired_unit_and_dropins_removed_idempotently(self):
        self.assertEqual(len(self.invoke()), 2)
        self.assertFalse((self.units / self.unit).exists())
        self.assertFalse(self.drop.exists())
        self.assertEqual((self.units / 'unrelated.service').read_text(), 'protected')
        self.assertEqual(self.invoke(), [])

    def test_symlink_unit_is_never_followed_or_deleted(self):
        target = self.units / self.unit
        target.unlink()
        target.symlink_to(self.units / 'unrelated.service')
        self.assertRaises(RuntimeError, self.invoke)
        self.assertEqual((self.units / 'unrelated.service').read_text(), 'protected')

    def test_mismatched_canonical_sources_no_deletion(self):
        self.g.rpc = lambda method, data=None, port=8888: {'last_irreversible_block_num': 20} if method == 'get_info' else {'id': str(port)}
        self.assertRaises(RuntimeError, self.invoke)
        self.assertTrue((self.units / self.unit).exists())

    def test_changed_config_since_plan_is_preserved(self):
        save(self.root / 'case/retired-unit-configs/openaimodels.json', {'status': 'STARTED', 'ban_block_id': 'a'*64,
             'files_SHA256': {str(self.units / self.unit): hashlib.sha256(b'old unit').hexdigest()}})
        self.assertRaises(RuntimeError, self.invoke)
        self.assertTrue((self.units / self.unit).exists())

    def test_interrupted_partial_deletion_reconciles_remaining_files(self):
        target = self.units / self.unit
        file = self.drop / 'override.conf'
        save(self.root / 'case/retired-unit-configs/openaimodels.json', {'status': 'STARTED', 'ban_block_id': 'a'*64,
             'files_SHA256': {str(target): hashlib.sha256(target.read_bytes()).hexdigest(), str(file): hashlib.sha256(file.read_bytes()).hexdigest()}})
        target.unlink()
        self.assertEqual(self.invoke(), [str(file)])
        self.assertFalse(self.drop.exists())

    def test_own_automatic_bootstrap_links_removed_shared_snapshot_safety_kept(self):
        owned = self.root / 'case/nodes/openaimodels'
        (owned / 'config').mkdir(parents=True)
        (owned / 'data/finalizers').mkdir(parents=True)
        (owned / 'data/finalizers/safety.dat').write_bytes(b'protected safety')
        shared = self.root / 'shared-founder-snapshot.bin'
        shared.write_bytes(b'public shared checkpoint')
        import os
        os.link(shared, owned / 'bootstrap-snapshot.bin')
        for name in ['bootstrap.json', 'sync-proof.json', 'bootstrap-attempted', 'bootstrap-complete']:
            (owned / name).write_text('fixture metadata')
        self.nodes['openaimodels'].update(config_dir=str(owned / 'config'), data_dir=str(owned / 'data'))
        self.invoke()
        self.assertFalse((owned / 'bootstrap-snapshot.bin').exists())
        self.assertFalse((owned / 'bootstrap.json').exists())
        self.assertEqual(shared.read_bytes(), b'public shared checkpoint')
        self.assertEqual((owned / 'data/finalizers/safety.dat').read_bytes(), b'protected safety')


if __name__ == '__main__':
    unittest.main()
