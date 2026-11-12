"""Offline smoke-profile/negative controls, separate from actual binary evidence."""
from copy import deepcopy
import ast
import inspect
from pathlib import Path
import json
import unittest
from unittest.mock import patch

from v1_fast_regression import (CONTROLLER, IMAGE, INTERNAL_MATRIX, NATIVE, PROGRAMS, RESOURCE, RUNTIME_EXPORTER, TESTS,
                                WORKER, check_parent, inspect_owned, native_cases,
                                host, runtime_complete, success_status, timing_cases, verify_boost)


def fixture():
    identity = 'a' * 64
    return dict(status='running', source_identity_sha256=identity,
        jobs=[dict(name=name, exit_code=0) for name in
              ('configure', 'build-node-controller', 'build-version-provenance')],
        binaries_sha256={**{'programs/' + p + '/' + p: 'b' * 64 for p in PROGRAMS},
                         **{'unittests/' + p: 'b' * 64 for p in TESTS}},
        build_version_provenance=dict(verified=True, client='v2.0.0',
            full='v2.0.0-src-' + identity, source_manifest_sha256=identity))


def boost_text(names):
    return ('Test module "Master Test Suite" has passed with:\n' + ''.join(
        '  Test case "' + name + '" has passed with:\n    1 assertion out of 1 passed\n'
        for name in names))


