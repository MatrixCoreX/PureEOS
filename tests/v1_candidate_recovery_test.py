"""Three fresh candidate recovery chains on existing B after its native shard; no TPS."""
import argparse
from datetime import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time
import traceback

from v1_candidate_deployment_test import PROGRAMS, check_candidate
from pureeos_historical_edge_cases_test import verify_case_set

SOURCE = Path(__file__).resolve().parents[1]

ROOT='/opt/pv1-recovery-v8'
SUPPORT=('pureeos_historical_edge_cases_test.py','pureeos_single_choice_local_test.py','native_savanna_genesis_test.py')


def complete(report):
    return (report.get('status')=='passed' and report.get('parallel_workers')==4
            and len(report.get('rounds',[]))==3
            and [r.get('round') for r in report['rounds']]==[1,2,3]
            and all(r.get('parallel_workers')==4 and verify_case_set(r) for r in report['rounds']))


def main():
    os.umask(0o077)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--native-shard',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    sys.path.insert(0,str(SOURCE.parent/'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'))
    import cloud_round as c
    UNIT=c.PREFIX+'-v1-recovery-v8'
    args.output.mkdir(exist_ok=False)
    report=dict(status='waiting-for-passed-candidate-and-finished-B-shard',release_ready=False,
                deadline_unchanged=c.DEADLINE,scope=__doc__,driver_sha256=c.sha(__file__),
                support_sha256={name:c.sha(SOURCE/'tests'/name) for name in SUPPORT})
    def left():return (datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds()
    def save():report['updated_utc']=c.now().isoformat();c.atomic(args.output/'report.json',report)
    def transfer(local,remote,download=False):
        ssh=c.old.ssh_args('b');src,dst=(ssh[-1]+':'+remote,str(local)) if download else (str(local),ssh[-1]+':'+remote)
        subprocess.run(['rsync','-a','--partial','--chmod=F600','-e',shlex.join(ssh[:-1]),src,dst],check=True,timeout=300)
    try:
        save()
        while True:
            assert left()>2400,'Preserve original time for three rounds and evidence; no extension'
            if (args.candidate/'download-verified.json').exists() and (args.native_shard/'report.json').exists():
                native=c.read(args.native_shard/'report.json')
                if native.get('status')=='passed' and native.get('evidence_archive'):break
                assert native.get('status') not in ('failed','failed-or-not-started'),'Native B shard failed; retain evidence before another chain'
            save();time.sleep(45)
        proof=c.read(args.candidate/'download-verified.json');core=c.read(args.candidate/'report.json')
        check_candidate(proof,core);assert c.sha(proof['local'])==proof['sha256']
        assert native['role']=='b' and native['core_archive_sha256']==proof['sha256']
        assert c.sha(native['evidence_archive']['local'])==native['evidence_archive']['sha256']
        plan=c.read(args.candidate/'input.json');contracts=args.output.parent/'contracts-safe-eviction-attempt-2/contracts'
        assert all(c.sha(contracts/n)==d for n,d in plan['contracts_sha256'].items())
        assert all(c.sha(SOURCE/'tests'/n)==d for n,d in report['support_sha256'].items())
        payload=args.output/'payload.tar.gz';hashes={}
        with tarfile.open(proof['local']) as source,tarfile.open(payload,'x:gz') as target:
            for name in PROGRAMS:
                member=source.getmember(f'build/programs/{name}/{name}');assert member.isfile()
                data=source.extractfile(member).read();digest=hashlib.sha256(data).hexdigest()
                assert digest==core['binaries_sha256'][f'programs/{name}/{name}']
                info=tarfile.TarInfo(f'build/programs/{name}/{name}');info.size=len(data);info.mode=0o700
                target.addfile(info,io.BytesIO(data));hashes[info.name]=digest
            for name in SUPPORT:
                target.add(SOURCE/'tests'/name,arcname='tests/'+name,recursive=False)
                hashes['tests/'+name]=report['support_sha256'][name]
            for name,digest in plan['contracts_sha256'].items():
                target.add(contracts/name,arcname='contracts/'+name,recursive=False);hashes['contracts/'+name]=digest
        config=dict(root=ROOT,unit=UNIT,sha256=hashes,payload_sha256=c.sha(payload),
                    core_archive_sha256=proof['sha256'],native_unit=native['unit'])
        setup='''import pathlib,json,subprocess,shutil
v=json.loads(CONFIG);p=pathlib.Path(v['root']);assert not p.exists()
assert subprocess.check_output(['systemctl','show',v['native_unit'],'-p','ActiveState','--value'],text=True).strip() in ('inactive','failed')
assert shutil.disk_usage('/opt').free>8*1024**3;p.mkdir(mode=0o700)
'''.replace('CONFIG',repr(json.dumps(config)))
        c.remote('b',['python3','-c',setup],40);transfer(payload,ROOT+'/payload.tar.gz')
        setup='''import pathlib,json,tarfile,hashlib
v=json.loads(CONFIG);p=pathlib.Path(v['root']);assert hashlib.sha256((p/'payload.tar.gz').read_bytes()).hexdigest()==v['payload_sha256']
with tarfile.open(p/'payload.tar.gz') as t:
 members=t.getmembers();assert all(m.isfile() and not pathlib.Path(m.name).is_absolute() and '..' not in pathlib.Path(m.name).parts for m in members)
 t.extractall(p,filter='data')
for name,digest in v['sha256'].items():assert hashlib.sha256((p/name).read_bytes()).hexdigest()==digest,name
for f in (p/'build/programs').glob('*/*'):f.chmod(0o700)
(p/'input.json').write_text(json.dumps(v,indent=2))
'''.replace('CONFIG',repr(json.dumps(config)))
        c.remote('b',['python3','-c',setup],90)
        runtime=min(3000,int(left())-1200);assert runtime>=1200
        c.remote('b',['systemd-run','--quiet','--unit='+UNIT,'--service-type=exec',
            '--property=RuntimeMaxSec='+str(runtime),'--property=TimeoutStopSec=90',
            '--property=KillMode=control-group','--property=MemoryMax=6500M',
            '--property=StandardOutput=append:'+ROOT+'/runner.log','--property=StandardError=inherit',
            'python3',ROOT+'/tests/pureeos_historical_edge_cases_test.py','--build-dir',ROOT+'/build',
            '--contracts-dir',ROOT+'/contracts','--report',ROOT+'/recovery.json',
            '--rounds','3','--parallel-workers','4'],40)
        report.update(status='running',root=ROOT,unit=UNIT,instance_id=c.IDS['b'],
                      core_archive_sha256=proof['sha256'],files_sha256=hashes);save()
        while left()>240:
            code='''import json,pathlib,subprocess
p=pathlib.Path(ROOT);f=p/'recovery.json';log=p/'runner.log'
print(json.dumps(dict(unit=subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','-p','Result'],text=True),
 report=json.loads(f.read_text()) if f.exists() else None,tail=log.read_text(errors='replace')[-2000:] if log.exists() else '')))
'''.replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
            value=json.loads(c.remote('b',['python3','-c',code],40));c.atomic(args.output/'monitor.json',value)
            if any(s in value['unit'] for s in ('ActiveState=inactive','ActiveState=failed')):break
            time.sleep(45)
        else:raise RuntimeError('Original cloud deadline imminent')
        code='''import pathlib,json,tarfile,hashlib,subprocess
p=pathlib.Path(ROOT);assert subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip() in ('inactive','failed')
v=json.loads((p/'input.json').read_text())
assert all(hashlib.sha256((p/name).read_bytes()).hexdigest()==digest for name,digest in v['sha256'].items())
a=p/'public-evidence.tar.gz';assert not a.exists()
with tarfile.open(a,'x:gz') as t:
 for f in [p/'input.json',p/'runner.log',p/'recovery.json']+list((p/'build/local-tests').rglob('report.json')):
  if f.is_file():t.add(f,arcname=str(f.relative_to(p)),recursive=False)
with a.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
print(json.dumps(dict(path=str(a),sha256=digest,bytes=a.stat().st_size)))
'''.replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
        evidence=json.loads(c.remote('b',['python3','-c',code],90));local=args.output/'public-evidence.tar.gz'
        transfer(local,evidence['path'],True)
        assert local.stat().st_size==evidence['bytes'] and c.sha(local)==evidence['sha256']
        report['evidence_archive']={**evidence,'local':str(local)};save()
        with tarfile.open(local) as archive:
            assert all(m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts for m in archive)
            actual=json.load(archive.extractfile('recovery.json'))
        c.save(args.output/'recovery-detail.json',actual)
        report.update(status='passed' if complete(actual) else 'failed',rounds=len(actual.get('rounds',[])),
                      parallel_evidence=[r.get('parallel_evidence') for r in actual.get('rounds',[])])
    except Exception as error:
        report.update(status='failed-or-not-started',error=str(error),traceback=traceback.format_exc())
    save();print(json.dumps({k:v for k,v in report.items() if k in ('status','error','rounds')}))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
