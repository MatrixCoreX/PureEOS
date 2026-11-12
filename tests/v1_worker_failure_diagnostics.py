"""Bounded observation of the three v8 failures; never a replacement release pass."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import threading
import time


ROOT = '/opt/pureeos-v1-worker-failure-diagnostics-20261022-attempt-1'
BINARY = '/opt/pureeos-v1-integrated-20261022/build/unittests/parallel_wasm_worker_test'
EXPECTED = '65b24b26a1881f407bb87ff4bf0a67a8ae92eaf4fdb18a3a90c3c62feb838662'
CASES = ('parallel_sync_private_tests/sync_leaf_wasm_intervals_overlap_on_two_and_four_threads',
    'parallel_exit_oracle_tests/ordinary_and_start_exit_codes_match_original_vm_and_actual_adoption',
    'parallel_environment_oracle_tests/multiple_roots_and_both_envelopes_keep_original_transaction_bytes_in_every_frame')
UNIT = 'pureeos-unified-TBMyRV-v1-worker-failure-diagnostics-1'
DEADLINE = '2026-10-22T14:45:00+00:00'


def now():
    return datetime.now(timezone.utc)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def save(path, value):
    path=Path(path);temporary=path.with_suffix('.pending')
    temporary.write_text(json.dumps(value,indent=2));temporary.replace(path)


def guest():
    root=Path(ROOT)
    report=dict(status='running-diagnostics-not-release',release_ready=False,
        started_utc=now().isoformat(),binary_sha256=sha(BINARY),cases=CASES,rounds=[],
        scope='Three fixed rounds of the exact three failed v8 cases; no budget/code change or TPS',
        original_full_matrix_status='failed',deadline_unchanged=DEADLINE)
    assert report['binary_sha256']==EXPECTED
    assert (datetime.fromisoformat(DEADLINE)-now()).total_seconds()>1500
    save(root/'report.json',report)
    stop=threading.Event()
    def sample():
        with (root/'cpu-samples.jsonl').open('x') as stream:
            while not stop.is_set():
                row=dict(utc=now().isoformat(),monotonic=time.monotonic(),loadavg=Path('/proc/loadavg').read_text().strip(),
                    cpu=Path('/proc/stat').read_text().splitlines()[0],
                    pressure=Path('/proc/pressure/cpu').read_text().strip())
                stream.write(json.dumps(row)+'\n');stream.flush();stop.wait(1)
    sampler=threading.Thread(target=sample);sampler.start()
    try:
        for number in range(1,4):
            temporary=root/f'tmp-{number}';temporary.mkdir()
            command=[BINARY,'--run_test='+':'.join(CASES),'--report_level=detailed',
                '--log_level=test_suite','--color_output=no','--','--eos-vm-jit']
            started=now().isoformat()
            with (root/f'round-{number}.log').open('x') as log:
                process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,
                    env={**os.environ,'TMPDIR':str(temporary)})
                try:
                    code=process.wait(timeout=300)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    try:process.wait(timeout=15)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
                    raise RuntimeError('Diagnostic timed out; do not retry until success')
            report['rounds'].append(dict(round=number,exit_code=code,passed=code==0,
                started_utc=started,finished_utc=now().isoformat()))
            save(root/'report.json',report)
        assert sha(BINARY)==EXPECTED
        report['status']='diagnostics-completed-not-release-pass'
    except Exception as error:
        report.update(status='diagnostics-incomplete-not-release-pass',error=str(error))
    finally:
        stop.set();sampler.join()
        report['finished_utc']=now().isoformat();save(root/'report.json',report)


def controller(output):
    source=Path(__file__).resolve()
    task=source.parents[2]/'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'
    sys.path.insert(0,str(task));import cloud_round as c
    assert c.DEADLINE==DEADLINE
    output=output.resolve();output.mkdir(exist_ok=False)
    candidate=output.parent/'integrated-build-cloud-v8'
    core=c.read(candidate/'report.json');proof=c.read(candidate/'download-verified.json')
    assert core['status']=='failed' and proof['status']=='passed' and c.sha(proof['local'])==proof['sha256']
    assert core['binaries_sha256']['unittests/parallel_wasm_worker_test']==EXPECTED
    assert (datetime.fromisoformat(DEADLINE)-now()).total_seconds()>1800
    ssh=c.old.ssh_args('c')
    c.remote('c',['python3','-c','import pathlib;pathlib.Path('+repr(ROOT)+').mkdir(mode=0o700)'],30)
    subprocess.run(['rsync','-a','--chmod=F600','-e',shlex.join(ssh[:-1]),str(source),ssh[-1]+':'+ROOT+'/guest.py'],check=True,timeout=60)
    verify='import pathlib,hashlib;assert hashlib.sha256(pathlib.Path('+repr(ROOT+'/guest.py')+').read_bytes()).hexdigest()=='+repr(sha(source))
    c.remote('c',['python3','-c',verify],30)
    c.remote('c',['systemd-run','--quiet','--unit='+UNIT,'--service-type=exec',
        '--property=RuntimeMaxSec=1050','--property=MemoryMax=6500M','--property=MemorySwapMax=512M',
        '--property=KillMode=control-group','--property=StandardOutput=append:'+ROOT+'/driver.log',
        '--property=StandardError=inherit','python3',ROOT+'/guest.py','--guest'],30)
    c.save(output/'launch.json',dict(instance_id=c.IDS['c'],unit=UNIT,root=ROOT,
        utc=now().isoformat(),deadline_unchanged=DEADLINE,source_sha256=sha(source)))
    while (datetime.fromisoformat(DEADLINE)-now()).total_seconds()>1200:
        code="""import pathlib,json,subprocess
