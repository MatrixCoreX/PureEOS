"""Exact-build full matrix; verified completed prefix may resume after test-driver-only repair. No cloud/TPS."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import traceback

ROOT=Path('/work')
SOURCE=ROOT/'source'
BUILD=ROOT/'build'
PROGRAMS=('nodeos','cleos','keosd','helios-util')
TESTS=('parallel_validation_test','parallel_wasm_worker_test','native_savanna_test',
       'parallel_access_hints_test','parallel_worker_signals_test','parallel_immutable_input_test','resource_limits_boundary_test')
FOCUSED=('parallel_integer_oracle_tests/selected_integer_profile_configures_canonical_and_private_engines',
    'parallel_float_oracle_tests/selected_float_profile_configures_canonical_and_private_engines',
    'parallel_pure_hash_oracle_tests/selected_hash_profile_configures_canonical_and_private_engines',
    'parallel_secondary_wasm_oracle_tests/selected_secondary_profile_configures_canonical_and_private_engines',
    'parallel_context_free_adoption_tests/selected_context_free_profile_configures_canonical_and_private_engines',
    'parallel_sync_private_tests/sync_leaf_wasm_intervals_overlap_on_two_and_four_threads',
    'parallel_console_oracle_tests/selected_console_profile_matches_both_canonical_and_private_engines',
    'parallel_exit_oracle_tests/ordinary_and_start_exit_codes_match_original_vm_and_actual_adoption',
    'parallel_environment_oracle_tests/multiple_roots_and_both_envelopes_keep_original_transaction_bytes_in_every_frame')
SUITES=('pureeos_genesis_tests','pureeos_governance_tests','pureeos_governance_contract_tests',
    'pureeos_single_choice_contract_tests','pureeos_resource_split_tests','pureeos_committee_boundary_tests',
    'pureeos_reward_halving_tests','pureeos_reward_activation_tests','pureeos_sequential_rotation_tests',
    'pureeos_blacklist_current_policy_tests')

PREFLIGHT=('pureeos_governance_contract_tests/production_bootstrap_and_electorate_ranking',
    'pureeos_governance_contract_tests/production_system_self_upgrade_and_resource_transfers_survive',
    'pureeos_single_choice_contract_tests/zero_stake_can_select_one_producer_without_creating_votes',
    'pureeos_single_choice_contract_tests/uninitialized_current_profile_rejects_votes_and_initializes_only_once',
    'pureeos_single_choice_contract_tests/queued_sale_blocks_clear_and_closed_sale_settles_once_before_switch',
    'pureeos_single_choice_contract_tests/integer_rpc_preserves_above_double_precision_and_pagination',
    'pureeos_single_choice_contract_tests/governance_live_index_preserves_full_electorate_above_1024_registrations',
    'pureeos_resource_split_tests/bootstrap_tracks_weights_and_requires_powerup_before_seal',
    'pureeos_resource_split_tests/rex_rental_fee_custody_with_forwarder',
    'pureeos_resource_split_tests/rex_rental_fee_custody_empty_treasury',
    'pureeos_resource_split_tests/rex_rental_fee_custody_without_forwarder',
    'pureeos_committee_boundary_tests/twenty_two_candidates_all_rotate_but_zero_output_cannot_claim',
    'pureeos_committee_boundary_tests/integer_ties_and_one_unit_changes_preserve_fifo_and_atomic_one_choice',
    'pureeos_committee_boundary_tests/twenty_one_member_quorum_loss_and_recovery_before_blacklist_windows')



def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    plan=json.loads((ROOT/'input.json').read_text())
    resume=plan.get('same_build_resume')
    cached={job['name']:job for job in resume['jobs']} if resume else {}
    report=dict(status='running',scope=__doc__,release_ready=False,started_utc=datetime.now(timezone.utc).isoformat(),
        jobs=list(cached.values()),parent_core_status='failed-runtime-fixtures-repaired' if resume else 'failed',
        source_identity_sha256=sha(ROOT/'source-identity-final.json'),
        image=plan['image'],input_sha256=sha(ROOT/'input.json'),no_cloud_purchase=True)
    if resume:report['same_build_resume']=resume
    deadline=time.monotonic()+8*3600
    def save():
        report['updated_utc']=datetime.now(timezone.utc).isoformat()
        temporary=ROOT/'report.pending';temporary.write_text(json.dumps(report,indent=2));temporary.replace(ROOT/'report.json')
    def run(name,command,timeout):
        if name in cached:
            assert cached[name]['exit_code']==0 and sha(ROOT/(name+'.log'))==resume['logs_sha256'][name]
            print(name+': reused verified same-build completion',flush=True)
            return (ROOT/(name+'.log')).read_text(errors='replace')
        remaining=int(deadline-time.monotonic())
        if remaining<30:raise RuntimeError('Bounded local run deadline exhausted')
        started=datetime.now(timezone.utc).isoformat()
        with (ROOT/(name+'.log')).open('x') as log:
            result=subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,
                timeout=min(timeout,remaining),cwd=ROOT)
        report['jobs'].append(dict(name=name,exit_code=result.returncode,started_utc=started,
            finished_utc=datetime.now(timezone.utc).isoformat()));save()
        print(name+': exit='+str(result.returncode),flush=True)
        if result.returncode:raise RuntimeError(name+' failed; preserve this candidate and do not retry until green')
        return (ROOT/(name+'.log')).read_text(errors='replace')
    def boost(name,program,selector='!*/export_*',engine=None,timeout=2400):
        command=[BUILD/'unittests'/program,'--run_test='+selector,'--report_level=detailed','--log_level=test_suite','--color_output=no']
        if engine:command+=['--','--'+engine]
        return run(name,command,timeout)
    try:
        save()
        assert sha('/runner.py')==plan['runner_sha256']
        assert all(sha(ROOT/'frozen-runners'/name)==value for name,value in plan['runner_identity'].items())
        sys.path.insert(0,str(ROOT/'frozen-runners'))
        from v1_runtime_dependencies import FULL_CDT_CASES, verify
        from v1_fast_regression import verify_boost
        assert sha(ROOT/'runtime-dependencies.json')==plan['runtime_dependencies_sha256']
        dependencies=json.loads((ROOT/'runtime-dependencies.json').read_text());verify(ROOT,dependencies)
        report['runtime_dependencies_verified']=len(dependencies);save()
        assert all(sha(SOURCE/p)==digest for p,digest in plan['overlay_sha256'].items())
        assert all(sha(Path('/contracts')/p)==digest for p,digest in plan['contracts_sha256'].items())
        if resume:
            assert resume['source_identity_sha256']==report['source_identity_sha256']
            assert len(cached)==len(resume['jobs']) and set(cached)==set(resume['logs_sha256'])
            assert all(j['exit_code']==0 and j.get('origin')=='same-build-parent' for j in cached.values())
            assert all(sha(BUILD/p)==d for p,d in resume['binaries_sha256'].items())
            identity=json.loads((ROOT/'source-identity-final.json').read_text())
            assert all(sha(SOURCE/p)==d for p,d in identity['files_sha256'].items()), 'Cannot reuse results with changed source'
        # Check the nine previously aborted cases before starting the long lane.
        # This is an additional preflight, never a substitute for rerunning the
        # entire failed worker job and all remaining regression rounds.
        started=datetime.now(timezone.utc).isoformat()
        with (ROOT/'full-cdt-fixture-preflight.log').open('x') as log:
            result=subprocess.run([str(BUILD/'unittests/parallel_wasm_worker_test'),
                '--run_test='+':'.join(FULL_CDT_CASES),'--report_level=detailed',
                '--log_level=test_suite','--color_output=no'],cwd=ROOT,
                stdout=log,stderr=subprocess.STDOUT,timeout=1200)
        report['full_cdt_fixture_preflight']=dict(exit_code=result.returncode,cases=list(FULL_CDT_CASES),
            started_utc=started,finished_utc=datetime.now(timezone.utc).isoformat());save()
        verify_boost((ROOT/'full-cdt-fixture-preflight.log').read_text(),FULL_CDT_CASES,result.returncode)
        verify(ROOT,dependencies)
        report['full_cdt_fixture_preflight']['status']='passed';save()
        report['toolchain']={name:subprocess.check_output(command,text=True).splitlines()[0] for name,command in
            [('compiler',['c++','--version']),('cmake',['cmake','--version']),('ninja',['ninja','--version'])]};save()
        run('configure',['cmake','-S',SOURCE,'-B',BUILD,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG','-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG',
            '-DENABLE_OC=OFF','-DENABLE_TESTS=OFF','-DENABLE_TOOLS=ON','-DENABLE_CCACHE=OFF',
            '-DPUREEOS_SOURCE_ID='+report['source_identity_sha256'],
            *['-D'+p+'=/contracts' for p in ('PUREEOS_GOVERNANCE_CONTRACTS_DIR',
              'PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR','PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR','PUREEOS_ROTATION_CONTRACTS_DIR')]],300)
        run('build-node-controller',['cmake','--build',BUILD,'--target',*PROGRAMS,*TESTS,'--parallel','1'],6*3600)
        binaries={**{f'programs/{p}/{p}':BUILD/'programs'/p/p for p in PROGRAMS},
            **{f'unittests/{p}':BUILD/'unittests'/p for p in TESTS}}
        report['binaries_sha256']={name:sha(path) for name,path in binaries.items()};save()
        assert sha(SOURCE/'libraries/chain/resource_limits.cpp') == plan['production_resource_cpp_sha256']
        assert report['binaries_sha256']['programs/nodeos/nodeos'] != plan['parent_programs_sha256']['nodeos'], \
            'Production range fix must be linked into the new nodeos, not a stale cached executable'
        report['program_changed_from_parent']={p:report['binaries_sha256']['programs/'+p+'/'+p]
            != plan['parent_programs_sha256'][p] for p in PROGRAMS};save()
        expected_version='v2.0.0-src-'+report['source_identity_sha256']
        version_probe='''import json,pathlib,subprocess
build=pathlib.Path('/work/build/programs')
checks={}
for name,args in [('nodeos',['--full-version']),('keosd',['--full-version']),('cleos',['version','full'])]:
 result=subprocess.run([str(build/name/name),*args],text=True,capture_output=True,timeout=30)
 assert result.returncode==0 and EXPECTED in result.stdout.splitlines(), (name,result.stdout,result.stderr)
 checks[name]=EXPECTED
print(json.dumps(checks))
'''.replace('EXPECTED',repr(expected_version))
        text=run('build-version-provenance',['python3','-I','-B','-c',version_probe],120)
        assert json.loads(text)=={n:expected_version for n in ('nodeos','cleos','keosd')}
        report['build_version_provenance']=dict(verified=True,client='v2.0.0',full=expected_version,
            source_manifest_sha256=report['source_identity_sha256']);save()
        for n in range(1,4):
            text=boost(f'resource-limits-round-{n}','resource_limits_boundary_test','resource_limits_test',timeout=180)
            actual=set(re.findall(r'Test case "([^"]+)" has passed',text))
            assert len(actual)==15 and {
                'resource_limits_test/pending_large_weights_remain_finite_and_enforce_block_caps',
                'resource_limits_test/pending_large_weight_query_undo_restores_finite_limits',
                'resource_limits_test/representable_pending_limits_keep_original_exact_formula'} <= actual
        text=boost('native-profile-preflight','native_savanna_test',':'.join(PREFLIGHT),timeout=2400)
        actual=set(re.findall(r'Test case "([^"]+)" has passed',text))
        assert set(PREFLIGHT)<=actual, 'Missing actual new-profile preflight case: '+str(set(PREFLIGHT)-actual)
        for n in range(1,4):
            for engine in ('eos-vm','eos-vm-jit'):
                text=boost(f'cold-warm-{engine}-round-{n}','parallel_wasm_worker_test',':'.join(FOCUSED),engine,900)
                actual=set(re.findall(r'Test case "([^"]+)" has passed',text))
                assert set(FOCUSED)<=actual, 'Missing actual engine-profile/cold-warm case: '+str(set(FOCUSED)-actual)
            boost(f'immutable-input-integration-probe-{n}','parallel_wasm_worker_test',
                'parallel_worker_pool_tests/shared_tree_catalog_versions_keep_old_inputs_and_original_budgets',timeout=180)
        for n in range(1,11):
            boost(f'leaf-overlap-stability-{n}','parallel_wasm_worker_test',
                'parallel_sync_pool_tests/multi_sync_pool_overlaps_leaf_execution_across_transactions_and_resets_each_root',timeout=180)
        critical='pureeos_blacklist_current_policy_tests:pureeos_reward_halving_tests/permanently_banned_producer_can_claim_old_unpaid_blocks_but_not_new_rewards:pureeos_sequential_rotation_tests/rotation_committee_six_offline_seven_offline_and_recovery'
        for n in range(1,4):
            text=boost(f'full-native-matrix-round-{n}','native_savanna_test',':'.join(SUITES),timeout=3600)
            actual=set(re.findall(r'Test suite "([^"]+)" has passed',text))
            assert set(SUITES)<=actual,'Missing or unexecuted native suite: '+str(set(SUITES)-actual)
        for n in range(1,4):
            for name,program in [('immutable-input','parallel_immutable_input_test'),
                ('access-hints','parallel_access_hints_test'),('worker-signals','parallel_worker_signals_test')]:
                run(f'{name}-round-{n}',[BUILD/'unittests'/program],60)
            for engine in ('eos-vm-jit','eos-vm'):
                suffix='' if engine=='eos-vm-jit' else '-interpreter'
                boost(f'controller{suffix}-round-{n}','parallel_validation_test',engine=engine)
                boost(f'worker{suffix}-round-{n}','parallel_wasm_worker_test',engine=engine)
            boost(f'blacklist-and-history-round-{n}','native_savanna_test',critical,timeout=2400)
        for n in range(1,4):
            run(f'cli-round-{n}',['python3',SOURCE/'tests/parallel_runtime_cli_test.py',
                '--nodeos',BUILD/'programs/nodeos/nodeos','--work-dir',ROOT/f'cli-{n}'],300)
            assert json.loads((ROOT/f'cli-{n}/report.json').read_text())['status']=='passed'
            run(f'sdk-round-{n}',['python3',SOURCE/'tests/ubuntu_cdt_runtime_test.py',
                '--nodeos',BUILD/'programs/nodeos/nodeos','--cleos',BUILD/'programs/cleos/cleos',
                '--fixture-exporter',BUILD/'unittests/parallel_validation_test','--sdk-hardening',
                '--complex-wasm','/sdk/complexbench/complexbench.wasm','--cdt-wasm','/sdk/cdtcompat/cdtcompat.wasm',
                '--work-dir',ROOT/f'sdk-{n}'],900)
            actual=json.loads((ROOT/f'sdk-{n}/report.json').read_text())
            assert actual['status']=='passed' and [c['workers'] for c in actual['cases']]==[0,4]
            assert all(c['passed'] for c in actual['cases'])
        identity=json.loads((ROOT/'source-identity-final.json').read_text())
        assert all(sha(SOURCE/p)==digest for p,digest in identity['files_sha256'].items()),'Frozen source changed'
        assert all(sha(p)==report['binaries_sha256'][name] for name,p in binaries.items())
        assert all(sha(Path('/contracts')/p)==digest for p,digest in plan['contracts_sha256'].items())
        verify(ROOT,dependencies)
        report['runtime_dependencies_unchanged']=True
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error),traceback=traceback.format_exc())
    report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
