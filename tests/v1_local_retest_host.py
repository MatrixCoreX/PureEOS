"""Monitor one exact local container and preserve its evidence; no cloud calls."""
from datetime import datetime, timezone
import argparse
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import tarfile
import time

NAME='pureeos-v1-local-cold-warm-retest-attempt-1'
LABEL='org.pureeos.task'


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True);args=parser.parse_args()
    root=args.root.resolve(strict=True);base=root.parent
    source=Path(__file__).resolve().parents[1];runner=source/'tests/v1_local_retest_guest.py'
    assert root.name=='local-cold-warm-retest-attempt-1' and base.name=='PureEOS-v1-hardening-20261022'
    plan=json.loads((root/'input.json').read_text());assert sha(runner)==plan['runner_sha256']
    assert not (root/'launch.json').exists(),'Never overwrite or restart an existing candidate'
    existing=subprocess.run(['sudo','-n','docker','container','inspect',NAME],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    assert existing.returncode!=0,'Do not replace an existing container'
    command=['sudo','-n','docker','run','--rm','--name='+NAME,'--label='+LABEL+'='+NAME,
        '--pull=never','--network=none','--read-only','--log-driver=none','--user=1000:1000',
        '--cpus=6','--memory=6500m','--memory-swap=7524m','--pids-limit=512',
        '--tmpfs','/tmp:rw,size=128m,uid=1000,gid=1000',
        '--env','TMPDIR=/work/tmp','--env','PYTHONDONTWRITEBYTECODE=1',
        '--mount',f'type=bind,src={root},dst=/work',
        '--mount',f'type=bind,src={runner},dst=/runner.py,readonly',
        '--mount',f'type=bind,src={base}/contracts-safe-eviction-attempt-2/contracts,dst=/contracts,readonly',
        '--mount',f'type=bind,src={base}/wasm-attempt-6,dst=/sdk,readonly',
        '--workdir=/work',plan['image'],'python3','/runner.py']
    def interrupted(signum,frame):raise InterruptedError('Local test service received stop signal')
    signal.signal(signal.SIGTERM,interrupted);signal.signal(signal.SIGINT,interrupted)
    record=dict(status='running',release_ready=False,started_utc=datetime.now(timezone.utc).isoformat(),
        image=plan['image'],container=NAME,runner_sha256=sha(runner),no_cloud_purchase=True)
    (root/'launch.json').write_text(json.dumps(record,indent=2))
    process=None
    try:
        with (root/'container-driver.log').open('x') as log:
            process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
            deadline=time.monotonic()+8.5*3600
            while process.poll() is None:
                if time.monotonic()>deadline:raise TimeoutError('Local test deadline reached')
                value={**record,'updated_utc':datetime.now(timezone.utc).isoformat()}
                path=root/'report.json'
                if path.is_file():
                    try:
                        report=json.loads(path.read_text());value.update(candidate_status=report['status'],
                            completed_jobs=len(report['jobs']),last_jobs=report['jobs'][-3:])
                    except json.JSONDecodeError:pass
                pending=root/'monitor.pending';pending.write_text(json.dumps(value,indent=2));pending.replace(root/'monitor.json')
                time.sleep(30)
        record['container_exit_code']=process.returncode
    except Exception as error:
        record.update(status='interrupted-or-failed',error=str(error))
    finally:
        inspected=subprocess.run(['sudo','-n','docker','container','inspect',NAME],
            text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=30)
        if inspected.returncode==0:
            detail=json.loads(inspected.stdout)[0]
            assert detail['Config']['Labels'].get(LABEL)==NAME and detail['Image']==plan['image']
            subprocess.run(['sudo','-n','docker','stop','--time=60',NAME],check=True,timeout=90)
        if process is not None:
            try:process.wait(timeout=30)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=15)
    report_path=root/'report.json'
    if report_path.is_file():report=json.loads(report_path.read_text())
    else:
        report=dict(status='failed',error='Container ended before candidate report',jobs=[],release_ready=False)
        report_path.write_text(json.dumps(report,indent=2))
    if record.get('container_exit_code')!=0 and report['status']=='passed':
        raise RuntimeError('Do not certify a nonzero container exit as passed')
    assert sha(runner)==plan['runner_sha256'],'Runner changed during tests'
    archive=root/'candidate-evidence.tar.gz'
    with tarfile.open(archive,'x:gz') as stream:
        for path in root.iterdir():
            if path.is_file() and path.suffix in ('.json','.log'):
                stream.add(path,arcname=path.name,recursive=False)
        stream.add(root/'repair-overlay.tar.gz',arcname='repair-overlay.tar.gz',recursive=False)
        for name in ('nodeos','cleos','keosd','helios-util'):
            path=root/'build/programs'/name/name
            if path.is_file():stream.add(path,arcname='build/programs/'+name+'/'+name,recursive=False)
        for name in ('parallel_validation_test','parallel_wasm_worker_test','native_savanna_test',
                     'parallel_access_hints_test','parallel_worker_signals_test','parallel_immutable_input_test'):
            path=root/'build/unittests'/name
            if path.is_file():stream.add(path,arcname='build/unittests/'+name,recursive=False)
    with tarfile.open(archive) as stream:
        for path,digest in report.get('binaries_sha256',{}).items():
            member=stream.getmember('build/'+path)
            assert member.isfile() and hashlib.file_digest(stream.extractfile(member),'sha256').hexdigest()==digest
    proof=dict(status='passed',candidate_test_status=report['status'],release_ready=False,
        local=str(archive),bytes=archive.stat().st_size,sha256=sha(archive),runtime_kind='local-container',
        verified_utc=datetime.now(timezone.utc).isoformat())
    (root/'download-verified.json').write_text(json.dumps(proof,indent=2))
    record.update(status='finished-and-archived',candidate_status=report['status'],finished_utc=datetime.now(timezone.utc).isoformat())
    (root/'monitor.json').write_text(json.dumps(record,indent=2))
    print(json.dumps(record),flush=True)
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
