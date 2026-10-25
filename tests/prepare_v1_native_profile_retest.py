"""Independent finite resource-query fix and full-profile candidate. Failed cache is never a pass."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import time
import traceback

from prepare_v1_engine_profile_retest import OVERLAY as ENGINE_OVERLAY, profile_audit, require_absent, verify_tree
from v1_local_release_workflows import IMAGE, PROGRAMS, read_progress, save, sha

NAME='local-native-profile-retest-attempt-7'
PARENT='local-native-profile-retest-attempt-6'
OVERLAY=ENGINE_OVERLAY+('unittests/pureeos_governance_tests.cpp',
    'unittests/pureeos_resource_split_tests.inc','unittests/pureeos_committee_boundary_tests.inc',
    'unittests/CMakeLists.txt','libraries/chain/resource_limits.cpp','unittests/resource_limits_test.cpp',
    'CMakeLists.txt','libraries/version/CMakeLists.txt','libraries/version/include/eosio/version/version.hpp',
    'libraries/version/src/version_impl.cpp.in')
RUNNERS=('prepare_v1_native_profile_retest.py','v1_native_profile_retest_guest.py',
    'v1_native_profile_retest_host.py','prepare_v1_engine_profile_retest.py','v1_local_release_workflows.py')


def failed_parent_jobs():
    return ['configure','build-node-controller','build-version-provenance']


def failed_parent_is_cache_only(actual, proof, monitor):
    jobs=actual.get('jobs',[])
    return (actual.get('status')=='failed' and proof.get('status')=='passed'
        and proof.get('candidate_test_status')=='failed'
        and monitor.get('status')=='finished-and-archived' and monitor.get('candidate_status')=='failed'
        and [j.get('name') for j in jobs]==failed_parent_jobs()
        and jobs[-1].get('exit_code')==1
        and all(j.get('exit_code')==0 for j in jobs[:-1])
        and len(actual.get('binaries_sha256',{}))==11)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();os.umask(0o077)
    parent=args.parent.resolve(strict=True);root=args.output.resolve()
    assert parent.name==PARENT and root.name==NAME and parent.parent==root.parent
    assert root.parent.name=='PureEOS-v1-hardening-20261022' and shutil.disk_usage(root.parent).free>25*1024**3
    require_absent('pureeos-v1-'+NAME)
    root.mkdir(exist_ok=False)
    report=dict(status='freezing-independent-repair',release_ready=False,parent_candidate_status='pending-failed-version-provenance',
        scope=__doc__,no_cloud_purchase=True,
        started_utc=datetime.now(timezone.utc).isoformat())
    try:
        save(root/'preparation.json',report)
        source=Path(__file__).resolve().parents[1]
        overlay=root/'frozen-overlay';runners=root/'frozen-runners';runners.mkdir()
        for name in OVERLAY:
            dst=overlay/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/name,dst)
        for name in RUNNERS:shutil.copyfile(source/'tests'/name,runners/name)
        overlay_hashes={n:sha(overlay/n) for n in OVERLAY};runner_hashes={n:sha(runners/n) for n in RUNNERS}
        identity=dict(scope=__doc__,parent_candidate_status='failed',overlay_sha256=overlay_hashes,
            runner_sha256=runner_hashes,parent_source_identity_sha256=sha(parent/'source-identity-final.json'),
            parent_input_sha256=sha(parent/'input.json'))
        save(root/'runner-identity.json',identity);save(root/'static-profile-audit.json',profile_audit(overlay))
        for name in ('repair-overlay.tar.gz','verified-runtime-overlay.tar.gz'):
            with tarfile.open(root/name,'x:gz') as stream:
                for n in OVERLAY:stream.add(overlay/n,arcname=n,recursive=False)
        with tarfile.open(root/'runner-sources.tar.gz','x:gz') as stream:
            for n in RUNNERS:stream.add(runners/n,arcname=n,recursive=False)
        report['status']='waiting-for-parent-archive';save(root/'preparation.json',report)
        deadline=time.monotonic()+3*3600
        while True:
            monitor=read_progress(parent/'monitor.json')
            if monitor.get('status')=='finished-and-archived':break
            state=subprocess.check_output(['systemctl','--user','show','pureeos-v1-'+PARENT+'.service',
                '-p','ActiveState','--value'],text=True,timeout=15).strip()
            if state in ('inactive','failed'):
                monitor=read_progress(parent/'monitor.json')
                if monitor.get('status')!='finished-and-archived':raise RuntimeError('Parent stopped without closed archive')
                break
            if time.monotonic()>deadline:raise TimeoutError('Parent evidence wait expired')
            time.sleep(15)
        actual,proof=(read_progress(parent/n) for n in ('report.json','download-verified.json'))
        assert failed_parent_is_cache_only(actual,proof,monitor), 'Only explicitly diagnosed closed failed version check supplies cache'
        archive=parent/'candidate-evidence.tar.gz'
        assert archive.stat().st_size==proof['bytes'] and sha(archive)==proof['sha256']
        assert sha(parent/'input.json')==identity['parent_input_sha256']
        assert sha(parent/'source-identity-final.json')==identity['parent_source_identity_sha256']
        require_absent('pureeos-v1-'+PARENT)
        report.update(parent_candidate_status='failed',parent_archive_sha256=proof['sha256'])
        previous=read_progress(parent/'source-identity-final.json');verify_tree(parent/'source',previous['files_sha256'])
        for n,d in actual['binaries_sha256'].items():assert sha(parent/'build'/n)==d
        report['status']='copying-owned-closed-cache';save(root/'preparation.json',report)
        for folder in ('source','build'):
            subprocess.run(['cp','-a','--reflink=auto',str(parent/folder),str(root/folder)],check=True,timeout=1800)
        expected=dict(previous['files_sha256'])
        changed=[]
        for n,d in overlay_hashes.items():
            assert n in expected
            if expected[n]!=d:
                shutil.copyfile(overlay/n,root/'source'/n);changed.append(n)
            expected[n]=d
        verify_tree(root/'source',expected)
        save(root/'source-identity-final.json',dict(scope='Finite CPU/NET query range repair, fixtures and explicit source-build version provenance; objective budgets unchanged',
            parent_candidate_status='failed',parent_archive_sha256=proof['sha256'],
            parent_identity_sha256=identity['parent_source_identity_sha256'],files_sha256=expected,
            relative_symlinks={},changes=changed,overlays=list(OVERLAY),toolchain=previous['toolchain']))
        programs={n:actual['binaries_sha256'][f'programs/{n}/{n}'] for n in PROGRAMS}
        assert all(sha(root/'build/programs'/n/n)==d for n,d in programs.items())
        assert all((root/'build/programs'/n/n).stat().st_ino!=(parent/'build/programs'/n/n).stat().st_ino for n in PROGRAMS)
        assert all(sha(overlay/n)==d for n,d in overlay_hashes.items())
        assert all(sha(runners/n)==d for n,d in runner_hashes.items())
        old=read_progress(parent/'input.json');assert old['image']==IMAGE
        (root/'tmp').mkdir()
        save(root/'input.json',dict(status='frozen-independent-repair-not-release',image=IMAGE,
            runner_sha256=runner_hashes['v1_native_profile_retest_guest.py'],overlay_sha256=overlay_hashes,
            parent_programs_sha256=programs,parent_core_archive_sha256=proof['sha256'],parent_candidate_status='failed',
            production_resource_cpp_sha256=overlay_hashes['libraries/chain/resource_limits.cpp'],
            contracts_sha256=old['contracts_sha256'],sdk_report_sha256=old['sdk_report_sha256'],
            no_cloud_purchase=True,tps_cancelled=True,compilation_jobs=1,
            cache_reuse='Independent file copies ONLY; all 59 jobs must rerun. Failed parent remains failed'))
        report.update(status='prepared',changed_source_paths=changed,
            finished_utc=datetime.now(timezone.utc).isoformat());save(root/'preparation.json',report)
        os.execv('/usr/bin/python3',['/usr/bin/python3',str(runners/'v1_native_profile_retest_host.py'),'--root',str(root)])
    except Exception as error:
        report.update(status='failed-or-not-started',error=str(error),traceback=traceback.format_exc(),
            finished_utc=datetime.now(timezone.utc).isoformat());save(root/'preparation.json',report);return 1


if __name__=='__main__':raise SystemExit(main())
