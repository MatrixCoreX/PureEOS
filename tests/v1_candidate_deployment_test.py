"""Deploy only an archived, passed candidate on exact existing A; no purchase/TPS."""
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

SOURCE = Path(__file__).resolve().parents[1]
TASK = SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'

ROOT = '/opt/pureeos-v1-deployment-20261022-attempt-1'
SUPPORT = ('run_observer_admission_smoke.py', 'pureeos_committed_policy_observer.py',
    'pureeos_large_multinode_local_test.py', 'pureeos_multinode_local_test.py',
    'pureeos_single_choice_local_test.py', 'native_savanna_genesis_test.py')
PROGRAMS = ('nodeos', 'cleos', 'keosd', 'helios-util')
MIN_START_SECONDS = 45 * 60
EVIDENCE_RESERVE_SECONDS = 20 * 60


def bounded_runtime(remaining):
    assert remaining >= MIN_START_SECONDS, 'Insufficient original window; no extension'
    return min(5400, int(remaining)-EVIDENCE_RESERVE_SECONDS)


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_candidate(proof, report):
    assert proof['status'] == 'passed' and proof['candidate_test_status'] == 'passed'
    assert report['status'] == 'passed', 'Do not deploy a failing or incomplete core'
    jobs = {v['name']: v['exit_code'] for v in report['jobs']}
    required = ['configure', 'build-node-controller']
    required += [f'{name}-round-{n}' for n in range(1, 4) for name in
        ('access-hints', 'worker-signals', 'immutable-input', 'controller', 'worker', 'blacklist-and-history')]
    required += [f'leaf-overlap-stability-{n}' for n in range(1, 11)]
    required += [f'immutable-input-integration-probe-{n}' for n in range(1,4)]
    assert len(jobs) == len(report['jobs']) and all(code == 0 for code in jobs.values()), 'Duplicate or failed candidate job'
    assert all(jobs.get(name) == 0 for name in required), 'Incomplete same-candidate core matrix'
    assert all(f'programs/{name}/{name}' in report['binaries_sha256'] for name in PROGRAMS)


