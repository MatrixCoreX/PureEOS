"""Early scheduling cannot replace completed full-core acceptance."""
from copy import deepcopy
import unittest

from v1_early_live_reward_claim_host import early_ready, final_core_ready, fixture_preflight_ready, initial_report_ready
from v1_runtime_dependencies import FULL_CDT_CASES
from v1_fast_regression import CONTROLLER, INTERNAL_MATRIX, RESOURCE, WORKER, native_cases
from v1_fast_regression_test import fixture
from v1_local_release_workflows_test import core_fixture


def early_fixture():
    core = fixture()
    core['runtime_dependencies_verified']=107
    core['full_cdt_fixture_preflight']=dict(status='passed',exit_code=0,cases=list(FULL_CDT_CASES))
    core['jobs'] += [dict(name=name, exit_code=0) for name in
        ('native-profile-preflight', 'full-native-matrix-round-1',
         'resource-limits-round-1', 'resource-limits-round-2', 'resource-limits-round-3')]
    counts = {'resource-limits': len(RESOURCE), 'native-policy': len(native_cases()),
        'controller-internal-matrix': len(INTERNAL_MATRIX),
        **{'worker-' + e: len(WORKER) for e in ('eos-vm', 'eos-vm-jit')},
        **{'controller-' + e: len(CONTROLLER) for e in ('eos-vm', 'eos-vm-jit')}}
    jobs = [dict(name=name, exit_code=0, cases_passed=count) for name, count in counts.items()]
    jobs += [dict(name=name, exit_code=0) for name in
        ('version-nodeos', 'version-cleos', 'version-keosd', 'parallel_access_hints_test',
         'parallel_worker_signals_test', 'parallel_immutable_input_test')]
    return (core, dict(status='smoke-passed-not-release', release_ready=False, full_regression=False,
        source_identity_sha256='a' * 64, inputs_unchanged=True, jobs=jobs,
        boost_case_executions=sum(counts.values())),
        dict(source_identity_sha256='a' * 64, binaries_sha256=deepcopy(core['binaries_sha256'])))


class EarlyClaimControls(unittest.TestCase):
    def test_report_creation_waits_only_for_a_live_core_unit(self):
        for state in ('active','activating'):
            self.assertFalse(initial_report_ready(False,state))
        for state in ('failed','inactive','deactivating','unknown'):
            with self.assertRaises(AssertionError):initial_report_ready(False,state)
            self.assertTrue(initial_report_ready(True,state))
    def test_fixture_preflight_missing_partial_failed_or_unidentified_core_cannot_start(self):
        for mode in ('missing','partial','failed','nonzero','manifest','version'):
            core,smoke,plan=early_fixture()
            if mode=='missing':core.pop('full_cdt_fixture_preflight')
            if mode=='partial':core['full_cdt_fixture_preflight']['cases'].pop()
            if mode=='failed':core['full_cdt_fixture_preflight']['status']='failed'
            if mode=='nonzero':core['full_cdt_fixture_preflight']['exit_code']=201
            if mode=='manifest':core['runtime_dependencies_verified']=0
            if mode=='version':core['build_version_provenance']['verified']=False
            self.assertFalse(fixture_preflight_ready(core))
            with self.assertRaises(AssertionError):early_ready(core,smoke,plan)
    def test_first_business_round_and_completed_smoke_allow_only_early_start(self):
        self.assertTrue(early_ready(*early_fixture()))

    def test_smoke_not_a_full_core_acceptance(self):
        core, smoke, plan = early_fixture()
        self.assertFalse(final_core_ready(core, dict(status='passed'), dict(status='running'),
                                         'a' * 64, plan['binaries_sha256']))

    def test_missing_native_round_or_failed_job_refuses_start(self):
        for name in ('full-native-matrix-round-1', 'native-profile-preflight', 'resource-limits-round-3'):
            core, smoke, plan = early_fixture()
            core['jobs'] = [j for j in core['jobs'] if j['name'] != name]
            with self.assertRaises(AssertionError): early_ready(core, smoke, plan)
        core, smoke, plan = early_fixture(); core['jobs'][0]['exit_code'] = 1
        with self.assertRaises(AssertionError): early_ready(core, smoke, plan)

    def test_running_failed_or_partial_smoke_cannot_start_real_day(self):
        for field, value in (('status', 'running'), ('status', 'failed'), ('inputs_unchanged', False),
                             ('release_ready', True), ('full_regression', True),
                             ('boost_case_executions', 0)):
            core, smoke, plan = early_fixture(); smoke[field] = value
            with self.assertRaises(AssertionError): early_ready(core, smoke, plan)

    def test_missing_duplicate_wrong_count_or_failed_smoke_case_refuses(self):
        for mode in ('missing', 'duplicate', 'wrong-count', 'failed'):
            core, smoke, plan = early_fixture()
            if mode == 'missing': smoke['jobs'].pop()
            if mode == 'duplicate': smoke['jobs'][-1] = deepcopy(smoke['jobs'][0])
            if mode == 'wrong-count': smoke['jobs'][0]['cases_passed'] -= 1
            if mode == 'failed': smoke['jobs'][0]['exit_code'] = 1
            with self.assertRaises(AssertionError): early_ready(core, smoke, plan)

    def test_different_candidate_or_source_cannot_reuse_smoke(self):
        for mode in ('source', 'binary'):
            core, smoke, plan = early_fixture()
            if mode == 'source': plan['source_identity_sha256'] = 'c' * 64
            if mode == 'binary': plan['binaries_sha256']['unittests/native_savanna_test'] = 'c' * 64
            with self.assertRaises(AssertionError): early_ready(core, smoke, plan)

    def test_final_gate_requires_all_59_jobs_exact_source_and_binaries(self):
        report, proof, monitor = core_fixture()
        binaries = deepcopy(report['binaries_sha256'])
        self.assertTrue(final_core_ready(report, proof, monitor, 'a' * 64, binaries))
        self.assertFalse(final_core_ready(report, proof, monitor, 'b' * 64, binaries))
        self.assertFalse(final_core_ready(report, proof, monitor, 'a' * 64, {}))
        report['jobs'].pop()
        self.assertFalse(final_core_ready(report, proof, monitor, 'a' * 64, binaries))

    def test_closed_failed_full_candidate_invalidates_provisional_claims(self):
        report, proof, monitor = core_fixture()
        for value in (report, proof, monitor):
            broken = deepcopy(value)
            if value is report: broken['status'] = 'failed'
            elif value is proof: broken['candidate_test_status'] = 'failed'
            else: broken['candidate_status'] = 'failed'
            values = (broken, proof, monitor) if value is report else (
                (report, broken, monitor) if value is proof else (report, proof, broken))
            self.assertFalse(final_core_ready(*values, 'a' * 64, report['binaries_sha256']))


if __name__ == '__main__': unittest.main()
