"""Source-only checkpoint; explicit allowlist excludes user config, wallets and keys."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import tarfile


def main():
    source=Path(__file__).resolve().parents[1]
    cdt=source.parent/'cdt-v1-hardening'
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/phase2-source-checkpoint'))
    parser.add_argument('--include-safe-eviction', action='store_true')
    args=parser.parse_args()
    output=args.output
    output.mkdir(exist_ok=False)
    files={}
    node_files=['LICENSE','AIC_LICENSE.md','LICENSING.md','README.md',
        'tests/pureeos_multinode_local_test.py','tests/pureeos_multinode_shutdown_test.py',
        'libraries/chain/controller.cpp','libraries/chain/parallel_worker_pool.cpp',
        'libraries/chain/resource_limits.cpp','unittests/resource_limits_test.cpp',
        'tests/run_v1_resource_boundary_probe.py','tests/run_v1_resource_boundary_probe_test.py',
        'tools/prepare_vm_vendor.py', 'tools/ubuntu-modern-build.sh',
        'tools/vendor/eos-vm-parallel-v1.json', 'tools/vendor/eos-vm-parallel-v1.patch',
        'tests/v1_vendor_source_test.py', 'tests/ubuntu_modern_build_test.py',
        '.github/workflows/build.yaml', '.github/workflows/build_base.yaml',
        'CMakeLists.txt','libraries/version/CMakeLists.txt','libraries/version/include/eosio/version/version.hpp',
        'libraries/version/src/version_impl.cpp.in',
        'tests/v1_build_version_test.py',
        'tests/v1_fast_regression.py', 'tests/v1_fast_regression_test.py',
        'tests/v1_early_live_reward_claim_host.py', 'tests/v1_early_live_reward_claim_host_test.py',
        'tests/prepare_v1_same_build_resume.py', 'tests/prepare_v1_same_build_resume_test.py',
        'tests/start_v1_same_build_resume.py',
        'tests/v1_runtime_dependencies.py','tests/v1_runtime_dependencies_test.py',
        'tests/package_v1_local_source.py','tests/package_v1_local_source_test.py',
        'tests/package_v1_candidate_runtime.py','tests/package_v1_candidate_runtime_test.py',
        'libraries/chain/include/eosio/chain/parallel_access_hints.hpp',
        'libraries/chain/include/eosio/chain/parallel_immutable_input.hpp',
        'libraries/chain/include/eosio/chain/parallel_worker_pool.hpp',
        'libraries/chain/include/eosio/chain/parallel_worker_signals.hpp','unittests/CMakeLists.txt',
        'unittests/parallel_prefix_refresh_cases.hpp','unittests/pureeos_reward_halving_tests.inc',
        'unittests/parallel_validation_tests.cpp',
        'unittests/parallel_sync_call_tests.cpp', 'unittests/parallel_wasm_worker_tests.cpp',
        'unittests/parallel_console_host_tests.cpp', 'unittests/parallel_test_runtime.hpp',
        'unittests/parallel_context_free_tests.cpp','unittests/parallel_integer_host_tests.cpp',
        'unittests/parallel_float_host_tests.cpp','unittests/parallel_pure_host_tests.cpp',
        'unittests/parallel_secondary_wasm_tests.cpp','unittests/pureeos_genesis_tests.cpp',
        'unittests/parallel_action_tree_pool_tests.cpp', 'unittests/parallel_mixed_tree_tests.cpp',
        'unittests/pureeos_blacklist_current_policy_tests.inc','tests/pureeos_large_multinode_local_test.py',
        'tests/pureeos_single_choice_local_test.py','tests/pureeos_rotation_cohort_cloud_test.py',
        'tests/pureeos_rotation_lifecycle.py','tests/pureeos_rotation_lifecycle_test.py',
        'tests/pureeos_committed_policy_observer.py','tests/pureeos_committed_policy_observer_test.py',
        'tests/verify_committed_policy_recording.py','tests/parallel_access_hints_test.cpp',
        'tests/parallel_immutable_input_test.cpp',
        'tests/run_access_hints_test.py','tests/start_v1_integrated_cloud_build.py',
        'tests/v1_integrated_cloud_build_guest.py','tests/watch_v1_integrated_cloud_build.py',
        'tests/run_observer_admission_smoke.py','tests/fetch_native_policy_test_cache.py',
        'tests/v1_candidate_deployment_test.py','tests/v1_candidate_deployment_test_test.py',
        'tests/v1_candidate_native_matrix.py', 'tests/v1_candidate_native_matrix_test.py',
        'tests/v1_candidate_recovery_test.py', 'tests/v1_candidate_recovery_test_test.py',
        'tests/archive_v1_candidate_sources.py', 'tests/archive_v1_candidate_sources_test.py',
        'tests/audit_v1_native_shards.py', 'tests/audit_v1_native_shards_test.py',
        'tests/run_v1_candidate_controls.py', 'tests/run_v1_candidate_sdk.py',
        'tests/v1_worker_failure_diagnostics.py',
        'tests/v1_local_retest_host.py', 'tests/v1_local_retest_guest.py',
        'tests/prepare_v1_engine_profile_retest.py', 'tests/prepare_v1_engine_profile_retest_test.py',
        'tests/v1_engine_profile_retest_host.py', 'tests/v1_engine_profile_retest_guest.py',
        'tests/v1_engine_profile_metrics.py',
        'tests/prepare_v1_native_profile_retest.py', 'tests/prepare_v1_native_profile_retest_test.py',
        'tests/v1_native_profile_retest_guest.py', 'tests/v1_native_profile_retest_host.py',
        'tests/v1_native_profile_metrics.py',
        'tests/v1_offline_portability_test.py',
        'tests/v1_local_repair_syntax.py',
        'tests/v1_local_cohort_release_test.py', 'tests/v1_local_cohort_release_test_test.py',
        'tests/v1_local_release_workflows.py', 'tests/v1_local_release_workflows_test.py',
        'tests/v1_local_retest_metrics.py', 'tests/v1_local_retest_metrics_test.py',
        'tests/v1_live_reward_claim_test.py', 'tests/v1_live_reward_claim_test_test.py',
        'tests/v1_live_reward_claim_host.py',
        'tests/pureeos_candidate_sync_test.py',
        'tests/native_policy_fixture_registration_test.py',
        'tests/native_policy_boundary_fixture.cpp','tests/run_native_policy_boundary.py',
        'tests/snapshot_v1_phase2_evidence.py','docs/parallel-execution/PX7_V1_HARDENING_PHASE2_RESULTS.md',
        'docs/parallel-execution/PX7_V1_PIPELINE_AND_LLVM_DECISION.md',
        'docs/parallel-execution/PX7_V1_REMAINING_OPTIMIZATION_DECISION.md',
        'docs/parallel-execution/PX7_V1_RELEASE_CHECKLIST.txt']
    node_files.append('docs/parallel-execution/PX7_V1_CANDIDATE_GUIDE.txt')
    node_files += ['docs/parallel-execution/PX7_V1_COLD_WARM_REPAIR_RESULTS.md',
        'docs/parallel-execution/PX7_V1_FAST_TESTING.md',
        'docs/parallel-execution/PX7_V1_RUNTIME_FIXTURE_REPAIR.md',
        'docs/parallel-execution/PX7_V1_RUNTIME_PACKAGE_RESULTS.md',
        'docs/parallel-execution/PX7_V1_CLOUD_ACCEPTANCE_HANDOFF.md',
        'docs/parallel-execution/PX7_V1_POST_REBOOT_TEST_RECOVERY.md',
        'docs/parallel-execution/PX7_V1_AIC_CDT_AND_COHORT_RECOVERY.md',
        'docs/parallel-execution/PX7_V1_LIVE_REWARD_TOPOLOGY_REPAIR.md',
        'docs/parallel-execution/PX7_V1_CURRENT_STATUS.txt',
        'docs/parallel-execution/PX7_V1_BUILD_VERSION_RESULTS.md',
        'docs/parallel-execution/PX7_V1_VENDOR_SOURCE_RESULTS.md',
        'docs/parallel-execution/PX7_V1_RESOURCE_LIMIT_BOUNDARY_RESULTS.md',
        'docs/parallel-execution/PX7_V1_CURRENT_PROFILE_REPAIR_RESULTS.md',
        'docs/parallel-execution/PX7_V1_LOCAL_RELEASE_WORKFLOWS.md']
    for name in node_files:files['node/'+name]=source/name
    # Exact pre-existing VM changes are part of the actual build baseline,
    # not new N2 implementation and not a clean upstream vendor tree.
    for name in ('allocator.hpp','parser.hpp','x86_64.hpp'):
        relative='libraries/eos-vm/include/eosio/vm/'+name
        files['node/'+relative]=source/relative
    if args.include_safe_eviction:
        for name in ('unittests/pureeos_sequential_rotation_tests.inc', 'unittests/pureeos_governance_tests.cpp',
                     'unittests/pureeos_resource_split_tests.inc', 'unittests/pureeos_committee_boundary_tests.inc',
                     'tests/run_safe_eviction_regression.py',
                     'tests/audit_safe_eviction_evidence.py',
                     'docs/parallel-execution/PX7_BLACKLIST_SAFE_EVICTION_RESULTS.md'):
            files['node/'+name]=source/name
        contracts=source.parent/'reference-contracts-v1-hardening'
        for name in ('LICENSE','AIC_LICENSE.md','LICENSING.md','README.md',
                     'contracts/CMakeLists.txt', 'contracts/eosio.system/src/rotation_state.cpp',
                     'contracts/eosio.system/include/eosio.system/rotation_state.hpp',
                     'contracts/eosio.system/include/eosio.system/powerup.results.hpp',
                     'contracts/eosio.system/include/eosio.system/rex.results.hpp', 'SAFE_BLACKLIST_EVICTION.md'):
            files['contracts/'+name]=contracts/name
    cdt_files=['LICENSE','AIC_LICENSE.md','LICENSING.md','README.md','DEVELOPMENT_STATUS.md','docs/INHERITED_README.md',
        'CMakeLists.txt','VERSION','UPSTREAM_BASELINE.json','AIC_CDT_1.0.0_IDENTITY.md',
        '.github/workflows/build.yaml','.github/workflows/release.yaml',
        'modules/AICCDTVersion.cmake','tools/cc/cdt-cpp.cpp.in','tools/cc/cdt-cc.cpp.in',
        'tools/ld/cdt-ld.cpp.in','tools/abidiff/cdt-abidiff.cpp.in','tools/include/compiler_options.hpp.in',
        'modules/cdt-config.cmake','modules/CDTWasmToolchain.cmake.in','tools/init/cdt-init.cpp',
        'tests/hardening/sdk_relocation_fixture/CMakeLists.txt',
        'libraries/eosiolib/core/eosio/varint.hpp','libraries/eosiolib/core/eosio/datastream.hpp',
        'libraries/eosiolib/simple_memory.hpp','libraries/eosiolib/simple_malloc.cpp',
        'tests/unit/datastream_tests.cpp','tools/toolchain-tester/tests.py','tools/toolchain-tester/testsuite.py',
        'tools/toolchain-tester/main.py','tools/toolchain-tester/testrunner.py']
    for name in cdt_files:files['cdt/'+name]=cdt/name
    for p in (cdt/'tests/hardening').glob('*'):
        if p.is_file() and p.suffix in ('.cpp','.py','.cmake'):files['cdt/'+str(p.relative_to(cdt))]=p
    plan=source.parent/'PureEOS_并行首版与CDT加固优化_实施计划.md'
    files['plan/'+plan.name]=plan
    def sha(p):
        with p.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
    manifest={name:sha(p) for name,p in sorted(files.items())}
    with (output/'sources.json').open('x') as stream:json.dump(manifest,stream,indent=2)
    archive=output/'phase2-changed-sources.tar.gz'
    with tarfile.open(archive,'x:gz') as stream:
        for name,p in sorted(files.items()):stream.add(p,arcname=name,recursive=False)
        stream.add(output/'sources.json',arcname='sources.json')
    assert all(sha(files[name])==expected for name,expected in manifest.items())
    value=dict(status='source-checkpoint-not-release',release_ready=False,files=len(files),
        archive=str(archive),bytes=archive.stat().st_size,sha256=sha(archive),
        created_utc=datetime.now(timezone.utc).isoformat(),
        boundary_decision_pending=not args.include_safe_eviction,
        safe_eviction_sources_included=args.include_safe_eviction,
        test_results_note='Source checkpoint only; consult separately hashed candidate-specific test reports',
        new_node_integrated_tests_pending=True,performance_gate_pending=True)
    with (output/'report.json').open('x') as stream:json.dump(value,stream,indent=2)
    print(json.dumps(value,indent=2))


if __name__=='__main__':main()