def seconds_left(context):
    return (datetime.fromisoformat(context.DEADLINE) - context.now()).total_seconds()


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Keep --help and offline predicates independent of private task state.
    sys.path.insert(0, str(TASK))
    import cloud_round as c
    UNIT = c.PREFIX + '-v1-deployment-attempt-1'
    args.output.mkdir(exist_ok=False)
    report = dict(status='waiting-for-passed-candidate', release_ready=False,
        deadline_unchanged=c.DEADLINE, no_new_vm_or_extension=True, tps_cancelled=True,
        scope='Fresh 1->4 independently keyed loopback nodes on existing A, JIT/4 workers; full ranking, REX rentals, resource split, quorum loss/recovery; not 40 nodes or TPS',
        support_sha256={name: sha(SOURCE / 'tests' / name) for name in SUPPORT},
        driver_sha256=sha(__file__), started_utc=c.now().isoformat())
    def save():
        report['updated_utc'] = c.now().isoformat()
        c.atomic(args.output / 'report.json', report)
    def transfer(local, remote, download=False):
        ssh = c.old.ssh_args('a')
        src, dst = (ssh[-1]+':'+remote, str(local)) if download else (str(local), ssh[-1]+':'+remote)
        subprocess.run(['rsync', '-a', '--partial', '--chmod=F600', '-e', shlex.join(ssh[:-1]), src, dst],
            check=True, timeout=min(600, max(30, int(seconds_left(c))-120)))
    try:
        save()
        while not (args.candidate / 'download-verified.json').exists():
            assert seconds_left(c) >= MIN_START_SECONDS, 'Not enough original cloud time left for a fresh deployment; no extension'
            save()
            time.sleep(45)
        proof = c.read(args.candidate / 'download-verified.json')
        core = c.read(args.candidate / 'report.json')
        check_candidate(proof, core)
        assert sha(proof['local']) == proof['sha256']
        assert seconds_left(c) >= MIN_START_SECONDS, 'Preserve execution and twenty minutes for shutdown/evidence'
        assert all(sha(SOURCE/'tests'/name) == digest for name, digest in report['support_sha256'].items())
        contracts = args.output.parent / 'contracts-safe-eviction-attempt-2/contracts'
        plan = c.read(args.candidate / 'input.json')
        assert all(sha(contracts/name) == digest for name, digest in plan['contracts_sha256'].items())
        payload = args.output / 'payload.tar.gz'
        hashes = {}
        with tarfile.open(proof['local']) as source, tarfile.open(payload, 'x:gz') as target:
            for name in PROGRAMS:
                member = source.getmember(f'build/programs/{name}/{name}')
                assert member.isfile()
                value = source.extractfile(member).read()
                digest = hashlib.sha256(value).hexdigest()
                assert digest == core['binaries_sha256'][f'programs/{name}/{name}']
                info = tarfile.TarInfo(f'programs/{name}.elf'); info.size=len(value); info.mode=0o700
                target.addfile(info, io.BytesIO(value)); hashes[info.name]=digest
            for name in SUPPORT:
                target.add(SOURCE/'tests'/name, arcname='tests/'+name, recursive=False)
                hashes['tests/'+name]=report['support_sha256'][name]
            for name, digest in plan['contracts_sha256'].items():
                target.add(contracts/name, arcname='contracts/'+name, recursive=False)
                hashes['contracts/'+name]=digest
            manifest = json.dumps(dict(files_sha256=hashes, core_archive_sha256=proof['sha256'],
                deadline=c.DEADLINE), indent=2).encode()
            info = tarfile.TarInfo('input.json'); info.mode=0o600; info.size=len(manifest)
            target.addfile(info, io.BytesIO(manifest))
        preflight = '''import pathlib,subprocess,shutil,json
p=pathlib.Path(ROOT);assert not p.exists(),'Never replace an existing deployment'
for n in ('nodeos','nodeos.elf'):
 assert subprocess.run(['pgrep','-x',n],stdout=subprocess.DEVNULL).returncode==1,'Existing node process on A'
assert shutil.disk_usage('/opt').free>12*1024**3
p.mkdir(mode=0o700);print(json.dumps(dict(status='private-directory-ready')))
'''.replace('ROOT', repr(ROOT))
        c.remote('a', ['python3','-c',preflight], 40)
        transfer(payload, ROOT+'/payload.tar.gz')
        extract = '''import pathlib,tarfile,hashlib,json
p=pathlib.Path(ROOT);a=p/'payload.tar.gz';assert hashlib.sha256(a.read_bytes()).hexdigest()==DIGEST
with tarfile.open(a) as t:
 m=t.getmembers();assert all(x.isfile() and not pathlib.Path(x.name).is_absolute() and '..' not in pathlib.Path(x.name).parts for x in m)
 t.extractall(p,members=m,filter='data')
for name,digest in json.loads((p/'input.json').read_text())['files_sha256'].items():
 assert hashlib.sha256((p/name).read_bytes()).hexdigest()==digest,name
for f in (p/'programs').iterdir():f.chmod(0o700)
'''.replace('ROOT',repr(ROOT)).replace('DIGEST',repr(sha(payload)))
        c.remote('a',['python3','-c',extract],90)
        runtime = bounded_runtime(seconds_left(c))
        c.remote('a',['systemd-run','--quiet','--unit='+UNIT,'--service-type=exec',
            '--property=RuntimeMaxSec='+str(runtime),'--property=TimeoutStopSec=90',
            '--property=KillMode=control-group','--property=MemoryMax=6500M',
            '--property=StandardOutput=append:'+ROOT+'/runner.log','--property=StandardError=inherit',
            'python3',ROOT+'/tests/run_observer_admission_smoke.py','--programs',ROOT+'/programs',
            '--contracts',ROOT+'/contracts','--output',ROOT+'/run','--workers','4','--workflow','full'],40)
        report.update(status='deployed-running', instance_id=c.IDS['a'], remote_root=ROOT,
            unit=UNIT, core_archive_sha256=proof['sha256'], payload_sha256=sha(payload), files_sha256=hashes)
        save()
        while seconds_left(c) > 240:
            code = '''import pathlib,json,subprocess
p=pathlib.Path(ROOT);r=p/'run/report.json';log=p/'runner.log'
v=dict(unit=subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','-p','SubState','-p','Result'],text=True),
 report=json.loads(r.read_text()) if r.exists() else None)
if log.exists():
 with log.open('rb') as f:f.seek(max(0,log.stat().st_size-3000));v['tail']=f.read().decode(errors='replace')
print(json.dumps(v))
'''.replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
            monitor=json.loads(c.remote('a',['python3','-c',code],40))
            c.atomic(args.output/'monitor.json',{**monitor,'updated_utc':c.now().isoformat()})
            if any(v in monitor['unit'] for v in ('ActiveState=inactive','ActiveState=failed')):
                break
            time.sleep(45)
        else:
            raise RuntimeError('Cloud deadline imminent; automatic cost guard remains unchanged')
        archive_code = '''import pathlib,tarfile,hashlib,json,subprocess
p=pathlib.Path(ROOT);assert subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip() in ('inactive','failed')
a=p/'public-evidence.tar.gz';assert not a.exists()
files=[p/'input.json',p/'runner.log']+list((p/'run').rglob('report.json'))+list((p/'run').rglob('progress.json'))
with tarfile.open(a,'x:gz') as t:
 for f in files:
  if f.is_file():t.add(f,arcname=str(f.relative_to(p)),recursive=False)
with a.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
print(json.dumps(dict(archive=str(a),sha256=digest,bytes=a.stat().st_size)))
'''.replace('ROOT',repr(ROOT)).replace('UNIT',repr(UNIT))
        archive=json.loads(c.remote('a',['python3','-c',archive_code],90))
        downloaded=args.output/'public-evidence.tar.gz'
        transfer(downloaded,archive['archive'],download=True)
        assert downloaded.stat().st_size==archive['bytes'] and sha(downloaded)==archive['sha256']
        report['evidence_archive']={**archive,'local':str(downloaded)}
        save()
        with tarfile.open(downloaded) as stream:
            members=stream.getmembers()
            assert all(m.isfile() and not Path(m.name).is_absolute() and '..' not in Path(m.name).parts for m in members)
            assert all(not any(x in Path(m.name).parts for x in ('wallet','config','state')) for m in members)
            summary=json.load(stream.extractfile('run/report.json'))
            detailed=json.load(stream.extractfile(next(m for m in members if m.name!='run/report.json' and m.name.endswith('/report.json'))))
        report.update(status='passed' if summary['status']=='passed' and detailed['status']=='passed' and detailed['processes_stopped'] else 'failed',
            deployment_summary=summary, checks=len(detailed['checks']),
            processes_stopped=detailed['processes_stopped'], evidence_archive={**archive,'local':str(downloaded)})
        c.save(args.output/'deployment-detail.json',detailed)
    except Exception as error:
        report.update(status='failed-or-not-started',error=str(error),traceback=traceback.format_exc())
    save()
    print(json.dumps({k:v for k,v in report.items() if k in ('status','error','checks','processes_stopped','remote_root')}))
    return 0 if report['status']=='passed' else 1


if __name__=='__main__':raise SystemExit(main())
