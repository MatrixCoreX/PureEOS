"""Bounded observer/archive downloader. No VM purchases, cleanup or test restarts."""
from datetime import datetime
import argparse
import fcntl
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time
import traceback

SOURCE = Path(__file__).resolve().parents[1]
TASK = SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'
sys.path.insert(0, str(TASK))
import cloud_round as c

OUTPUT = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/integrated-build-cloud')
ROOT = '/opt/pureeos-v1-integrated-20261022'
UNIT = c.PREFIX + '-v1-integrated-build'


def publish(value):
    pending = OUTPUT / 'monitor.pending'
    pending.write_text(json.dumps(value, indent=2))
    pending.replace(OUTPUT / 'monitor.json')


def archive():
    code = '''import pathlib,tarfile,json,hashlib,subprocess
p=pathlib.Path(ROOT);a=p/'candidate-evidence.tar.gz';m=p/'archive.json'
if m.exists(): print(m.read_text());raise SystemExit(0)
assert not a.exists(),'Never replace an incomplete archive'
unit=subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip()
assert unit not in ('active','activating','deactivating'),'Only archive stopped build/test processes'
files=[x for x in p.iterdir() if x.is_file() and x.suffix in ('.json','.log','.gz') and x.name!='candidate-evidence.tar.gz']
build=pathlib.Path(json.loads((p/'report.json').read_text()).get('reused_private_build_cache',str(p/'build')))
assert build in (p/'build',pathlib.Path('/opt/pureeos-v1-integrated-20261022/build'))
for name in ('nodeos','cleos','keosd','helios-util'):
 f=build/'programs'/name/name
 if f.is_file(): files.append(f)
for name in ('parallel_validation_test','parallel_access_hints_test','parallel_worker_signals_test','parallel_wasm_worker_test','native_savanna_test','parallel_immutable_input_test'):
 f=build/'unittests'/name
 if f.is_file(): files.append(f)
with tarfile.open(a,'x:gz') as t:
 for f in files:t.add(f,arcname=str(f.relative_to(p)) if f.is_relative_to(p) else 'build/'+str(f.relative_to(build)),recursive=False)
 t.add(p/'overlay',arcname='overlay')
with a.open('rb') as s: digest=hashlib.file_digest(s,'sha256').hexdigest()
v=dict(status='archived-not-a-release-pass',archive=str(a),bytes=a.stat().st_size,sha256=digest,
       files=[str(f.relative_to(p)) if f.is_relative_to(p) else 'build/'+str(f.relative_to(build)) for f in files],source_note='Explicit changed-source overlay plus input manifest; full vendor baseline not a self-contained release source package')
m.write_text(json.dumps(v,indent=2));print(json.dumps(v))
'''.replace('ROOT', repr(ROOT)).replace('UNIT', repr(UNIT))
    remote = json.loads(c.remote('c', ['python3', '-c', code], 600))
    target = OUTPUT / 'candidate-evidence.tar.gz'
    if not target.exists():
        partial = OUTPUT / 'candidate-evidence.download.part'
        ssh = c.old.ssh_args('c')
        subprocess.run(['rsync', '-a', '--partial', '--chmod=F600', '-e', shlex.join(ssh[:-1]),
            ssh[-1] + ':' + remote['archive'], str(partial)], check=True, timeout=600)
        assert partial.stat().st_size == remote['bytes'] and c.sha(partial) == remote['sha256']
        partial.rename(target)
    assert target.stat().st_size == remote['bytes'] and c.sha(target) == remote['sha256']
    with tarfile.open(target) as stream:
        for member in stream:
            assert not Path(member.name).is_absolute() and '..' not in Path(member.name).parts
            assert member.isfile() or member.isdir(), 'No links/devices in explicit evidence archive'
        report = json.load(stream.extractfile('report.json'))
    (OUTPUT / 'report.json').write_text(json.dumps(report, indent=2))
    c.save(OUTPUT / 'download-verified.json', {**remote, 'status': 'passed',
        'instance_id': c.IDS['c'], 'local': str(target), 'verified_utc': c.now().isoformat(),
        'candidate_test_status': report['status'], 'release_ready': False})


def main():
    global OUTPUT, ROOT, UNIT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt', type=int, choices=(1, 2, 3, 4, 5, 6, 7, 8), default=1)
    args = parser.parse_args()
    if args.attempt > 1:
        OUTPUT = OUTPUT.with_name(OUTPUT.name + f'-v{args.attempt}')
        ROOT += f'-v{args.attempt}'
        UNIT += f'-v{args.attempt}'
    with (OUTPUT / 'watch.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if (OUTPUT / 'download-verified.json').exists():
            proof = c.read(OUTPUT / 'download-verified.json')
            assert c.sha(proof['local']) == proof['sha256']
            return 0
        while c.now() < datetime.fromisoformat(c.DEADLINE):
            try:
                code = '''import pathlib,json,subprocess
p=pathlib.Path(ROOT);r=p/'report.json';b=p/'build-node-controller.log'
v=dict(report=json.loads(r.read_text()) if r.exists() else None,
 unit=subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','-p','SubState','-p','Result'],text=True))
if b.exists():
 with b.open('rb') as f:f.seek(max(0,b.stat().st_size-4000));v['build_tail']=f.read().decode(errors='replace')
print(json.dumps(v))
'''.replace('ROOT', repr(ROOT)).replace('UNIT', repr(UNIT))
                value = json.loads(c.remote('c', ['python3', '-c', code], 40))
                value.update(updated_utc=c.now().isoformat(), deadline_unchanged=c.DEADLINE, release_ready=False)
                publish(value)
                if (value.get('report') or {}).get('status') in ('passed', 'failed') or any(
                        state in value['unit'] for state in ('ActiveState=inactive', 'ActiveState=failed')):
                    archive(); return 0
            except Exception as error:
                publish(dict(status='observation-error-not-a-test-pass', error=str(error),
                    traceback=traceback.format_exc(), updated_utc=c.now().isoformat(), release_ready=False))
            time.sleep(45)
    publish(dict(status='original-deadline-reached-no-extension', release_ready=False,
        updated_utc=c.now().isoformat()))
    return 1


if __name__ == '__main__': raise SystemExit(main())
