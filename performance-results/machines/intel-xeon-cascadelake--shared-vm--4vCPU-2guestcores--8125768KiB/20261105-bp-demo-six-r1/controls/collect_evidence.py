"""Copy only named public evidence from the retained host, with byte hashes."""
from pathlib import Path
import sys,subprocess,shlex,json,tarfile,io,hashlib,datetime
D=Path(__file__).resolve().parent;W=D.parents[1]
sys.path.insert(0,str(W/'release-work-20261105/system-signup-r1'));from remote import host
names=sys.argv[1:]
assert names and len(names)==len(set(names))
for n in names:assert '/'not in n and n.endswith(('.json','.png','.log','.py','.mjs','.cjs'))and 'private'not in n.lower()
code='''from pathlib import Path
import sys,json,io,tarfile,re,hashlib
K=Path('/opt/pureeos-bp-demo-six-20261105-r1');names=json.load(sys.stdin);b=io.BytesIO()
with tarfile.open(fileobj=b,mode='w:gz')as t:
 for n in names:
  assert '/'not in n and 'private'not in n.lower()
  p=K/n;assert p.is_file()and not p.is_symlink(),'Named public evidence missing: '+n;data=p.read_bytes()
  if not n.endswith('.png'):
   assert not re.search(rb'PVT_[A-Za-z0-9_]+|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----|(?<![A-Za-z0-9_])5[HJK][1-9A-HJ-NP-Za-km-z]{49}(?![A-Za-z0-9_])',data),'Secret-shaped evidence rejected: '+n
  info=tarfile.TarInfo(n);info.size=len(data);info.mode=0o644;t.addfile(info,io.BytesIO(data))
sys.stdout.buffer.write(b.getvalue())
'''
r=subprocess.run(host.ssh()+['python3','-c',shlex.quote(code)],input=json.dumps(names).encode(),capture_output=True)
if r.returncode:
 print(r.stderr.decode(errors='replace'),file=sys.stderr);raise SystemExit(r.returncode)
E=D/'evidence';E.mkdir(exist_ok=True);manifest={}
with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:gz')as t:
 for m in t.getmembers():
  assert m.isfile()and m.name in names;data=t.extractfile(m).read();p=E/m.name
  if p.exists():assert p.read_bytes()==data,'Existing evidence differs: '+m.name
  else:p.write_bytes(data)
  manifest[m.name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
assert set(manifest)==set(names)
index=D/'EVIDENCE_DOWNLOAD_INDEX.json';old=json.loads(index.read_text())if index.exists()else {'files':{}}
old['files'].update(manifest);old.update(status='PASS',UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),remote_root='/opt/pureeos-bp-demo-six-20261105-r1',scope='Explicit public evidence only; no keys, signed packets, raw chain databases or passwords.')
index.write_text(json.dumps(old,indent=2)+'\n');print('BYTE_VERIFIED_PUBLIC_EVIDENCE '+str(len(manifest)))
