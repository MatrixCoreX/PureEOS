"""Same-candidate native contract matrix; optionally shard across existing B/C/D."""
import argparse
from datetime import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import time
import traceback

from v1_candidate_deployment_test import MIN_START_SECONDS, bounded_runtime, check_candidate

SOURCE=Path(__file__).resolve().parents[1]

ROOT='/opt/pureeos-v1-native-matrix-20261022-attempt-1'
SUITES=('pureeos_genesis_tests','pureeos_governance_tests','pureeos_governance_contract_tests',
    'pureeos_single_choice_contract_tests','pureeos_resource_split_tests','pureeos_committee_boundary_tests',
    'pureeos_reward_halving_tests','pureeos_reward_activation_tests','pureeos_sequential_rotation_tests',
    'pureeos_blacklist_current_policy_tests')

SHARDS = {'b': (4, 6, 9), 'c': (0, 1, 2, 3, 5, 7), 'd': (8,)}


def selected_suites(role=None):
    indices = [i for values in SHARDS.values() for i in values]
    assert sorted(indices) == list(range(len(SUITES))), 'Shards must cover every suite exactly once'
    return SUITES if role is None else tuple(SUITES[i] for i in SHARDS[role])


def contract_root(root):
    assert root in tuple('/opt/pureeos-v1-integrated-20261022'+s for s in ('-v4','-v5','-v6','-v7','-v8'))
    return root + '/contracts'