p=pathlib.Path(ROOT);r=p/'report.json'
print(json.dumps(dict(report=json.loads(r.read_text()) if r.exists() else None,
 unit=subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','-p','Result'],text=True))))
""".replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
        value=json.loads(c.remote('c',['python3','-c',code],40))
        value['updated_utc']=now().isoformat();save(output/'monitor.json',value)
        if any(state in value['unit'] for state in ('ActiveState=inactive','ActiveState=failed')):
            break
        time.sleep(45)
    else:
        c.remote('c',['systemctl','stop',UNIT],60)
        raise RuntimeError('Original evidence reserve reached; no extension')
    code="""import pathlib,tarfile,json,hashlib,subprocess
p=pathlib.Path(ROOT)
assert subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip() in ('failed','inactive')
a=p/'evidence.tar.gz';files=[f for f in p.iterdir() if f.is_file() and f.suffix in ('.json','.jsonl','.log','.py')]
with tarfile.open(a,'x:gz') as t:
 for f in files:t.add(f,arcname=f.name,recursive=False)
with a.open('rb') as s:digest=hashlib.file_digest(s,'sha256').hexdigest()
print(json.dumps(dict(archive=str(a),sha256=digest,bytes=a.stat().st_size)))
""".replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
    archive=json.loads(c.remote('c',['python3','-c',code],120));local=output/'evidence.tar.gz'
    subprocess.run(['rsync','-a','--chmod=F600','-e',shlex.join(ssh[:-1]),ssh[-1]+':'+archive['archive'],str(local)],check=True,timeout=120)
    assert local.stat().st_size==archive['bytes'] and sha(local)==archive['sha256']
    with tarfile.open(local) as stream:
        assert all(m.isfile() and Path(m.name).name==m.name for m in stream.getmembers())
        report=json.load(stream.extractfile('report.json'))
    save(output/'report.json',report)
    save(output/'download-verified.json',dict(status='passed',release_ready=False,local=str(local),
        instance_id=c.IDS['c'],**archive,verified_utc=now().isoformat()))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guest',action='store_true');parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.guest:guest()
    elif args.output:controller(args.output)
    else:parser.error('--output is required outside guest mode')
