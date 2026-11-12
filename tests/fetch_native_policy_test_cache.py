"""Read-only export of frozen native-test link inputs; no running build mutation."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[1]
TASK = SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'
sys.path.insert(0, str(TASK))
import cloud_round as c


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--attempt',type=int,choices=(1,2),default=1)
    parser.add_argument('--collect-existing',action='store_true')
    args=parser.parse_args()
    suffix='' if args.attempt==1 else '-v2'
    output = Path('/run/media/guagua/Windows/PureEOS-v1-hardening-20261022/native-policy-portable'+suffix)
    output.mkdir(exist_ok=args.collect_existing)
    code = '''import pathlib,json,hashlib,shlex,subprocess,tarfile
cache=pathlib.Path('/opt/pureeos-unified-TBMyRV-compiler-cache-after-sanitizers-v2')
root=pathlib.Path(REMOTE_ROOT);root.mkdir(mode=0o700)
commands=json.loads((cache/'compile_commands.json').read_text())
compile=next(v for v in commands if v['file'].endswith('/pureeos_governance_tests.cpp'))
link=subprocess.check_output(['ninja','-C',str(cache),'-t','commands','native_savanna_test'],text=True).splitlines()[-1]
files=set()
for value in shlex.split(link):
 p=pathlib.Path(value)
 if p.suffix not in ('.o','.a'):continue
 p=p if p.is_absolute() else cache/p
 assert p.is_relative_to(cache),'Unexpected non-cache input'
 assert p.is_file(),str(p)
 files.add(p)
files.update(p for p in (cache/'unittests/include').rglob('*') if p.is_file())
files.update(p for p in cache.rglob('*') if p.is_file() and p.suffix in ('.h','.hpp','.inc'))
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
hashes={str(p.relative_to(cache)):sha(p) for p in files}
plan=dict(cache=str(cache),compile=compile,link=link,inputs_sha256=hashes)
(root/'input.json').write_text(json.dumps(plan,indent=2))
archive=root/'native-policy-cache.tar.gz'
with tarfile.open(archive,'x:gz',dereference=True) as stream:
 stream.add(root/'input.json',arcname='input.json')
 for p in sorted(files):stream.add(p,arcname='cache/'+str(p.relative_to(cache)),recursive=False)
assert all(sha(cache/p)==expected for p,expected in hashes.items())
print(json.dumps(dict(status='archived-read-only-inputs',archive=str(archive),bytes=archive.stat().st_size,sha256=sha(archive))))
'''.replace('REMOTE_ROOT',repr('/opt/pureeos-v1-policy-portable-20261022'+suffix))
    if args.collect_existing:
        code='''import pathlib,json,hashlib,tarfile,subprocess
root=pathlib.Path(REMOTE_ROOT);a=root/'native-policy-cache.tar.gz'
subprocess.run(['gzip','-t',str(a)],check=True)
plan=json.loads((root/'input.json').read_text())
with tarfile.open(a) as stream:
 for member in stream:
  if member.name.startswith('cache/'):
   with stream.extractfile(member) as f:got=hashlib.file_digest(f,'sha256').hexdigest()
   assert got==plan['inputs_sha256'][member.name.removeprefix('cache/')]
with a.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
print(json.dumps(dict(status='existing-archive-verified',archive=str(a),bytes=a.stat().st_size,sha256=digest)))
'''.replace('REMOTE_ROOT',repr('/opt/pureeos-v1-policy-portable-20261022'+suffix))
    result = json.loads(c.remote('c', ['python3', '-c', code], 600))
    ssh = c.old.ssh_args('c')
    target = output / 'native-policy-cache.tar.gz'
    subprocess.run(['rsync', '-a', '--partial', '--chmod=F600', '-e', shlex.join(ssh[:-1]),
        ssh[-1] + ':' + result['archive'], str(target)], check=True, timeout=600)
    assert target.stat().st_size == result['bytes'] and c.sha(target) == result['sha256']
    (output / 'download-verified.json').write_text(json.dumps({**result, 'status':'passed',
        'instance_id':c.IDS['c'], 'local':str(target), 'verified_utc':c.now().isoformat()}, indent=2))
    print(json.dumps(dict(status='download-verified',bytes=result['bytes'],sha256=result['sha256'])))


if __name__ == '__main__': main()