def main():
    os.umask(0o077)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--shard', choices=tuple(SHARDS), help='Existing exact role only; no new VM')
    args=parser.parse_args()
    sys.path.insert(0,str(SOURCE.parent/'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'))
    import cloud_round as c
    UNIT=c.PREFIX+'-v1-native-matrix-attempt-1'
    args.output.mkdir(exist_ok=False)
    role=args.shard or 'c';suites=selected_suites(args.shard)
    root=ROOT+('-shard-'+role if args.shard else '')
    unit=UNIT+('-shard-'+role if args.shard else '')
    program=root+'/native_savanna_test'
    report=dict(status='waiting-for-passed-candidate',scope=__doc__,release_ready=False,
        started_utc=c.now().isoformat(),deadline_unchanged=c.DEADLINE,no_new_vm_or_extension=True,
        driver_sha256=c.sha(__file__),suites=suites,role=role,shard=args.shard)
    def save():report.update(updated_utc=c.now().isoformat());c.atomic(args.output/'report.json',report)
    try:
        save()
        while not (args.candidate/'download-verified.json').exists():
            assert (datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds()>=MIN_START_SECONDS,'Original cloud window insufficient; no extension'
            save();time.sleep(45)
        proof=c.read(args.candidate/'download-verified.json');core=c.read(args.candidate/'report.json')
        check_candidate(proof,core);assert c.sha(proof['local'])==proof['sha256']
        plan=c.read(args.candidate/'input.json')
        contracts=contract_root(plan.get('compiled_contracts_candidate_root',c.read(args.candidate/'launch.json')['root']))
        local_contracts=args.output.parent/'contracts-safe-eviction-attempt-2/contracts'
        assert all(c.sha(local_contracts/name)==value for name,value in plan['contracts_sha256'].items())
        payload=args.output/'payload.tar.gz'
        with tarfile.open(proof['local']) as archive, tarfile.open(payload,'x:gz') as target:
            member=archive.getmember('build/unittests/native_savanna_test');assert member.isfile()
            value=archive.extractfile(member).read();digest=hashlib.sha256(value).hexdigest()
            assert digest==core['binaries_sha256']['unittests/native_savanna_test']
            info=tarfile.TarInfo('native_savanna_test');info.size=len(value);info.mode=0o700
            target.addfile(info,io.BytesIO(value))
            for name in plan['contracts_sha256']:
                assert not Path(name).is_absolute() and '..' not in Path(name).parts
                target.add(local_contracts/name,arcname='contracts/'+name,recursive=False)
        runtime=bounded_runtime((datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds())
        preflight='import pathlib,shutil;p=pathlib.Path('+repr(root)+');assert not p.exists();assert shutil.disk_usage("/opt").free>3*1024**3;p.mkdir(mode=0o700)'
        c.remote(role,['python3','-c',preflight],40)
        import shlex,subprocess
        ssh=c.old.ssh_args(role)
        subprocess.run(['rsync','-a','--partial','--chmod=F600','-e',shlex.join(ssh[:-1]),str(payload),ssh[-1]+':'+root+'/payload.tar.gz'],check=True,timeout=300)
        config=dict(root=root,program=program,contracts=contracts,digest=digest,suites=suites,
            hashes=plan['contracts_sha256'],payload_sha256=c.sha(payload),role=role,deadline=c.DEADLINE,
            core_archive_sha256=proof['sha256'])
        setup='''import pathlib,tarfile,hashlib,json,os
v=json.loads(CONFIG);root=pathlib.Path(v['root']);a=root/'payload.tar.gz'
assert hashlib.sha256(a.read_bytes()).hexdigest()==v['payload_sha256']
with tarfile.open(a) as t:
 members=t.getmembers();assert all(m.isfile() and not pathlib.Path(m.name).is_absolute() and '..' not in pathlib.Path(m.name).parts for m in members)
 t.extractall(root,filter='data')
contracts=pathlib.Path(v['contracts'])
if v['role']!='c':
 assert not contracts.parent.exists(),'Never replace another candidate directory'
 contracts.parent.mkdir(mode=0o700);os.rename(root/'contracts',contracts)
for name,digest in v['hashes'].items():
 assert hashlib.sha256((contracts/name).read_bytes()).hexdigest()==digest,name
program=pathlib.Path(v['program']);assert hashlib.sha256(program.read_bytes()).hexdigest()==v['digest'];program.chmod(0o700)
'''.replace('CONFIG',repr(json.dumps(config)))
        c.remote(role,['python3','-c',setup],90)
        guest='''import datetime,hashlib,json,os,pathlib,subprocess,traceback,time
v=json.loads(CONFIG)
ROOT=v['root'];PROGRAM=v['program'];CONTRACTS=v['contracts'];DIGEST=v['digest'];SUITES=v['suites'];HASHES=v['hashes']
os.umask(0o077);root=pathlib.Path(ROOT);program=pathlib.Path(PROGRAM);contracts=pathlib.Path(CONTRACTS)
report=dict(status='running',scope='Same N1/N2/N4 Release/O3 native core and hardened safe-eviction WASM; virtual-clock contract boundaries, not P2P/TPS or real 24h waiting',jobs=[],program_sha256=DIGEST,suites=SUITES,contracts_sha256=HASHES,core_archive_sha256=v['core_archive_sha256'],role=v['role'],release_ready=False)
def sha(p):
 with pathlib.Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def save():(root/'report.json').write_text(json.dumps(report,indent=2))
try:
 assert sha(program)==DIGEST
 assert all(sha(contracts/n)==d for n,d in HASHES.items())
 save()
 for i,suite in enumerate(SUITES,1):
  remaining=(datetime.datetime.fromisoformat(v['deadline'])-datetime.datetime.now(datetime.timezone.utc)).total_seconds()-1200
  assert remaining>60,'Original deadline too close for next suite'
  started=time.monotonic()
  with (root/(str(i)+'-'+suite+'.log')).open('x') as log:
   result=subprocess.run([program,'--run_test='+suite,'--report_level=detailed','--log_level=test_suite','--color_output=no'],stdout=log,stderr=subprocess.STDOUT,timeout=min(1800,int(remaining)))
  report['jobs'].append(dict(suite=suite,exit_code=result.returncode,elapsed_seconds=time.monotonic()-started));save()
 assert sha(program)==DIGEST
 assert all(sha(contracts/n)==d for n,d in HASHES.items())
 report['status']='passed' if all(x['exit_code']==0 for x in report['jobs']) else 'failed'
except Exception as error:report.update(status='failed',error=str(error),traceback=traceback.format_exc())
report['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
raise SystemExit(0 if report['status']=='passed' else 1)
'''.replace('CONFIG',repr(json.dumps(config)))
        runtime=bounded_runtime((datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds())
        c.remote(role,['systemd-run','--quiet','--unit='+unit,'--service-type=exec',
            '--property=RuntimeMaxSec='+str(runtime),'--property=KillMode=control-group',
            '--property=MemoryMax=6500M','--property=StandardOutput=append:'+root+'/driver.log',
            '--property=StandardError=inherit','python3','-c',guest],40)
        report.update(status='running',instance_id=c.IDS[role],root=root,unit=unit,
            core_archive_sha256=proof['sha256'],program_sha256=digest,contracts_sha256=plan['contracts_sha256']);save()
        while (datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds()>240:
            code='import json,pathlib,subprocess;p=pathlib.Path('+repr(root)+');r=p/"report.json";print(json.dumps(dict(report=json.loads(r.read_text()) if r.exists() else None,unit=subprocess.check_output(["systemctl","show",'+repr(unit)+',"-p","ActiveState","-p","SubState","-p","Result"],text=True))))'
            value=json.loads(c.remote(role,['python3','-c',code],40))
            c.atomic(args.output/'monitor.json',{**value,'updated_utc':c.now().isoformat()})
            if any(s in value['unit'] for s in ('ActiveState=inactive','ActiveState=failed')):break
            time.sleep(45)
        else:raise RuntimeError('Original cloud deadline imminent')
        code='''import pathlib,subprocess,tarfile,hashlib,json
p=pathlib.Path(ROOT);assert subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip() in ('inactive','failed')
a=p/'evidence.tar.gz';assert not a.exists()
with tarfile.open(a,'x:gz') as t:
 for f in p.iterdir():
  if f.is_file() and f.suffix in ('.json','.log'):t.add(f,arcname=f.name,recursive=False)
with a.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
print(json.dumps(dict(path=str(a),sha256=digest,bytes=a.stat().st_size)))
'''.replace('ROOT',repr(root)).replace('UNIT',repr(unit))
        evidence=json.loads(c.remote(role,['python3','-c',code],90))
        ssh=c.old.ssh_args(role);download=args.output/'evidence.tar.gz'
        subprocess.run(['rsync','-a','--partial','--chmod=F600','-e',shlex.join(ssh[:-1]),ssh[-1]+':'+evidence['path'],str(download)],check=True,timeout=300)
        assert download.stat().st_size==evidence['bytes'] and c.sha(download)==evidence['sha256']
        report['evidence_archive']={**evidence,'local':str(download)};save()
        with tarfile.open(download) as archive:
            assert all(m.isfile() and '/' not in m.name and m.name not in ('.','..') for m in archive.getmembers())
            actual=json.load(archive.extractfile('report.json'))
        c.save(args.output/'native-detail.json',actual)
        assert actual['program_sha256']==digest and actual['contracts_sha256']==plan['contracts_sha256']
        assert actual['core_archive_sha256']==proof['sha256'] and actual['suites']==list(suites)
        assert [job['suite'] for job in actual['jobs']]==list(suites), 'Incomplete or duplicate shard'
        report.update(status=actual['status'],jobs=actual['jobs'])
    except Exception as error:report.update(status='failed-or-not-started',error=str(error),traceback=traceback.format_exc())
    save();print(json.dumps({k:v for k,v in report.items() if k in ('status','error','jobs')}))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
