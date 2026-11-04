"""Remove only stopped private qualification data and duplicate candidate source trees."""
from pathlib import Path
import datetime,hashlib,json,os,shutil,subprocess
K=Path('/opt/pureeos-cap-veto-20261104-r1')
assert json.loads((K/'CAMPAIGN_RESULT.json').read_text())['status']=='PASS'
assert json.loads((K/'QUALIFICATION_RESULT.json').read_text())['status']=='PASS'
assert(K/'verified-public-evidence.tar.gz').is_file()
paths=[Path('/opt/kcap-q/r2'),K/'private-qualification-r1',K/'source',K/'source-failed-r1',K/'artifacts-failed-r1']
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit()or int(proc.name)==os.getpid():continue
 try:
  cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace');cwd=(proc/'cwd').resolve()
 except(OSError,RuntimeError):continue
 for p in paths:
  assert str(p)not in cmd and not cwd.is_relative_to(p),'Active process depends on cleanup path'
files={};allocated=0
for p in paths:
 if not p.exists():continue
 for child in p.rglob('*'):
  if child.is_file()and not child.is_symlink():allocated+=child.stat().st_blocks*512
 files[str(p)]={'exists':True}
before=shutil.disk_usage(K).free
for p in paths:
 if p.is_symlink():p.unlink()
 elif p.exists():shutil.rmtree(p)
after=shutil.disk_usage(K).free
record={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'removed':files,'allocated_removed_MiB':round(allocated/1048576,3),'net_free_increase_MiB':round((after-before)/1048576,3),'available_GiB':round(after/1073741824,3),'preserved':'Public chain/history/wallets/original26nodes, signed immutable campaign packets, candidate artifacts, verified public source/evidence archive and failures'}
(K/'CLEANUP_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
