"""Stage only this approved public case; never copies any private server data."""
from pathlib import Path
import hashlib
import io
import json
import shlex
import subprocess
import tarfile

D = Path(__file__).resolve().parent
R = D.parents[1]
S = R / 'release-work-20261102/cheap-vps-35bp-history-r1'
files = {
    'AUTHORIZATION.json': D / 'AUTHORIZATION.json',
    'AUTHORIZED_PREFLIGHT.json': D / 'AUTHORIZED_PREFLIGHT.json',
    'ONE_SHOT_REVIEW.zh-CN.md': D / 'ONE_SHOT_REVIEW.zh-CN.md',
    'ISOLATED_QUALIFICATION_R8_RESULT.json': D / 'ISOLATED_QUALIFICATION_R8_RESULT.json',
    'CONTROLLER_TEST_RESULT.json': D / 'CONTROLLER_TEST_RESULT.json',
    'bank_upgrade.py': D / 'bank_upgrade.py',
    'candidate/eosio.token.wasm': D / 'reviewed-artifact/eosio.token.wasm',
    'candidate/eosio.token.raw.abi': D / 'reviewed-artifact/eosio.token.raw.abi',
    'candidate/eosio.token.abi': D / 'reviewed-artifact/eosio.token.abi',
}
manifest = {'source_commit': '47545adb3ce1185e2f145858c6b0ee444b3110d4',
            'github_direct_ref_verified': True,
            'files': {n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in files.items()}}
(D / 'AUTHORIZED_STAGE_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
files['AUTHORIZED_STAGE_MANIFEST.json'] = D / 'AUTHORIZED_STAGE_MANIFEST.json'
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w') as tar:
    for name, path in files.items():
        data = path.read_bytes()
        item = tarfile.TarInfo(name)
        item.size = len(data)
        item.mode = 0o600
        tar.addfile(item, io.BytesIO(data))

program = r'''
from pathlib import Path
import os,sys,io,tarfile,json,hashlib,base64,urllib.request,py_compile
os.umask(0o077)
R=Path('/opt/pureeos-testnet-bank-issuance-20261119-r1')
assert not R.exists(), 'Existing case must be inspected, never replaced'
R.mkdir(mode=0o700)
for m in tarfile.open(fileobj=io.BytesIO(sys.stdin.buffer.read()),mode='r:').getmembers():
 pass
# Re-open the in-memory archive once; no extractall/path traversal is allowed.
'''
# The archive must be consumed once, so bind its bytes before iterating.
program = program[:program.index('for m in tarfile.')] + r'''
blob=sys.stdin.buffer.read()
with tarfile.open(fileobj=io.BytesIO(blob),mode='r:') as t:
 for m in t.getmembers():
  assert m.isfile() and not m.name.startswith('/') and '..' not in Path(m.name).parts
  p=R/m.name;assert p.resolve().is_relative_to(R.resolve())
  p.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
  assert not p.exists();p.write_bytes(t.extractfile(m).read());p.chmod(0o600)
manifest=json.loads((R/'AUTHORIZED_STAGE_MANIFEST.json').read_text())
for n,h in manifest['files'].items():assert hashlib.sha256((R/n).read_bytes()).hexdigest()==h
request=urllib.request.Request('http://127.0.0.1:8890/v1/chain/get_raw_abi',data=b'{"account_name":"eosio.token"}',headers={'Content-Type':'application/json'})
with urllib.request.urlopen(request,timeout=10) as f:a=json.load(f)
assert a['code_hash']=='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
assert a['abi_hash']=='2d69b533e9ac8e557abfde5c034988dfc8a9d3e7bc7f8656fcf866cea2dda856'
original=R/'original';original.mkdir(mode=0o700)
raw=base64.b64decode(a['abi']);assert hashlib.sha256(raw).hexdigest()==a['abi_hash']
(original/'eosio.token.raw.abi').write_bytes(raw)
runtime=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/contracts/eosio.token')
code=(runtime/'eosio.token.wasm').read_bytes();assert hashlib.sha256(code).hexdigest()==a['code_hash']
(original/'eosio.token.wasm').write_bytes(code)
(original/'eosio.token.abi').write_bytes((runtime/'eosio.token.abi').read_bytes())
py_compile.compile(str(R/'bank_upgrade.py'),doraise=True)
r={'status':'PASS_APPROVED_CASE_STAGED_NO_CHAIN_WRITE','root':str(R),'source_commit':manifest['source_commit'],'original':{k:a[k]for k in ['code_hash','abi_hash']},'files_verified':len(manifest['files'])}
(R/'STAGED.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
'''
ssh = ['ssh', '-i', str(S / 'ssh-key'), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
       '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + str(S / 'known_hosts'),
       '-o', 'ConnectTimeout=15', 'root@66.245.194.132', 'python3 -c ' + shlex.quote(program)]
assert not (D / 'STAGE_RESULT.json').exists()
p = subprocess.run(ssh, input=buf.getvalue(), capture_output=True, timeout=90)
(D / 'STAGE_RESULT.json').write_bytes(p.stdout)
(D / 'STAGE_RESULT.stderr').write_bytes(p.stderr)
assert p.returncode == 0, 'Staging refusal retained; no automatic rerun'
print(p.stdout.decode())
