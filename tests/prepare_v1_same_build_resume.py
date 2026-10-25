"""Continue an unchanged 11-ELF build after a verified green prefix; never reuse failed/unrun jobs."""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from v1_fast_regression import BASE, IMAGE, PROGRAMS, TESTS, check_parent, sha
from v1_runtime_dependencies import FULL_CDT, FULL_CDT_CASES, copy_dependencies, inventory


def job_order():
    names=['configure','build-node-controller','build-version-provenance']
    names += [f'resource-limits-round-{n}' for n in range(1,4)]
    names += ['native-profile-preflight']
    for n in range(1,4):
        names += [f'cold-warm-{engine}-round-{n}' for engine in ('eos-vm','eos-vm-jit')]
        names += [f'immutable-input-integration-probe-{n}']
    names += [f'leaf-overlap-stability-{n}' for n in range(1,11)]
    names += [f'full-native-matrix-round-{n}' for n in range(1,4)]
    for n in range(1,4):
        names += [f'{part}-round-{n}' for part in ('immutable-input','access-hints','worker-signals',
                  'controller','worker','controller-interpreter','worker-interpreter','blacklist-and-history')]
    for n in range(1,4):names += [f'cli-round-{n}',f'sdk-round-{n}']
    return names


def check_prefix(report, proof, monitor, marker, identity):
    check_parent(report,identity)
    jobs=report['jobs']; names=[j['name'] for j in jobs]
    assert 29<=len(jobs)<=33 and names==job_order()[:len(jobs)], 'Only completed native-3/controller-1 boundary may resume'
    assert report['status']=='running' and proof.get('status')=='passed'
    assert proof.get('candidate_test_status')=='running'
    assert monitor.get('status')=='finished-and-archived' and monitor.get('candidate_status')=='running'
    assert marker.get('status')=='superseded-test-driver-only' and marker.get('production_changed') is False
    assert marker.get('reason_code')=='sdk-exporter-owned-by-validation-not-worker'
    assert marker.get('runtime_correction_status')=='runtime-preflight-passed-not-release'
    return True


