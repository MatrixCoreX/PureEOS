#!/usr/bin/env python3
import copy
import json
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import benchmark_environment as environment
from finality_cpp_benchmark import validate_sends, run
from pureeos_benchmark import BenchmarkChain
from pureeos_single_choice_local_test import LocalTest
from stage_report import compare
from finality_tps import analyze, read_events


def sends():
    return [{'type': 'generator_start', 'at_ns': 10, 'clock': 'CLOCK_MONOTONIC', 'boot_id': 'boot'},
            {'type': 'sent', 'at_ns': 30, 'queued_ns': 20, 'id': 'a' * 64, 'actions': 1},
            {'type': 'generator_end', 'at_ns': 40, 'attempted': 1, 'completed': 1, 'errors': 0}]


class SendValidation(unittest.TestCase):
    def test_complete_journal(self):
        self.assertEqual(validate_sends(sends(), 'boot', 1), [sends()[1]])

    def test_clock_domain_mismatch(self):
        with self.assertRaisesRegex(ValueError, 'clock domain'):
            validate_sends(sends(), 'other-boot', 1)

    def test_missing_end_and_trailing_event(self):
        for events in (sends()[:-1], sends() + [sends()[1]]):
            with self.assertRaisesRegex(ValueError, 'Incomplete'):
                validate_sends(events, 'boot', 1)

    def test_duplicate_is_not_extra_tps(self):
        events = sends()
        events.insert(2, copy.deepcopy(events[1]))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            validate_sends(events, 'boot', 2)

    def test_failed_or_partial_sender(self):
        for key in ('errors', 'attempted', 'completed'):
            events = sends()
            events[-1][key] += 1
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'failed'):
                validate_sends(events, 'boot', 1)

    def test_rate_under_delivery(self):
        with self.assertRaisesRegex(ValueError, 'terminated early'):
            validate_sends(sends(), 'boot', 2)

    def test_queue_order(self):
        for queued in (9, 31):
            events = sends()
            events[1]['queued_ns'] = queued
            with self.assertRaisesRegex(ValueError, 'queue timing'):
                validate_sends(events, 'boot', 1)

    def test_clock_backwards(self):
        events = sends()
        events[-1]['at_ns'] = 11
        with self.assertRaisesRegex(ValueError, 'backwards'):
            validate_sends(events, 'boot', 1)

    def test_errors_cannot_be_hidden_by_zero_summary(self):
        events = sends()
        events.insert(2, {'type': 'error', 'at_ns': 35})
        with self.assertRaisesRegex(ValueError, 'failed'):
            validate_sends(events, 'boot', 1)

    def test_malformed_transaction_id(self):
        events = sends()
        events[1]['id'] = 'signed transaction data'
        with self.assertRaisesRegex(ValueError, 'Invalid transaction'):
            validate_sends(events, 'boot', 1)


