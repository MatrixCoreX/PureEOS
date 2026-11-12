"""Fresh isolated local build and fixed regression matrix; no cloud or TPS work."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time
import traceback

ROOT=Path('/work')
SOURCE=ROOT/'source'
BUILD=ROOT/'build'
PROGRAMS=('nodeos','cleos','keosd','helios-util')
TESTS=('parallel_validation_test','parallel_wasm_worker_test','native_savanna_test',
       'parallel_access_hints_test','parallel_worker_signals_test','parallel_immutable_input_test')
FOCUSED=('parallel_sync_private_tests/sync_leaf_wasm_intervals_overlap_on_two_and_four_threads',
    'parallel_console_oracle_tests/selected_console_profile_matches_both_canonical_and_private_engines',
    'parallel_exit_oracle_tests/ordinary_and_start_exit_codes_match_original_vm_and_actual_adoption',
    'parallel_environment_oracle_tests/multiple_roots_and_both_envelopes_keep_original_transaction_bytes_in_every_frame')
SUITES=('pureeos_genesis_tests','pureeos_governance_tests','pureeos_governance_contract_tests',
    'pureeos_single_choice_contract_tests','pureeos_resource_split_tests','pureeos_committee_boundary_tests',
    'pureeos_reward_halving_tests','pureeos_reward_activation_tests','pureeos_sequential_rotation_tests',
    'pureeos_blacklist_current_policy_tests')


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    plan=json.loads((ROOT/'input.json').read_text())
    report=dict(status='running',scope=__doc__,release_ready=False,started_utc=datetime.now(timezone.utc).isoformat(),
        jobs=[],parent_core_status='failed',source_identity_sha256=sha(ROOT/'source-identity-final.json'),
        image=plan['image'],input_sha256=sha(ROOT/'input.json'),no_cloud_purchase=True)
    deadline=time.monotonic()+8*3600
    def save():
        report['updated_utc']=datetime.now(timezone.utc).isoformat()
        temporary=ROOT/'report.pending';temporary.write_text(json.dumps(report,indent=2));temporary.replace(ROOT/'report.json')
    def run(name,command,timeout):
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
        command=[BUILD/'unittests'/program,'--run_test='+selector,'--report_level=detailed','--color_output=no']
        if engine:command+=['--','--'+engine]
        return run(name,command,timeout)
    try:
        save()
        assert sha('/runner.py')==plan['runner_sha256']
        assert all(sha(SOURCE/p)==digest for p,digest in plan['overlay_sha256'].items())
        assert all(sha(Path('/contracts')/p)==digest for p,digest in plan['contracts_sha256'].items())
        report['toolchain']={name:subprocess.check_output(command,text=True).splitlines()[0] for name,command in
            [('compiler',['c++','--version']),('cmake',['cmake','--version']),('ninja',['ninja','--version'])]};save()
        run('configure',['cmake','-S',SOURCE,'-B',BUILD,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG','-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG',
            '-DENABLE_OC=OFF','-DENABLE_TESTS=OFF','-DENABLE_TOOLS=ON','-DENABLE_CCACHE=OFF',
            *['-D'+p+'=/contracts' for p in ('PUREEOS_GOVERNANCE_CONTRACTS_DIR',
              'PUREEOS_SINGLE_CHOICE_CONTRACTS_DIR','PUREEOS_RESOURCE_SPLIT_CONTRACTS_DIR','PUREEOS_ROTATION_CONTRACTS_DIR')]],300)
        run('build-node-controller',['cmake','--build',BUILD,'--target',*PROGRAMS,*TESTS,'--parallel','1'],6*3600)
        binaries={**{f'programs/{p}/{p}':BUILD/'programs'/p/p for p in PROGRAMS},
            **{f'unittests/{p}':BUILD/'unittests'/p for p in TESTS}}
        report['binaries_sha256']={name:sha(path) for name,path in binaries.items()};save()
        for n in range(1,4):
            for engine in ('eos-vm','eos-vm-jit'):
                boost(f'cold-warm-{engine}-round-{n}','parallel_wasm_worker_test',':'.join(FOCUSED),engine,600)
            boost(f'immutable-input-integration-probe-{n}','parallel_wasm_worker_test',
                'parallel_worker_pool_tests/shared_tree_catalog_versions_keep_old_inputs_and_original_budgets',timeout=180)
        for n in range(1,11):
            boost(f'leaf-overlap-stability-{n}','parallel_wasm_worker_test',
                'parallel_sync_pool_tests/multi_sync_pool_overlaps_leaf_execution_across_transactions_and_resets_each_root',timeout=180)
        critical='pureeos_blacklist_current_policy_tests:pureeos_reward_halving_tests/permanently_banned_producer_can_claim_old_unpaid_blocks_but_not_new_rewards:pureeos_sequential_rotation_tests/rotation_committee_six_offline_seven_offline_and_recovery'
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
            text=boost(f'full-native-matrix-round-{n}','native_savanna_test',':'.join(SUITES),timeout=3600)
            actual=set(re.findall(r'Test suite "([^"]+)" has passed',text))
            assert set(SUITES)<=actual,'Missing or unexecuted native suite: '+str(set(SUITES)-actual)
        for n in range(1,4):
            run(f'cli-round-{n}',['python3',SOURCE/'tests/parallel_runtime_cli_test.py',
                '--nodeos',BUILD/'programs/nodeos/nodeos','--work-dir',ROOT/f'cli-{n}'],300)
            assert json.loads((ROOT/f'cli-{n}/report.json').read_text())['status']=='passed'
            run(f'sdk-round-{n}',['python3',SOURCE/'tests/ubuntu_cdt_runtime_test.py',
                '--nodeos',BUILD/'programs/nodeos/nodeos','--cleos',BUILD/'programs/cleos/cleos',
                '--fixture-exporter',BUILD/'unittests/parallel_wasm_worker_test','--sdk-hardening',
                '--complex-wasm','/sdk/complexbench/complexbench.wasm','--cdt-wasm','/sdk/cdtcompat/cdtcompat.wasm',
                '--work-dir',ROOT/f'sdk-{n}'],900)
            actual=json.loads((ROOT/f'sdk-{n}/report.json').read_text())
            assert actual['status']=='passed' and [c['workers'] for c in actual['cases']]==[0,4]
            assert all(c['passed'] for c in actual['cases'])
        identity=json.loads((ROOT/'source-identity-final.json').read_text())
        assert all(sha(SOURCE/p)==digest for p,digest in identity['files_sha256'].items()),'Frozen source changed'
        assert all(sha(p)==report['binaries_sha256'][name] for name,p in binaries.items())
        assert all(sha(Path('/contracts')/p)==digest for p,digest in plan['contracts_sha256'].items())
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error),traceback=traceback.format_exc())
    report['finished_utc']=datetime.now(timezone.utc).isoformat();save()
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
