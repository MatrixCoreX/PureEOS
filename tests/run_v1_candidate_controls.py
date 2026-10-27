"""Three fixed rounds of offline candidate controls, not chain/TPS evidence."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import unittest
import warnings

MODULES = (
    'pureeos_multinode_shutdown_test',
    'pureeos_candidate_sync_test','pureeos_rotation_cohort_cloud_test_test',
    'pureeos_fourhost_rotation_cloud_test_test','pureeos_multihost_cloud_test_test','v1_twohost_cohort_test_test',
    'v1_cloud_host_power_test_test',
    'pureeos_rotation_lifecycle_test','pureeos_host_metrics_test',
    'pureeos_historical_edge_cases_unit_test','parallel_loopback_wave_test',
    'ubuntu_cdt_runtime_test_test','parallel_release_benchmark_test',
    'parallel_mixed_stress_workload_test','parallel_complex_replay_test',
    'parallel_release_recovery_test','parallel_execution_harness_test',
    'parallel_pressure_metrics_test','parallel_saturation_metrics_test',
    'presigned_sender_start_unit_test','parallel_access_audit_test',
    'native_policy_fixture_registration_test','v1_candidate_deployment_test_test',
    'v1_candidate_native_matrix_test','v1_candidate_recovery_test_test',
    'pureeos_committed_policy_observer_test','archive_v1_candidate_sources_test',
    'audit_v1_native_shards_test', 'v1_local_cohort_release_test_test', 'v1_local_release_workflows_test',
    'v1_local_retest_metrics_test', 'v1_live_reward_claim_test_test', 'prepare_v1_engine_profile_retest_test',
    'v1_offline_portability_test', 'prepare_v1_native_profile_retest_test',
    'run_v1_resource_boundary_probe_test', 'v1_vendor_source_test', 'ubuntu_modern_build_test', 'ci_delivery_test',
    'workflow_inventory_test', 'portable_build_cache_test', 'parallel_sanitizer_options_test',
    'v1_build_version_test', 'v1_fast_regression_test', 'v1_early_live_reward_claim_host_test',
    'prepare_v1_same_build_resume_test','v1_runtime_dependencies_test','package_v1_local_source_test',
    'package_v1_candidate_runtime_test')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    args.output.mkdir(exist_ok=False);warnings.simplefilter('error',ResourceWarning)
    directory=Path(__file__).resolve().parent
    names=set(MODULES)|{'v1_candidate_deployment_test','v1_candidate_native_matrix',
        'v1_candidate_recovery_test','archive_v1_candidate_sources','audit_v1_native_shards',
        'run_v1_candidate_controls','pureeos_committed_policy_observer','pureeos_rotation_lifecycle',
        'pureeos_rotation_cohort_cloud_test','pureeos_large_multinode_local_test',
        'pureeos_historical_edge_cases_test','pureeos_single_choice_local_test',
        'pureeos_multinode_local_test','v1_twohost_cohort_test','v1_cloud_host_power_test','pureeos_multihost_cloud_test',
        'v1_local_cohort_release_test','v1_local_release_workflows','v1_local_retest_metrics',
        'v1_live_reward_claim_test','v1_live_reward_claim_host', 'prepare_v1_engine_profile_retest',
        'v1_engine_profile_retest_host', 'v1_engine_profile_retest_guest',
        'prepare_v1_native_profile_retest', 'v1_native_profile_retest_guest', 'v1_native_profile_retest_host',
        'v1_native_profile_metrics', 'run_v1_resource_boundary_probe',
        'v1_fast_regression', 'v1_early_live_reward_claim_host', 'prepare_v1_same_build_resume',
        'run_v1_candidate_sdk', 'start_v1_same_build_resume', 'v1_runtime_dependencies', 'package_v1_local_source',
        'package_v1_candidate_runtime'}
    files=[directory/(n+'.py') for n in sorted(names)]
    files += [directory.parent/'libraries/eos-vm/include/eosio/vm'/n
              for n in ('allocator.hpp','parser.hpp','x86_64.hpp')]
    files += [directory.parent/'cmake/ParallelSanitizerOptions.cmake', directory.parent/'unittests/CMakeLists.txt', directory.parent/'scripts/parallel_sanitizer_check.py']
    files += [directory.parent/'libraries/chain/resource_limits.cpp',
              directory.parent/'unittests/resource_limits_test.cpp']
    files += [directory.parent/'tools'/name for name in
              ('prepare_vm_vendor.py', 'ubuntu-modern-build.sh', 'ci_native_checks.py', 'workflow_inventory.py', 'portable_build_cache.py',
               'vendor/eos-vm-parallel-v1.json', 'vendor/eos-vm-parallel-v1.patch')]
    files += [directory.parent/'.github/workflows'/name for name in ('build.yaml', 'build_base.yaml')]
    files += sorted((directory.parent/'docs/parallel-execution/ci-reference-20261024').glob('*'))
    files += [directory.parent/name for name in ('.gitmodules','CMakeLists.txt','libraries/version/CMakeLists.txt',
              'libraries/version/include/eosio/version/version.hpp','libraries/version/src/version_impl.cpp.in')]
    def sha(p):
        with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
    report=dict(status='running',scope=__doc__,modules=MODULES,rounds=[],
                inputs_sha256={str(p):sha(p) for p in files})
    for n in range(1,4):
        with (args.output/f'round-{n}.log').open('x') as stream:
            result=unittest.TextTestRunner(stream=stream,verbosity=2).run(
                unittest.defaultTestLoader.loadTestsFromNames(MODULES))
        report['rounds'].append(dict(round=n,tests=result.testsRun,failures=len(result.failures),
                                    errors=len(result.errors),skipped=len(result.skipped),passed=result.wasSuccessful()))
        if not result.wasSuccessful():break
    unchanged=all(sha(p)==report['inputs_sha256'][str(p)] for p in files)
    report.update(status='passed' if unchanged and len(report['rounds'])==3 and
        all(r['passed'] for r in report['rounds']) else 'failed',inputs_unchanged=unchanged,
        finished_utc=datetime.now(timezone.utc).isoformat())
    (args.output/'report.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:report[k] for k in ('status','rounds','inputs_unchanged')}))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
