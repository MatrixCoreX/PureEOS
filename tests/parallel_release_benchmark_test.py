#!/usr/bin/env python3
"""Pure harness tests: no node, chain, socket bind, compiler or TPS sample."""
import json
import subprocess
import sys
import time
from types import SimpleNamespace
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
from parallel_release_benchmark import final_ids, head_business_ids, summarize, comparison_status, performance_release_eligible, receipt_capacity, pin_paused_head, producer_health_errors, validator_workers
from parallel_release_benchmark import action_delta, expected_row_totals, check_adoption
from parallel_release_benchmark import parallel_diagnostics, PARALLEL_DIAGNOSTIC_FIELDS
from parallel_release_benchmark import admission_diagnostics, ADMISSION_DIAGNOSTIC_FIELDS
from parallel_release_benchmark import intake_diagnostics, INTAKE_DIAGNOSTIC_FIELDS
from parallel_release_benchmark import test_block_cpu_limit, fixture_resource_policy, check_live_resource_policy
from parallel_release_benchmark import validate_sender_ready, wait_sender_ready, finish_runtime_cpu_evidence, append_runtime_end_sample, end_runtime_observation
from parallel_execution_test import Node
from benchmark_environment import wait_for_quiet_gate, process_cpu, scheduler_sample, runtime_cpu_evidence, active_runtime_cpu_evidence