def check_fixture_failure(report, proof, monitor, log, identity):
    """Only this closed, fully classified fixture failure may reuse earlier green jobs."""
    assert report['status']=='failed' and report.get('error') == \
        'worker-round-1 failed; preserve this candidate and do not retry until green'
    assert proof.get('status')=='passed' and proof.get('candidate_test_status')=='failed'
    assert monitor.get('status')=='finished-and-archived' and monitor.get('candidate_status')=='failed'
    jobs=report['jobs']
    assert len(jobs)==34 and [j['name'] for j in jobs]==job_order()[:34]
    assert jobs[-1]['exit_code']==201
    prefix=deepcopy(report);prefix.update(status='running',jobs=deepcopy(jobs[:-1]))
    check_parent(prefix,identity)
    fatal=re.findall(r'unknown location\(0\): fatal error: in "([^"]+)": std::runtime_error: Caught Unexpected Exception',log)
    aborted=re.findall(r'Test case "([^"]+)" was aborted with:',log)
    missing=re.findall(r'^f: Failed to open (.+)$',log,re.M)
    assert len(fatal)==len(aborted)==len(missing)==9
    assert set(fatal)==set(aborted)==set(FULL_CDT_CASES)
    assert set(missing)=={'/work/'+FULL_CDT}
    assert len(re.findall(r'^(?:unknown location\(0\)|[^\n]+\(\d+\)): (?:fatal )?error:',log,re.M))==9
    assert 'Test module "Master Test Suite" has failed with:\n  403 test cases out of 420 passed\n  9 test cases out of 420 failed\n  8 test cases out of 420 skipped\n  9 test cases out of 420 aborted\n  305731 assertions out of 305740 passed\n  9 assertions out of 305740 failed' in log
    return prefix


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repair-runtime-fixtures',action='store_true',required=True)
    args=parser.parse_args(); parent=args.parent.resolve(strict=True);root=args.output.absolute()
    assert args.repair_runtime_fixtures and parent==BASE/'local-native-profile-retest-attempt-8'
    assert root==BASE/'local-native-profile-retest-attempt-9' and not root.exists() and not root.is_symlink()
    assert shutil.disk_usage(BASE).free>20*1024**3
    original,proof,monitor=[json.loads((parent/n).read_text()) for n in
        ('report.json','download-verified.json','monitor.json')]
    identity=sha(parent/'source-identity-final.json')
    report=check_fixture_failure(original,proof,monitor,(parent/'worker-round-1.log').read_text(),identity)
    assert sha(parent/'candidate-evidence.tar.gz')==proof['sha256']
    assert (parent/'candidate-evidence.tar.gz').stat().st_size==proof['bytes']
    donor=BASE/'local-native-profile-retest-attempt-7'
    donor_report=json.loads((donor/'report.json').read_text())
    assert sha(donor/'source-identity-final.json')==identity
    assert donor_report['binaries_sha256']==report['binaries_sha256']
    dependencies=inventory(donor)
    runtime=BASE/'runtime-preflight-attempt-3'
    actual=json.loads((runtime/'report.json').read_text());runtime_plan=json.loads((runtime/'input.json').read_text())
    assert actual['status']=='runtime-preflight-passed-not-release' and actual['runtime_rounds']==[1,2,3]
    assert actual['inputs_unchanged'] is True and all(j['exit_code']==0 for j in actual['jobs'])
    assert runtime_plan['binaries_sha256']==report['binaries_sha256']
    assert runtime_plan['source_identity_sha256']==identity
    runtime_proof=json.loads((runtime/'download-verified.json').read_text())
    assert runtime_proof['status']=='passed' and sha(runtime/'smoke-evidence.tar.gz')==runtime_proof['sha256']
    # A failure/silent replacement cannot become a prefix cache. Both exact
    # owned containers must be absent before copying independent artifacts.
    from v1_fast_regression import inspect_owned
    assert not inspect_owned('pureeos-v1-local-native-profile-retest-attempt-8')
    assert not inspect_owned('pureeos-v1-local-native-profile-retest-attempt-7')
    assert not inspect_owned('pureeos-v1-runtime-preflight-attempt-3')
    plan=json.loads((parent/'input.json').read_text());assert plan['image']==IMAGE
    source_identity=json.loads((parent/'source-identity-final.json').read_text())
    assert all(sha(parent/'source'/p)==d for p,d in source_identity['files_sha256'].items())
    assert all(sha(parent/'build'/p)==d for p,d in report['binaries_sha256'].items())
    root.mkdir(mode=0o700)
    subprocess.run(['cp','-a','--reflink=auto',str(parent/'source'),str(root/'source')],check=True,timeout=900)
    for relative,digest in report['binaries_sha256'].items():
        target=root/'build'/relative;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(parent/'build'/relative,target);target.chmod(0o700);assert sha(target)==digest
    copy_dependencies(donor,root,dependencies)
    (root/'runtime-dependencies.json').write_text(json.dumps(dependencies,indent=2))
    for name in ('source-identity-final.json','repair-overlay.tar.gz','verified-runtime-overlay.tar.gz'):
        shutil.copyfile(parent/name,root/name)
    shutil.copytree(parent/'frozen-overlay',root/'frozen-overlay')
    frozen=root/'frozen-runners';frozen.mkdir()
    runner_names=('prepare_v1_same_build_resume.py','v1_native_profile_retest_guest.py',
                  'v1_native_profile_retest_host.py','v1_fast_regression.py','v1_local_release_workflows.py',
                  'v1_runtime_dependencies.py')
    for name in runner_names:shutil.copyfile(Path(__file__).with_name(name),frozen/name)
    runner_identity={name:sha(frozen/name) for name in runner_names}
    (root/'runner-identity.json').write_text(json.dumps(runner_identity,indent=2))
    with tarfile.open(root/'runner-sources.tar.gz','x:gz') as stream:
        for name in runner_names:stream.add(frozen/name,arcname=name,recursive=False)
    logs={}
    for job in report['jobs']:
        path=parent/(job['name']+'.log');logs[job['name']]=sha(path)
        shutil.copyfile(path,root/path.name);assert sha(root/path.name)==logs[job['name']]
    jobs=deepcopy(report['jobs'])
    for job in jobs:job['origin']='same-build-parent'
    resume=dict(parent=str(parent),parent_archive_sha256=proof['sha256'],parent_report_sha256=sha(parent/'report.json'),
        source_identity_sha256=identity,binaries_sha256=report['binaries_sha256'],jobs=jobs,logs_sha256=logs,
        reason='Restore omitted generated runtime test data. Failed worker job is NOT reused; all earlier green jobs retain original times/logs.',
        excluded_failed_job=deepcopy(original['jobs'][-1]),runtime_dependencies_donor=str(donor),
        runtime_dependencies_sha256=sha(root/'runtime-dependencies.json'),
        runtime_correction_archive_sha256=runtime_proof['sha256'])
    plan.update(runner_sha256=runner_identity['v1_native_profile_retest_guest.py'],same_build_resume=resume,
                runtime_dependencies_sha256=sha(root/'runtime-dependencies.json'),runner_identity=runner_identity)
    (root/'input.json').write_text(json.dumps(plan,indent=2));(root/'tmp').mkdir()
    assert sha(root/'source-identity-final.json')==identity
    assert all(sha(root/'source'/p)==d for p,d in source_identity['files_sha256'].items())
    print(json.dumps(dict(status='prepared-same-build-resume',reused_jobs=len(jobs),total_jobs=len(job_order()),
                         source_identity_sha256=identity,release_ready=False)),flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