class EnvironmentTests(unittest.TestCase):
    def test_tmpfs_and_overlay_are_not_ssd(self):
        for fs in ('tmpfs', 'overlay', 'nfs'):
            with patch.object(environment, 'filesystem', return_value={'fstype': fs, 'source': 'none'}):
                with self.assertRaisesRegex(ValueError, 'Real-disk'):
                    environment.preflight(Path('/tmp'))

    def test_space_guard_cannot_be_lowered(self):
        with patch.object(environment, 'filesystem', return_value={'fstype': 'ext4', 'source': '/dev/example'}):
            for free, reserve in ((8 * 2**30, 8), (20 * 2**30, 1)):
                with patch.object(environment.shutil, 'disk_usage', return_value=SimpleNamespace(free=free)):
                    with self.assertRaisesRegex(ValueError, 'free space'):
                        environment.preflight(Path('/tmp'), reserve_gib=reserve)

    def test_adequate_real_disk(self):
        with patch.object(environment, 'filesystem', return_value={'fstype': 'ext4', 'source': '/dev/example'}), \
                patch.object(environment.shutil, 'disk_usage', return_value=SimpleNamespace(free=20 * 2**30)):
            self.assertEqual(environment.preflight(Path('/tmp'))['reserve_bytes'], 8 * 2**30)

    def test_linux_samples_do_not_dump_argv_or_environment(self):
        data = environment.sample([os.getpid(), 999999999])
        self.assertGreater(data['processes'][str(os.getpid())]['rss_pages'], 0)
        self.assertTrue(data['processes']['999999999']['unavailable'])
        for forbidden in ('cmdline', 'environ', 'private_key'):
            self.assertNotIn(forbidden, json.dumps(data).lower())

    def test_sha256_streaming(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / 'example'
            path.write_bytes(b'abc')
            self.assertEqual(environment.sha256(path), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')

    def test_thermal_and_memory_guards(self):
        good = {'temperature_millidegrees': {'cpu': 80000}, 'memory_kib': {'MemAvailable': 2 * 1024 * 1024}}
        environment.check_host_health(good)
        hot = copy.deepcopy(good)
        hot['temperature_millidegrees']['cpu'] = 95000
        with self.assertRaisesRegex(ValueError, 'temperature'):
            environment.check_host_health(hot)
        low = copy.deepcopy(good)
        low['memory_kib']['MemAvailable'] = 100
        with self.assertRaisesRegex(ValueError, 'memory'):
            environment.check_host_health(low)


class BootstrapTests(unittest.TestCase):
    def test_positive_vote_and_rex_before_sealing(self):
        chain = BenchmarkChain.__new__(BenchmarkChain)
        chain.contracts = Path('/unused')
        chain.accounts = {'nodea': {'Public key': 'test-only-public'}}
        chain.bls = {'nodea': {'Public key': 'test-only-bls', 'Proof of Possession': 'test-pop'}}
        with patch.object(LocalTest, 'action') as action, patch.object(chain, 'tx'):
            chain.action('govinit', {}, 'eosio@owner')
            names = [c.args[0] for c in action.call_args_list]
            self.assertEqual(names, ['regproducer', 'regfinkey', 'delegatebw', 'voteproducer', 'deposit', 'buyrex', 'govinit'])
            self.assertEqual(action.call_args_list[3].args[1]['producers'], ['nodea'])

    def test_expected_failed_seal_does_not_register_nodes(self):
        chain = BenchmarkChain.__new__(BenchmarkChain)
        with patch.object(LocalTest, 'action') as action:
            chain.action('govinit', {}, 'eosio@owner', failure='configure PowerUp')
            self.assertEqual(action.call_count, 1)

    def test_private_keys_required_before_network_or_process(self):
        with tempfile.TemporaryDirectory() as directory:
            keys = Path(directory) / 'keys'
            keys.write_text('[]')
            keys.chmod(0o644)
            args = SimpleNamespace(rate=1, seconds=1, warmup=0, drain=1, poll=.01, timeout=1, inflight=1,
                endpoint='http://127.0.0.1:9999', port=9998, accounts='alice,bob', keys=keys)
            with patch('finality_cpp_benchmark.subprocess.Popen') as process:
                with self.assertRaisesRegex(ValueError, 'private'):
                    run(args)
                process.assert_not_called()


class StageReportTests(unittest.TestCase):
    def histogram(self, count, total, small):
        return '\n'.join([
            f'nodeos_stage_duration_seconds_bucket{{stage="onblock",le="0.001"}} {small}',
            f'nodeos_stage_duration_seconds_bucket{{le="+Inf",stage="onblock"}} {count}',
            f'nodeos_stage_duration_seconds_sum{{stage="onblock"}} {total}',
            f'nodeos_stage_duration_seconds_count{{stage="onblock"}} {count}'])

    def test_delta_and_bucket_bounds(self):
        result = compare(self.histogram(2, .002, 2), self.histogram(4, .005, 3))['stages']['onblock']
        self.assertEqual(result['attempt_count'], 2)
        self.assertAlmostEqual(result['mean_us'], 1500)
        self.assertEqual(result['p50_upper_bound_us'], 1000)
        self.assertIsNone(result['p99_upper_bound_us'])

    def test_counter_reset_invalidates_comparison(self):
        with self.assertRaisesRegex(ValueError, 'Counter reset'):
            compare(self.histogram(4, .005, 3), self.histogram(2, .002, 2))

    def test_no_samples_not_zero_latency(self):
        result = compare(self.histogram(2, .002, 2), self.histogram(2, .002, 2))['stages']['onblock']
        self.assertEqual(result['attempt_count'], 0)
        self.assertIsNone(result['mean_us'])

    def test_inconsistent_bucket_counts_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Inconsistent'):
            compare(self.histogram(0, 0, 0), self.histogram(1, .001, 2))


class CppRunnerIntegration(unittest.TestCase):
    def test_joined_journals_and_failed_process(self):
        # Simulator for collector integration only; never evidence of chain TPS.
        state = {'sent': False, 'exit': 0}
        txid = 'a' * 64

        class Observer:
            def __init__(self, endpoint, chain_id, journal, timeout):
                self.journal = journal
                self.known = False

            def initialize(self):
                self.journal.emit('anchor', id='genesis', num=1)
                self.sample()

            def info(self):
                return {'last_irreversible_block_id': 'genesis'}

            def sample(self):
                if state['sent'] and not self.known:
                    self.journal.emit('block', id='block', num=2, previous='genesis',
                                      receipts=[{'id': txid, 'status': 'executed'}])
                    self.known = True
                head = 'block' if self.known else 'genesis'
                self.journal.emit('sample', head_id=head, lib_id=head)

        class Process:
            def __init__(self, command, **kwargs):
                self.pid = os.getpid()
                self.returncode = state['exit']
                if self.returncode:
                    return
                at = time.monotonic_ns()
                records = sends()
                records[0].update(at_ns=at, boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip())
                records[1].update(at_ns=at + 1, queued_ns=at)
                records[2].update(at_ns=at + 2)
                output = Path(command[command.index('--evidence-log') + 1])
                output.write_text(''.join(json.dumps(e) + '\n' for e in records))
                state['sent'] = True

            def poll(self):
                return self.returncode

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            keys = root / 'keys'
            keys.write_text('[]')
            keys.chmod(0o600)
            args = SimpleNamespace(rate=1, seconds=1, warmup=0, drain=.01, poll=.01, timeout=1, inflight=1,
                endpoint='http://127.0.0.1:9999', port=9998, accounts='alice,bob', keys=keys,
                output=root / 'ok', chain_id='test-only', generator=Path('/unused'))
            with patch('finality_cpp_benchmark.Observer', Observer), \
                    patch('finality_cpp_benchmark.subprocess.Popen', Process), \
                    patch('finality_cpp_benchmark.check_host_health'):
                report = run(args)
                recalculated = analyze(read_events(args.output / 'evidence.jsonl'))
                self.assertEqual(report['finalized_success_unique'], 1)
                self.assertEqual(report['cohort_unresolved_at_end'], [])
                self.assertIsNone(report['rpc_accepted_unique_tps_diagnostic'])
                for key, value in recalculated.items():
                    self.assertEqual(report[key], value)
                state.update(exit=2, sent=False)
                args.output = root / 'failed'
                with self.assertRaisesRegex(ValueError, 'unsuccessfully'):
                    run(args)
                self.assertNotIn('"type":"end"', (args.output / 'observer.jsonl').read_text())
                self.assertFalse((args.output / 'report.json').exists())


if __name__ == '__main__':
    unittest.main()