class ReleaseHarnessTests(unittest.TestCase):
    def run_readiness_child(self, body, expected_error=None, timeout=1):
        observer, journal = Mock(), Mock()
        before = time.monotonic_ns()
        child = subprocess.Popen([sys.executable, '-c', body], stdout=subprocess.PIPE)
        try:
            if expected_error:
                with self.assertRaisesRegex((RuntimeError, ValueError), expected_error):
                    wait_sender_ready(child, 7, observer, journal, before, timeout=timeout)
            else:
                ready = wait_sender_ready(child, 7, observer, journal, before, timeout=timeout)
                self.assertEqual(ready['count'], 7)
        finally:
            if child.poll() is None: child.terminate()
            child.wait(timeout=5)
            child.stdout.close()
        return observer

    def test_real_pipe_sender_readiness(self):
        self.run_readiness_child('import json,time; print(json.dumps(dict(status="ready",protocol=1,count=7,ready_ns=time.monotonic_ns())),flush=True); time.sleep(5)')

    def test_partial_sender_ready_line_times_out_and_keeps_observing(self):
        observer = self.run_readiness_child('import sys,time; sys.stdout.write("{"); sys.stdout.flush(); time.sleep(5)',
                                          'timed out', timeout=.25)
        self.assertGreater(observer.sample.call_count, 0)

    def test_oversized_or_wrong_sender_ready_is_rejected(self):
        self.run_readiness_child('import time; print("x"*4097,flush=True); time.sleep(5)', 'Oversized')
        self.run_readiness_child('import json,time; print(json.dumps(dict(status="ready",protocol=2,count=7,ready_ns=time.monotonic_ns())),flush=True); time.sleep(5)', 'Malformed')
        self.run_readiness_child('import time; print("{}\\n{}",flush=True); time.sleep(5)', 'Unexpected output')

    def test_closed_sender_ready_channel_is_rejected(self):
        self.run_readiness_child('pass', 'closed readiness|exited before readiness')

    def test_sender_ready_protocol_checks_corpus_clock_and_exact_integer_types(self):
        ready = dict(status="ready", protocol=1, count=123, ready_ns=1000)
        self.assertEqual(validate_sender_ready(ready, 123, 900, 1100), ready)
        for key, value in (("status", "completed"), ("protocol", True), ("protocol", 2),
                           ("count", 124), ("count", 123.0), ("ready_ns", 899),
                           ("ready_ns", 1101), ("ready_ns", True), ("ready_ns", 1000.0)):
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                validate_sender_ready({**ready, key: value}, 123, 900, 1100)
        for key in ready:
            bad = dict(ready); bad.pop(key)
            with self.assertRaises(RuntimeError):
                validate_sender_ready(bad, 123, 900, 1100)

    def policy_fixture(self, override=None):
        defaults = {"max_block_cpu_usage": 200000, "max_transaction_cpu_usage": 150000,
                    "min_transaction_cpu_usage": 100, "max_block_net_usage": 1048576,
                    "target_block_cpu_usage_pct": 1000, "max_authority_depth": 6}
        actual = dict(defaults)
        if override is not None:
            actual["max_block_cpu_usage"] = override
        return {"initial_configuration": actual}, {"default_initial_configuration": defaults,
                                                   "test_capacity_diagnostic": override is not None}

    def test_capacity_diagnostic_is_explicit_bounded_and_not_default_release_evidence(self):
        for value in (None, 200000, 200001, 800000):
            self.assertEqual(test_block_cpu_limit(value), value)
            genesis, manifest = self.policy_fixture(value)
            out = fixture_resource_policy(genesis, manifest, value)
            self.assertEqual(out["diagnostic_only"], value is not None)
            self.assertEqual(out["production_default_performance_evidence"], value is None)
        for value in (-1, 0, 199999, 800001, 2**32, True, "800000", 800000.0):
            with self.assertRaises(RuntimeError):
                test_block_cpu_limit(value)

    def test_capacity_diagnostic_rejects_stale_exporters_and_unrequested_changes(self):
        import copy
        genesis, manifest = self.policy_fixture(800000)
        for value in (None, 200000):
            with self.assertRaises(RuntimeError):
                fixture_resource_policy(genesis, manifest, value)
        for key in manifest:
            bad = dict(manifest); bad.pop(key)
            with self.assertRaises(RuntimeError):
                fixture_resource_policy(genesis, bad, 800000)
        for key in genesis["initial_configuration"]:
            bad = copy.deepcopy(genesis)
            bad["initial_configuration"][key] += 1
            with self.assertRaises(RuntimeError):
                fixture_resource_policy(bad, manifest, 800000)
        bad = copy.deepcopy(genesis); bad["initial_configuration"]["min_transaction_cpu_usage"] = 100.0
        with self.assertRaises(RuntimeError):
            fixture_resource_policy(bad, manifest, 800000)
        bad = copy.deepcopy(manifest); bad["default_initial_configuration"]["max_block_cpu_usage"] = 800000
        with self.assertRaises(RuntimeError):
            fixture_resource_policy(genesis, bad, 800000)

    def test_live_resource_policy_must_match_before_and_after_each_sample(self):
        genesis, _ = self.policy_fixture(800000)
        expected = genesis["initial_configuration"]
        actual = {**expected, "max_sync_call_depth": 16}
        self.assertEqual(check_live_resource_policy({"chain_config": actual}, expected), actual)
        for response in ({}, {"chain_config": None}, {"chain_config": {}},
                         {"chain_config": {**actual, "max_block_cpu_usage": 200000}},
                         {"chain_config": {**actual, "min_transaction_cpu_usage": 1}},
                         {"chain_config": {**actual, "max_block_cpu_usage": 800000.0}}):
            with self.assertRaises(RuntimeError):
                check_live_resource_policy(response, expected)

    def test_admission_counters_keep_missing_and_malformed_records_distinct(self):
        values = (9, 12, 100, 30, 4)  # bypass also includes a previously billed queue head
        payload = ", ".join(f"{key}={v}" for key, v in zip(ADMISSION_DIAGNOSTIC_FIELDS, values))
        line = "Experimental parallel production admission: " + payload + "; local queries\n"
        self.assertIsNone(admission_diagnostics(""))
        self.assertEqual(admission_diagnostics(line), dict(zip(ADMISSION_DIAGNOSTIC_FIELDS, values)))
        for bad in (line + line, line.replace("capacity_reductions=9", "capacity_reductions=-1"),
                    line.replace("ready_no_receipt=4", "ready_no_receipt=not-a-counter"),
                    line.replace("receipt_observations=100, ", "")):
            with self.assertRaises(RuntimeError):
                admission_diagnostics(bad)

    def test_live_intake_diagnostics_do_not_confuse_dispatch_with_adoption(self):
        values = (64, 2, 4, 80, 3, 64, 4 * 1024 * 1024)
        payload = ", ".join(f"{key}={v}" for key, v in zip(INTAKE_DIAGNOSTIC_FIELDS, values))
        line = "Experimental parallel intake: " + payload + "; local dispatch counts\n"
        self.assertIsNone(intake_diagnostics(""))
        self.assertEqual(intake_diagnostics(line), dict(zip(INTAKE_DIAGNOSTIC_FIELDS, values)))
        for bad in (line + line, line.replace("staged=64", "staged=-1"),
                    line.replace("drains=2", "drains=65"), line.replace("peak_transactions=64", "peak_transactions=65"),
                    line.replace("peak_bytes=4194304", "peak_bytes=4194305"),
                    line.replace("batch_offered=80", "batch_offered=7"), line.replace("batch_calls=4, ", "")):
            with self.assertRaises(RuntimeError):
                intake_diagnostics(bad)

    def test_live_intake_single_recovery_probes_are_not_parallel_width(self):
        values = (4, 1, 2, 3, 1, 4, 100)
        payload = ", ".join(f"{key}={v}" for key, v in zip(INTAKE_DIAGNOSTIC_FIELDS, values))
        line = "Experimental parallel intake: " + payload + ", single_probe_offers=1; local counts\n"
        expected = {**dict(zip(INTAKE_DIAGNOSTIC_FIELDS, values)), "single_probe_offers": 1}
        self.assertEqual(intake_diagnostics(line), expected)
        for bad in (line.replace("single_probe_offers=1", "single_probe_offers=3"),
                    line.replace("single_probe_offers=1", "single_probe_offers=-1"),
                    line.replace("batch_offered=3", "batch_offered=2")):
            with self.assertRaises(RuntimeError):
                intake_diagnostics(bad)

    def test_parallel_diagnostics_preserve_overlaps_and_missing_records(self):
        values = (3, 4, 5, 6, 7, 8, 9, 10, 12, 11)
        payload = ", ".join(f"{key}={value}" for key, value in zip(PARALLEL_DIAGNOSTIC_FIELDS, values))
        line = "Experimental parallel production diagnostics: " + payload + "; local overlapping counters\n"
        self.assertEqual(parallel_diagnostics(line), dict(zip(PARALLEL_DIAGNOSTIC_FIELDS, values)))
        self.assertIsNone(parallel_diagnostics(""))
        self.assertIsNone(parallel_diagnostics(line, "validation"))
        self.assertEqual(parallel_diagnostics(line.replace("production", "validation"), "validation"),
                         dict(zip(PARALLEL_DIAGNOSTIC_FIELDS, values)))
        for bad in (line + line, line.replace("worker_deadlines=7", "worker_deadlines=-1"),
                    line.replace("prepared_successes=3, ", ""),
                    line.replace("preparation_deadlines=4", "preparation_deadlines=100"),
                    line.replace("prefix_refresh_successes=11", "prefix_refresh_successes=13")):
            with self.assertRaises(RuntimeError):
                parallel_diagnostics(bad)
        with self.assertRaises(RuntimeError):
            parallel_diagnostics(line, "other")

    def test_actual_action_counts_and_checksum_contribute_to_state(self):
        self.assertEqual(action_delta(0, False), 1)
        self.assertEqual(action_delta(999, False), 1)
        self.assertEqual(action_delta(0, True), 2)
        self.assertEqual(action_delta(1, True), 56606)
        self.assertEqual(action_delta(0, True, "sha256"), 2)
        self.assertNotEqual(action_delta(1, True, "sha256"), action_delta(2, True, "sha256"))
        with self.assertRaisesRegex(RuntimeError, "Unknown checksum"):
            action_delta(1, True, "unknown")
        entries = [{"id": "a", "scope": "r", "actions": 100},
                   {"id": "b", "scope": "r", "actions": 16},
                   {"id": "c", "scope": "s", "actions": 1}]
        self.assertEqual(expected_row_totals(entries, {"a", "b"}, 2), {"r": 232})
        self.assertEqual(expected_row_totals([{"id": "i", "scope": "r", "actions": 1,
                                              "row_updates": 100}], {"i"}, 2), {"r": 200})
        with self.assertRaisesRegex(RuntimeError, "action count"):
            expected_row_totals([{"id": "a", "scope": "r", "actions": 0}], {"a"}, 1)

    def test_hundred_action_bypass_is_never_called_parallel_adoption(self):
        check_adoption(0, 100, None)
        check_adoption(4, 100, {"attempts": 0, "adopted": 0})
        check_adoption(4, 100, None)
        check_adoption(4, 16, {"attempts": 20, "adopted": 10})
        for actions, totals in ((16, {"attempts": 0, "adopted": 0}),
                                (100, {"attempts": 1, "adopted": 0}), (16, None)):
            with self.assertRaises(RuntimeError):
                check_adoption(4, actions, totals)

    def test_fixed_validator_does_not_track_producer_threads(self):
        self.assertEqual([validator_workers(w, 0) for w in (0, 1, 2, 4)], [0, 0, 0, 0])
        self.assertEqual([validator_workers(w, 2) for w in (0, 1, 2, 4)], [2, 2, 2, 2])
        self.assertEqual([validator_workers(w, None) for w in (0, 1, 2, 4)], [0, 1, 2, 4])
        for invalid in (-1, 17):
            with self.assertRaisesRegex(RuntimeError, "validator"):
                validator_workers(0, invalid)

    def test_scheduler_evidence_handles_comm_parentheses_and_missing_processes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            proc = root / '123'
            task = proc / 'task/123'
            task.mkdir(parents=True)
            fields = ['0'] * 22
            fields[0], fields[11], fields[12], fields[19] = 'S', '11', '7', '99'
            stat = '123 (name (with) spaces) ' + ' '.join(fields)
            (proc / 'stat').write_text(stat)
            (task / 'stat').write_text(stat)
            (task / 'schedstat').write_text('1000 2000 3\n')
            (task / 'status').write_text('voluntary_ctxt_switches: 4\nnonvoluntary_ctxt_switches: 5\n')
            (root / '456').mkdir()  # disappearing process
            result = scheduler_sample([123, 789], root)
            self.assertEqual(process_cpu(proc), {'name': 'name (with) spaces', 'user_ticks': 11,
                                                'system_ticks': 7, 'start_ticks': 99})
            self.assertEqual(result['missed_processes'], 1)
            self.assertEqual(result['missed_threads'], 1)
            self.assertEqual(result['threads']['123']['123']['schedstat'], [1000, 2000, 3])
            self.assertEqual(result['threads']['123']['123']['context_switches']['voluntary_ctxt_switches'], 4)
            self.assertIsNone(result['kernel_schedstats'])
            (proc / 'stat').write_text('malformed')
            with self.assertRaises(ValueError):
                process_cpu(proc)

    def test_producer_lifecycle_failures_cannot_be_counted_as_healthy_tps(self):
        message = "called produce_block while not actually producing"
        self.assertEqual(producer_health_errors("producer_exception: " + message), [message])
        self.assertEqual(producer_health_errors("Produced block, timestamp in current round, latency: -4000ms"), [])
        self.assertEqual(producer_health_errors("producing without pending_block_state"),
                         ["producing without pending_block_state"])

    def row(self, workers, tps, valid=True):
        return {"workers": workers, "valid": valid, "finality": {"finalized_success_tps": tps},
                "runtime_cpu_evidence": {"status": "eligible"}}

    @staticmethod
    def cpu_sample(seconds, host_busy, node_cpu, guest=0):
        return {"at_ns": int(seconds * 1e9), "clock_ticks_per_second": 100,
                "cpu_ticks": [host_busy, 0, 0, int(seconds * 800), 0, 0, 0, 0, guest, 0],
                "processes": {"7": {"user_ticks": node_cpu, "system_ticks": 0, "start_ticks": 3}}}

    def test_runtime_cpu_evidence_excludes_idle_iowait_and_guest_double_count(self):
        first = self.cpu_sample(0, 0, 0)
        last = self.cpu_sample(1, 120, 100, guest=1000)
        last["cpu_ticks"][4] = 1000
        result = runtime_cpu_evidence([first, last])
        self.assertEqual(result["status"], "eligible")
        self.assertAlmostEqual(result["mean_non_node_cores"], .2)
        self.assertAlmostEqual(result["observed_seconds"], 1)

    def test_runtime_cpu_gate_keeps_slow_and_busy_intervals(self):
        result = runtime_cpu_evidence([self.cpu_sample(0, 0, 0), self.cpu_sample(1, 300, 100)])
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["mean_non_node_cores"], 2)
        # A quiet average cannot hide a burst over the per-interval limit.
        samples = [self.cpu_sample(i, i * 100 + (120 if i else 0), i * 100) for i in range(5)]
        result = runtime_cpu_evidence(samples)
        self.assertAlmostEqual(result["mean_non_node_cores"], .3)
        self.assertEqual(result["status"], "inconclusive")

    def test_active_cpu_span_keeps_all_boundary_intervals_and_separate_lifecycle(self):
        rows = [self.cpu_sample(i, 200 + i * 100 if i else 0, max(0, i - 1) * 100) for i in range(7)]
        self.assertEqual(runtime_cpu_evidence(rows)['status'], 'inconclusive')
        result = active_runtime_cpu_evidence(rows, 2_250_000_000, 4_250_000_000)
        self.assertEqual(result['status'], 'eligible')
        self.assertEqual((result['bracket_start_ns'], result['bracket_end_ns']), (2_000_000_000, 5_000_000_000))
        self.assertEqual(result['selected_samples'], 4)
        self.assertEqual(result['source_samples'], 7)
        self.assertEqual((result['mean_limit_cores'], result['interval_limit_cores'], result['max_interval_seconds']), (.5, 1.0, 3.0))

    def test_active_cpu_never_discards_noise_at_middle_or_boundary_intervals(self):
        for noisy_interval in (1, 2, 4):
            rows = [self.cpu_sample(i, i * 100 + (200 if i > noisy_interval else 0), i * 100) for i in range(7)]
            result = active_runtime_cpu_evidence(rows, 1_500_000_000, 4_500_000_000)
            self.assertEqual(result['status'], 'inconclusive')
            self.assertEqual(result['max_interval_non_node_cores'], 2)

    def test_active_cpu_requires_complete_brackets_order_and_valid_declared_boundaries(self):
        rows = [self.cpu_sample(i, i * 100, i * 100) for i in range(1, 7)]
        for start, end in ((500_000_000, 3_000_000_000), (2_000_000_000, 6_500_000_000),
                           (True, 5_000_000_000), (1.0, 5_000_000_000), (5, 5), (6, 5), (0, 5)):
            self.assertEqual(active_runtime_cpu_evidence(rows, start, end)['status'], 'inconclusive')
        for bad in (rows[::-1], rows[:2] + rows[1:], [], [{**rows[0], 'at_ns': True}, *rows[1:]]):
            self.assertEqual(active_runtime_cpu_evidence(bad, 2_000_000_000, 4_000_000_000)['status'], 'inconclusive')

    def test_active_cpu_does_not_trim_dead_edges_or_sampling_gaps(self):
        import copy
        rows = [self.cpu_sample(i, i * 100, i * 100) for i in range(7)]
        for index in (1, 3, 5):
            bad = copy.deepcopy(rows); bad[index]['processes'] = {}
            self.assertEqual(active_runtime_cpu_evidence(bad, 1_500_000_000, 4_500_000_000)['status'], 'inconclusive')
        gap = [self.cpu_sample(i, i * 100, i * 100) for i in (0, 1, 4.5, 5)]
        self.assertEqual(active_runtime_cpu_evidence(gap, 1_500_000_000, 4_300_000_000)['status'], 'inconclusive')

    def test_final_host_sample_preserves_raw_lifecycle_and_brackets_active_end(self):
        rows = [self.cpu_sample(i, 200 + i * 100 if i else 0, max(0, i - 1) * 100) for i in range(6)]
        final = self.cpu_sample(6, 800, 500)
        final['memory_kib'] = {'MemAvailable': 2 * 1024 * 1024}
        row = {'finality': {'sender_start': {'protocol': 1, 'origin_ns': 2_250_000_000,
                'observation_end_ns': 5_500_000_000, 'window_fixed_once_before_sending': True, 'cpu_boundary_recorded': True}}}
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory)
            (case / 'host.jsonl').write_text(''.join(json.dumps(value) + '\n' for value in rows))
            errors = []
            with patch('parallel_release_benchmark.sample', return_value=final), \
                    patch('parallel_release_benchmark.shutil.disk_usage', return_value=SimpleNamespace(free=9 * 2**30)):
                self.assertTrue(append_runtime_end_sample(case, [1], {'reserve_bytes': 8 * 2**30}, errors))
                host = finish_runtime_cpu_evidence(row, case, [1], {'reserve_bytes': 8 * 2**30}, errors)
            self.assertEqual(host[:-1], rows)
            self.assertEqual(len(host), 7)
            self.assertFalse(errors)
            self.assertEqual(row['runtime_cpu_evidence_full_lifecycle']['status'], 'inconclusive')
            self.assertEqual(row['runtime_cpu_evidence']['status'], 'eligible')
            self.assertGreaterEqual(row['runtime_cpu_evidence']['bracket_end_ns'], 5_500_000_000)

    def test_failed_final_host_sample_is_retained_not_a_successful_partial_span(self):
        row = {'finality': {'sender_start': {'protocol': 1, 'origin_ns': 1_500_000_000,
                'observation_end_ns': 5_500_000_000, 'window_fixed_once_before_sending': True}}}
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory)
            (case / 'host.jsonl').write_text(''.join(json.dumps(self.cpu_sample(i, i * 100, i * 100)) + '\n' for i in range(4)))
            errors = []
            with patch('parallel_release_benchmark.sample', side_effect=OSError('not available')):
                self.assertFalse(append_runtime_end_sample(case, [1], {'reserve_bytes': 8 * 2**30}, errors))
                finish_runtime_cpu_evidence(row, case, [1], {'reserve_bytes': 8 * 2**30}, errors)
            self.assertTrue(errors)
            self.assertEqual(row['runtime_cpu_evidence']['status'], 'inconclusive')

    def test_runtime_end_joins_monitor_before_writing_boundary_and_never_resamples_ready_postprocessing(self):
        stop, thread = Mock(), Mock()
        events = []
        stop.set.side_effect = lambda: events.append('stop')
        thread.join.side_effect = lambda: events.append('join')
        with patch('parallel_release_benchmark.append_runtime_end_sample', side_effect=lambda *a: events.append('sample') or True):
            self.assertTrue(end_runtime_observation(stop, thread, Path('/not-opened'), [1], {}, []))
        self.assertEqual(events, ['stop', 'join', 'sample'])
        rows = [self.cpu_sample(i, i * 100, i * 100) for i in range(7)]
        row = {'finality': {'sender_start': {'protocol': 1, 'origin_ns': 1_500_000_000,
            'observation_end_ns': 5_500_000_000, 'window_fixed_once_before_sending': True, 'cpu_boundary_recorded': True}}}
        with tempfile.TemporaryDirectory() as temporary:
            case = Path(temporary); (case / 'host.jsonl').write_text(''.join(json.dumps(value) + '\n' for value in rows))
            with patch('parallel_release_benchmark.sample') as sampler:
                finish_runtime_cpu_evidence(row, case, [1], {}, [])
            sampler.assert_not_called()
            self.assertEqual(row['runtime_cpu_evidence']['status'], 'eligible')

    def test_runtime_cpu_missing_reset_reused_pid_and_internal_gaps_are_not_zero_load(self):
        import copy
        changes = [lambda s: s["processes"]["7"].update(start_ticks=4),
                   lambda s: s["processes"]["7"].update(user_ticks=-1),
                   lambda s: s.update(at_ns=0), lambda s: s.update(at_ns=4_000_000_000),
                   lambda s: s.pop("clock_ticks_per_second"),
                   lambda s: s.update(cpu_ticks=[-1] * 10)]
        first, last = self.cpu_sample(0, 0, 0), self.cpu_sample(1, 120, 100)
        for change in changes:
            bad = copy.deepcopy(last); change(bad)
            self.assertEqual(runtime_cpu_evidence([first, bad])["status"], "inconclusive")
        missing = {"at_ns": 1_000_000_000, "processes": {"7": {"unavailable": True}}}
        self.assertEqual(runtime_cpu_evidence([first, missing, self.cpu_sample(2, 240, 200)])["status"], "inconclusive")
        self.assertEqual(runtime_cpu_evidence([missing, first, last, missing])["status"], "eligible")
        self.assertEqual(runtime_cpu_evidence([first])["status"], "inconclusive")

    def test_noisy_or_missing_runtime_evidence_cannot_certify_gain(self):
        rows = [self.row(0, 10) for _ in range(3)] + [self.row(4, 20) for _ in range(3)]
        rows[0]["runtime_cpu_evidence"] = {"status": "inconclusive"}
        result = summarize(rows, [0, 4], 3)
        self.assertTrue(result["available"])  # raw comparison retained
        self.assertEqual(result["modes"]["4"]["median_tps"], 20)
        self.assertEqual(result["performance_evidence_status"], "inconclusive")
        self.assertFalse(result["performance_goal_met"])
        rows[0].pop("runtime_cpu_evidence")
        self.assertFalse(summarize(rows, [0, 4], 3)["performance_goal_met"])

    def test_median_and_negative_gain_keep_every_round(self):
        rows = [self.row(0, v) for v in (10, 20, 30)] + [self.row(4, v) for v in (5, 10, 15)]
        out = summarize(rows, [0, 4], 3)
        self.assertEqual(out["modes"]["4"]["ratio_vs_serial_jit"], .5)
        self.assertEqual(out["modes"]["4"]["improvement_percent"], -50)
        self.assertEqual(out["modes"]["4"]["min_tps"], 5)
        self.assertEqual(out["modes"]["4"]["max_tps"], 15)

    def test_one_invalid_round_invalidates_comparison_not_removed(self):
        rows = [self.row(0, 10), self.row(0, 10), self.row(4, 20), self.row(4, 99, False)]
        self.assertFalse(summarize(rows, [0, 4], 2)["available"])

    def test_missing_round_is_not_completion(self):
        self.assertFalse(summarize([self.row(0, 10), self.row(4, 20)], [0, 4], 3)["available"])

    def test_zero_baseline_cannot_claim_infinite_gain(self):
        rows=[self.row(0, 0), self.row(4, 20)]
        result=summarize(rows,[0,4],1)
        self.assertFalse(result['available']);self.assertFalse(result['performance_goal_met'])
        self.assertEqual(result['reason_code'],'zero-serial-finality-window')
        self.assertEqual(result['modes']['4']['median_tps'],20)
        self.assertNotIn('ratio_vs_serial_jit',result['modes']['4'])
        self.assertEqual(comparison_status(result),'inconclusive')
        self.assertEqual([r['finality']['finalized_success_tps'] for r in rows],[0,20])

    def test_both_zero_windows_are_retained_without_inventing_throughput(self):
        result=summarize([self.row(0,0),self.row(4,0)],[0,4],1)
        self.assertEqual(result['modes']['0']['max_tps'],0)
        self.assertEqual(result['modes']['4']['max_tps'],0)
        self.assertFalse(result['performance_goal_met'])

    def test_noisy_comparison_is_not_a_passing_benchmark(self):
        rows=[self.row(0,10) for _ in range(3)]+[self.row(4,20) for _ in range(3)]
        self.assertEqual(comparison_status(summarize(rows,[0,4],3)),'passed')
        rows[0]['runtime_cpu_evidence']={'status':'inconclusive'}
        result=summarize(rows,[0,4],3)
        self.assertTrue(result['available']);self.assertFalse(result['performance_goal_met'])
        self.assertEqual(comparison_status(result),'inconclusive')

    def test_diagnostic_capacity_instrumentation_or_failed_closure_never_certify_release_performance(self):
        comparison={'available':True,'performance_evidence_status':'eligible','performance_goal_met':True}
        context=dict(report_status='passed',capacity_diagnostic=False,instrumentation_enabled=False)
        self.assertTrue(performance_release_eligible(comparison,**context))
        for key,value in (('report_status','failed'),('report_status','inconclusive'),
                          ('capacity_diagnostic',True),('instrumentation_enabled',True)):
            self.assertFalse(performance_release_eligible(comparison,**{**context,key:value}))
        for key,value in (('available',False),('performance_evidence_status','inconclusive'),('performance_goal_met',False)):
            self.assertFalse(performance_release_eligible({**comparison,key:value},**context))
        self.assertFalse(performance_release_eligible({},**context))

    def test_only_repeatable_gain_meets_the_performance_goal(self):
        rows = [self.row(0, v) for v in (10, 10, 10)] + [self.row(4, v) for v in (13, 14, 15)]
        self.assertTrue(summarize(rows, [0, 4], 3)["performance_goal_met"])
        overlap = [self.row(0, v) for v in (10, 20, 30)] + [self.row(4, v) for v in (15, 40, 50)]
        self.assertFalse(summarize(overlap, [0, 4], 3)["performance_goal_met"])
        self.assertFalse(summarize([self.row(0, 10), self.row(4, 20)], [0, 4], 1)["performance_goal_met"])

    def test_finality_ignores_orphaned_and_reversible_and_failed_receipts(self):
        events = [
            {"type": "anchor", "id": "a"},
            {"type": "block", "id": "old", "previous": "a", "receipts": [{"id": "orphan", "status": "executed"}]},
            {"type": "block", "id": "b", "previous": "a", "receipts": [
                {"id": "good", "status": "executed"}, {"id": "bad", "status": "hard_fail"}]},
            {"type": "block", "id": "c", "previous": "b", "receipts": [{"id": "pending", "status": "executed"}]},
            {"type": "sample", "head_id": "old", "lib_id": "a"},
            {"type": "sample", "head_id": "c", "lib_id": "b"}]
        self.assertEqual(final_ids(events), {"good"})

    def test_incomplete_finality_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Incomplete final branch"):
            final_ids([{"type": "anchor", "id": "a"}, {"type": "sample", "lib_id": "missing"}])

    def test_ancestry_cycle_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Incomplete final branch"):
            final_ids([{"type": "anchor", "id": "a"},
                       {"type": "block", "id": "b", "previous": "b", "receipts": []},
                       {"type": "sample", "lib_id": "b"}])

    def test_head_check_counts_successful_receipts_and_validates_id(self):
        class Fake:
            def rpc(self, method, payload):
                return {"id": "b", "previous": "a", "transactions": [
                    {"status": "executed", "trx": {"id": "x"}},
                    {"status": "expired", "trx": "ignored"}]}
        self.assertEqual(head_business_ids(Fake(), "b", "a"), {"x"})
        with self.assertRaisesRegex(RuntimeError, "Wrong block"):
            head_business_ids(Fake(), "wrong", "a")

    def test_node_configuration_uses_same_vm_and_explicit_batch(self):
        for engine, vm in (("interpreter", "eos-vm"), ("jit", "eos-vm-jit")):
            with tempfile.TemporaryDirectory() as temporary, patch.object(Node, "free_port", side_effect=[12341, 12342]):
                node = Node(Path("/not-started"), Path(temporary) / "node", Path("/not-read"),
                            {"code_hashes": ["0" * 64]}, 4, engine=engine, batch_size=32)
                text = (node.root / "config/config.ini").read_text()
                self.assertIn("wasm-runtime = " + vm + "\n", text)
                self.assertIn("experimental-parallel-validation-batch-size = 32\n", text)
                self.assertEqual("experimental-parallel-engine = jit\n" in text, engine == "jit")

    def test_receipts_report_actual_genesis_limits_and_do_not_claim_full_resource_bill(self):
        rows = [{"transactions": 2, "receipt_cpu_us": 150, "receipt_net_bytes": 64},
                {"transactions": 0, "receipt_cpu_us": 0, "receipt_net_bytes": 0}]
        result = receipt_capacity(rows, {"max_block_cpu_usage": 200, "max_block_net_usage": 128})
        self.assertEqual(result["max_receipt_cpu_fraction"], .75)
        self.assertEqual(result["max_receipt_net_fraction"], .5)
        self.assertEqual(result["mean_receipt_cpu_us_per_transaction"], 75)
        self.assertIn("excluding implicit", result["scope"])
        self.assertIsNone(receipt_capacity([], {"max_block_cpu_usage": 200,
                                               "max_block_net_usage": 128})["mean_receipt_cpu_us_per_transaction"])
        with self.assertRaisesRegex(RuntimeError, "Invalid genesis"):
            receipt_capacity(rows, {"max_block_cpu_usage": 0, "max_block_net_usage": 128})

    def test_cpu_saturation_reports_frequency_not_just_one_peak(self):
        rows = [{"transactions": 2, "receipt_cpu_us": 199, "receipt_net_bytes": 0},
                {"transactions": 1, "receipt_cpu_us": 100, "receipt_net_bytes": 0},
                {"transactions": 0, "receipt_cpu_us": 0, "receipt_net_bytes": 0}]
        cfg = {"max_block_cpu_usage": 200, "max_block_net_usage": 128}
        result = receipt_capacity(rows, cfg)
        self.assertEqual(result["blocks"], 3)
        self.assertEqual(result["nonempty_blocks"], 2)
        self.assertEqual(result["nonempty_blocks_at_least_95_percent_receipt_cpu"], 1)
        self.assertEqual(result["fraction_nonempty_blocks_at_least_95_percent_receipt_cpu"], .5)
        self.assertEqual(result["mean_nonempty_receipt_cpu_fraction"], 299 / 400)
        empty = receipt_capacity(rows[-1:], cfg)
        self.assertEqual(empty["nonempty_blocks"], 0)
        self.assertIsNone(empty["fraction_nonempty_blocks_at_least_95_percent_receipt_cpu"])
        self.assertIsNone(empty["mean_nonempty_receipt_cpu_fraction"])

    def test_startup_rejects_an_unrelated_chain_before_any_mutating_rpc(self):
        with tempfile.TemporaryDirectory() as temporary, patch.object(Node, "free_port", side_effect=[12341, 12342]):
            node = Node(Path("/not-started"), Path(temporary) / "node", Path("/not-read"),
                        {"code_hashes": [], "chain_id": "expected"})
            process = Mock()
            process.poll.return_value = None
            with patch("parallel_execution_test.subprocess.Popen", return_value=process), \
                    patch.object(node, "rpc", return_value={"chain_id": "unrelated"}) as rpc:
                try:
                    with self.assertRaisesRegex(RuntimeError, "another chain"):
                        node.start("fake-start")
                    rpc.assert_called_once_with("get_info")
                finally:
                    node.stop(check=False)

    def test_paused_checkpoint_uses_integrity_head_not_a_stale_info_cache(self):
        node = Mock()
        state = {"head_block_id": "b", "integrity_hash": "hash-b"}
        node.rpc.side_effect = [state, {"head_block_id": "a"}, {"head_block_id": "b"}]
        with patch("parallel_release_benchmark.time.sleep"):
            info, pinned = pin_paused_head(node)
        self.assertEqual(info["head_block_id"], pinned["head_block_id"])
        self.assertEqual(pinned, state)
        self.assertEqual(node.rpc.call_args_list[0].args, ("get_integrity_hash",))
        self.assertEqual(node.rpc.call_args_list[0].kwargs, {"namespace": "producer"})

    def test_quiet_gate_retries_only_busy_and_retains_every_preflight(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch("benchmark_environment.subprocess.run", side_effect=[Mock(returncode=75), Mock(returncode=0)]), \
                patch("benchmark_environment.time.sleep") as pause:
            root = Path(temporary) / "gate"
            result = wait_for_quiet_gate(Path("/not-executed"), root)
            self.assertEqual(result, {"attempts": 2, "prior_busy_checks": 1})
            self.assertEqual(len(list(root.iterdir())), 2)
            pause.assert_called_once_with(5)

    def test_quiet_gate_does_not_retry_other_failures_or_wait_without_bound(self):
        for code, maximum, expected_calls in ((2, 6, 1), (75, 2, 2)):
            with tempfile.TemporaryDirectory() as temporary, \
                    patch("benchmark_environment.subprocess.run", return_value=Mock(returncode=code)) as run, \
                    patch("benchmark_environment.time.sleep"):
                with self.assertRaisesRegex(RuntimeError, "Quiet gate"):
                    wait_for_quiet_gate(Path("/not-executed"), Path(temporary) / "gate", maximum)
                self.assertEqual(run.call_count, expected_calls)
        with self.assertRaises(ValueError):
            wait_for_quiet_gate(Path("/not-executed"), Path("/must-not-create"), 7)


if __name__ == "__main__":
    unittest.main()
