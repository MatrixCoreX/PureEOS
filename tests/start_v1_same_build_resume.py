"""Bounded local handoff at native-3 completion. No rebuild, cloud, TPS or foreign process control."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import traceback

from prepare_v1_same_build_resume import job_order
from v1_fast_regression import BASE, IMAGE, check_parent, inspect_owned, sha
from v1_local_release_workflows import SUPPORT as WORKFLOW_SUPPORT, read_progress, save
from v1_early_live_reward_claim_host import SUPPORT as LIVE_SUPPORT


def main():
    os.umask(0o077)
    root=BASE/'same-build-resume-transition-attempt-1';root.mkdir(exist_ok=False)
    frozen=root/'frozen-tests';frozen.mkdir()
    names=set((*WORKFLOW_SUPPORT,*LIVE_SUPPORT,'prepare_v1_same_build_resume.py',
               'v1_native_profile_retest_guest.py','v1_native_profile_retest_host.py',
               'start_v1_same_build_resume.py'))
    for name in names:shutil.copyfile(Path(__file__).with_name(name),frozen/name)
    hashes={name:sha(frozen/name) for name in names}
    save(root/'frozen-inputs.json',hashes)
    report=dict(status='waiting-for-third-native',release_ready=False,no_cloud_purchase=True,jobs=[])
    parent=BASE/'local-native-profile-retest-attempt-7'
    runtime=BASE/'runtime-preflight-attempt-3'
    def run(label,command,timeout=180):
        with (root/(label+'.log')).open('x') as log:
            result=subprocess.run(list(map(str,command)),stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
        report['jobs'].append(dict(name=label,exit_code=result.returncode));save(root/'report.json',report)
        assert result.returncode==0,label+' failed; never retry until green'
    try:
        save(root/'report.json',report)
        deadline=time.monotonic()+2*3600
        while True:
            actual=read_progress(parent/'report.json')
            check_parent(actual,sha(parent/'source-identity-final.json'))
            if 'full-native-matrix-round-3' in {j['name'] for j in actual['jobs']}:break
            assert time.monotonic()<deadline,'Bounded native-3 wait expired'
            time.sleep(2)
        assert 29<=len(actual['jobs'])<=33
        assert [j['name'] for j in actual['jobs']]==job_order()[:len(actual['jobs'])]
        corrected=read_progress(runtime/'report.json');proof=read_progress(runtime/'download-verified.json')
        assert corrected['status']=='runtime-preflight-passed-not-release' and corrected['runtime_rounds']==[1,2,3]
        assert proof['status']=='passed' and sha(runtime/'smoke-evidence.tar.gz')==proof['sha256']
        assert read_progress(runtime/'input.json')['binaries_sha256']==actual['binaries_sha256']
        assert all(sha(frozen/name)==digest for name,digest in hashes.items())
        report['status']='closing-superseded-tasks';save(root/'report.json',report)
        # These exact units belong to this test task. Containers additionally
        # require both the original task label and immutable image identity.
        assert inspect_owned('pureeos-v1-local-native-profile-retest-attempt-7')
        inspect_owned('pureeos-v1-live-reward-claims-attempt-9')
        run('stop-old-real-day',['systemctl','--user','stop','pureeos-v1-live-reward-claims-attempt-9-import-fixed.service'])
        run('close-old-core',['systemctl','--user','stop','pureeos-v1-local-native-profile-retest-attempt-7.service'])
        actual=read_progress(parent/'report.json')
        marker=dict(status='superseded-test-driver-only',production_changed=False,
            reason_code='sdk-exporter-owned-by-validation-not-worker',
            runtime_correction_status=corrected['status'],runtime_correction_archive_sha256=proof['sha256'],
            parent_report_sha256=sha(parent/'report.json'),original_completed_jobs=len(actual['jobs']),
            note='Current unfinished job is NOT inherited. Original report remains running/superseded, not passed.')
        save(parent/'superseded-test-driver-only.json',marker)
        save(BASE/'live-reward-claims-attempt-9'/'superseded-test-driver-only.json',dict(
            status='superseded-with-private-chain-stopped',reason=marker['reason_code'],release_ready=False,
            note='New real-day chain must independently wait a fresh genuine 86400 seconds; no elapsed time carried over.'))
        report['status']='preparing-independent-same-build';save(root/'report.json',report)
        new=BASE/'local-native-profile-retest-attempt-8'
        run('prepare',['python3','-B',frozen/'prepare_v1_same_build_resume.py','--parent',parent,'--output',new],1200)
        run('start-core',['systemd-run','--user','--unit=pureeos-v1-local-native-profile-retest-attempt-8',
            '--description=PureEOS same-build verified-prefix continuation, corrected SDK exporter',
            '--property=RuntimeMaxSec=46800','--property=TimeoutStopSec=180',
            'python3','-B',new/'frozen-runners/v1_native_profile_retest_host.py','--root',new])
        deadline=time.monotonic()+300
        while True:
            if (new/'report.json').is_file():
                value=read_progress(new/'report.json')
                assert value['status']=='running','New candidate did not enter healthy running state'
                if value.get('build_version_provenance',{}).get('verified'):
                    check_parent(value,sha(new/'source-identity-final.json'));break
            assert time.monotonic()<deadline,'New core provenance startup timeout'
            time.sleep(2)
        run('start-workflows',['systemd-run','--user','--unit=pureeos-v1-local-release-workflows-attempt-9',
            '--description=PureEOS exact same-build continuation P2P recovery and real-clock 30-to-40',
            '--property=RuntimeMaxSec=115200','--property=TimeoutStopSec=240',
            'python3','-B',frozen/'v1_local_release_workflows.py','--core',new,
            '--output',BASE/'local-release-workflows-attempt-9'])
        run('start-real-day',['systemd-run','--user','--unit=pureeos-v1-live-reward-claims-attempt-10',
            '--description=PureEOS fresh real-day claims overlapping corrected same-build full matrix',
            '--property=RuntimeMaxSec=140400','--property=TimeoutStopSec=240',
            'python3','-B',frozen/'v1_early_live_reward_claim_host.py','--core',new,
            '--smoke',BASE/'fast-regression-attempt-2','--output',BASE/'live-reward-claims-attempt-10'])
        assert all(sha(frozen/name)==digest for name,digest in hashes.items())
        report.update(status='handed-off',reused_jobs=len(actual['jobs']),source_identity_sha256=sha(new/'source-identity-final.json'),
            new_core=str(new),new_real_day='live-reward-claims-attempt-10',metrics_note='Old metrics7 ends automatically; metrics8 not started by this handoff.')
    except Exception as error:
        report.update(status='failed',error=str(error),traceback=traceback.format_exc())
    save(root/'report.json',report);print(json.dumps(report),flush=True)
    return 0 if report['status']=='handed-off' else 1


if __name__=='__main__':raise SystemExit(main())