class FastRegressionControls(unittest.TestCase):
    def test_cdt_export_fixture_is_owned_by_validation_not_worker_binary(self):
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(RUNTIME_EXPORTER, 'parallel_validation_test')
        self.assertIn('#include "parallel_release_fixture.hpp"',
                      (root / 'unittests/parallel_validation_tests.cpp').read_text())
        self.assertNotIn('#include "parallel_release_fixture.hpp"',
                         (root / 'unittests/parallel_wasm_worker_tests.cpp').read_text())

    def test_container_identity_has_one_binding_and_cannot_be_an_artifact_loop_variable(self):
        tree = ast.parse(inspect.getsource(host))
        stores = [n for n in ast.walk(tree) if isinstance(n, ast.Name)
                  and n.id == 'container_name' and isinstance(n.ctx, ast.Store)]
        self.assertEqual(len(stores), 1)
        checks = [n for n in ast.walk(tree) if isinstance(n, ast.Call)
                  and isinstance(n.func, ast.Name) and n.func.id == 'inspect_owned']
        self.assertEqual(len(checks), 3)
        self.assertTrue(all(isinstance(c.args[0], ast.Name) and c.args[0].id == 'container_name' for c in checks))

    def test_profiles_are_explicit_and_cannot_certify_release(self):
        self.assertEqual(success_status('smoke'), 'smoke-passed-not-release')
        self.assertEqual(success_status('runtime'), 'runtime-preflight-passed-not-release')
        with self.assertRaises(AssertionError): success_status('full-release')

    def test_runtime_requires_cli_all_cases_and_actual_production_and_validation_adoption(self):
        cli = dict(status='passed', cases=[dict(status='passed') for _ in range(8)],
                   reported_limits=dict(workers=16, batch_transactions=64))
        sdk = dict(status='passed', cases=[dict(workers=n, passed=True,
            sdk_rejection_partial_writes_rolled_back=True, finalized_transactions=45,
            production_attempts_including_aborted_blocks=[dict(adopted=n)],
            validator_attempts_including_aborted_blocks=[dict(adopted=n)]) for n in (0, 4)])
        self.assertTrue(runtime_complete(cli, sdk))
        for mode in ('missing', 'failed', 'no-production', 'no-validation', 'no-rollback', 'no-finality'):
            broken = deepcopy(sdk)
            if mode == 'missing': broken['cases'].pop()
            if mode == 'failed': broken['cases'][-1]['passed'] = False
            if mode == 'no-production': broken['cases'][-1]['production_attempts_including_aborted_blocks'] = []
            if mode == 'no-validation': broken['cases'][-1]['validator_attempts_including_aborted_blocks'] = []
            if mode == 'no-rollback': broken['cases'][-1]['sdk_rejection_partial_writes_rolled_back'] = False
            if mode == 'no-finality': broken['cases'][-1]['finalized_transactions'] = 0
            self.assertFalse(runtime_complete(cli, broken))
        cli['cases'].pop(); self.assertFalse(runtime_complete(cli, sdk))

    def test_built_running_candidate_is_only_smoke_not_a_release_gate(self):
        check_parent(fixture(), 'a' * 64)
        report = fixture(); report['status'] = 'passed'
        check_parent(report, 'a' * 64)

    def test_failed_or_incomplete_candidate_refused(self):
        for state in ('failed', 'superseded', 'waiting-for-core'):
            report = fixture(); report['status'] = state
            with self.assertRaises(AssertionError): check_parent(report, 'a' * 64)
        for index in range(3):
            report = fixture(); report['jobs'].pop(index)
            with self.assertRaises(AssertionError): check_parent(report, 'a' * 64)

    def test_any_failed_duplicate_or_unfinished_job_refused(self):
        for mutation in ('failed', 'duplicate', 'unfinished'):
            report = fixture()
            if mutation == 'failed': report['jobs'][0]['exit_code'] = 1
            if mutation == 'duplicate': report['jobs'].append(deepcopy(report['jobs'][0]))
            if mutation == 'unfinished': report['jobs'].append(dict(name='native-profile-preflight'))
            with self.assertRaises(AssertionError): check_parent(report, 'a' * 64)

    def test_missing_extra_and_non_hash_binaries_refused(self):
        for mutation in ('missing', 'extra', 'bad-hash'):
            report = fixture()
            if mutation == 'missing': report['binaries_sha256'].pop('unittests/native_savanna_test')
            if mutation == 'extra': report['binaries_sha256']['../other'] = 'b' * 64
            if mutation == 'bad-hash': report['binaries_sha256']['unittests/native_savanna_test'] = 'x' * 64
            with self.assertRaises(AssertionError): check_parent(report, 'a' * 64)

    def test_source_and_real_version_must_match(self):
        for field, value in (('client', 'unknown'), ('verified', False),
                             ('full', 'v2.0.0-src-' + 'c' * 64),
                             ('source_manifest_sha256', 'c' * 64)):
            report = fixture(); report['build_version_provenance'][field] = value
            with self.assertRaises(AssertionError): check_parent(report, 'a' * 64)
        with self.assertRaises(AssertionError): check_parent(fixture(), 'c' * 64)

    def test_profile_is_explicit_unique_and_covers_policy_and_cpu_boundaries(self):
        for cases in (native_cases(), WORKER, CONTROLLER, INTERNAL_MATRIX, RESOURCE):
            self.assertEqual(len(cases), len(set(cases)))
            self.assertTrue(all('*' not in c and ':' not in c for c in cases))
        self.assertEqual(len(RESOURCE), 15)
        self.assertEqual(len(NATIVE), 9)
        self.assertIn('pureeos_governance_tests/review_period_is_fixed_at_fourteen_days', native_cases())
        self.assertTrue(any('four_year_boundary' in name for name in native_cases()))
        self.assertTrue(any('zero_block' in name for name in native_cases()))
        self.assertTrue(any('under21' in name for name in native_cases()))
        self.assertTrue(any('six_finalized_bans' in name for name in native_cases()))
        self.assertTrue(any('shared_balances_fall_back' in name for name in CONTROLLER))
        self.assertTrue(any('cannot_renew_the_production_private_deadline' in name for name in INTERNAL_MATRIX))
        self.assertTrue(any('original_inline_authority' in name for name in WORKER))

    def test_internal_engine_thread_matrix_is_not_repeated_by_outer_engine(self):
        self.assertFalse(set(INTERNAL_MATRIX) & set(CONTROLLER))
        self.assertEqual(len(INTERNAL_MATRIX), 3)

    def test_boost_full_exact_case_evidence_required(self):
        self.assertEqual(verify_boost(boost_text(('s/a', 's/b')), ('s/a', 's/b')), 2)
        for names in (('s/a',), ('s/a', 's/b', 's/c'), ('s/a', 's/a', 's/b')):
            with self.assertRaises(AssertionError): verify_boost(boost_text(names), ('s/a', 's/b'))

    def test_boost_truncated_failed_and_nonzero_refused(self):
        text = boost_text(('s/a',))
        with self.assertRaises(AssertionError): verify_boost(text, ('s/a',), 201)
        with self.assertRaises(AssertionError): verify_boost(text.split('\n', 1)[1], ('s/a',))
        with self.assertRaises(AssertionError):
            verify_boost(text + '\n  1 assertion out of 2 failed\n', ('s/a',))

    def test_timing_parser_preserves_suite_and_microseconds(self):
        text = ('Entering test suite "s"\nEntering test suite "nested"\n'
                'Leaving test case "x"; testing time: 1234567us\n'
                'Leaving test suite "nested"\nLeaving test suite "s"\n')
        self.assertEqual(timing_cases(text), [dict(case='s/nested/x', seconds=1.234567)])
        with self.assertRaises(AssertionError): timing_cases('Leaving test suite "foreign"')

    @patch('v1_fast_regression.subprocess.run')
    def test_container_identity_must_match_before_any_cleanup(self, run):
        run.return_value.returncode = 0
        for image, name in ((IMAGE, 'other'), ('other', 'expected')):
            run.return_value.stdout = json.dumps([dict(Image=image, Config=dict(Labels={'org.pureeos.task': name}))])
            with self.assertRaises(AssertionError): inspect_owned('expected')
        run.return_value.stdout = json.dumps([dict(Image=IMAGE, Config=dict(Labels={'org.pureeos.task': 'expected'}))])
        self.assertTrue(inspect_owned('expected'))

    @patch('v1_fast_regression.subprocess.run')
    def test_only_exact_absence_not_daemon_error_counts_as_absent(self, run):
        run.return_value.returncode = 1
        for error in ('permission denied', 'Cannot connect to Docker daemon', 'No such container: other'):
            run.return_value.stderr = error
            with self.assertRaises(AssertionError): inspect_owned('expected')
        run.return_value.stderr = 'No such container: expected'
        self.assertFalse(inspect_owned('expected'))


if __name__ == '__main__': unittest.main()
