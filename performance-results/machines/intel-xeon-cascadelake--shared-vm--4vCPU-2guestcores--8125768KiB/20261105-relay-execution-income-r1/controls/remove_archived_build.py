"""Remove only an unchanged, inactive, fully archived generated CDT build."""
from pathlib import Path
import os,json,hashlib,subprocess,shutil,datetime
B=Path('/home/guagua/AntelopeIO');D=B/'release-work-20261105/relay-execution-income-r1'
for name in ['FINAL_UI_PUBLIC.json','PUBLIC_ASSET_DIGESTS.json','FINAL_INVARIANTS.json','CURRENT_AUDIT.json']:
 assert json.loads((D/'evidence'/name).read_text())['status']=='PASS',name
record=json.loads((D/'CLEANUP_BUILD_ARCHIVE.json').read_text());assert record['status']=='ARCHIVE_VERIFIED_NOT_REMOVED'
S=Path(record['source']);assert S==B/'cdt/build-pureeos'and S.is_dir()and not S.is_symlink()
archive=Path(record['archive']);assert archive.is_file()and archive.stat().st_size==record['archive_bytes']
with archive.open('rb')as f:assert hashlib.file_digest(f,'sha256').hexdigest()==record['archive_SHA256']
assert not subprocess.check_output(['git','ls-files','--','build-pureeos'],cwd=B/'cdt',text=True)
def process_refs():
 refs=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit()or int(proc.name)==os.getpid():continue
  for p in [proc/'cwd',proc/'exe',*list((proc/'fd').glob('*'))]:
   try:value=os.readlink(p)
   except OSError:continue
   if value==str(S)or value.startswith(str(S)+'/'):refs.append({'PID':int(proc.name),'reference':str(p),'path':value})
 return refs
assert not process_refs(),'Generated build is in use'
entries={e['path']:e for e in record['entries']};observed=set()
for p in S.rglob('*'):
 if not(p.is_file()or p.is_symlink()):continue
 n=str(p.relative_to(S));observed.add(n);e=entries[n]
 if p.is_symlink():assert e['kind']=='symlink'and os.readlink(p)==e['target']
 else:
  assert e['kind']=='file'and p.stat().st_size==e['bytes']
  with p.open('rb')as f:assert hashlib.file_digest(f,'sha256').hexdigest()==e['sha256'],n
assert observed==set(entries)
refs=process_refs();assert not refs
before=shutil.disk_usage(B);allocated=int(subprocess.check_output(['du','-s','-B1',str(S)],text=True).split()[0])
shutil.rmtree(S);assert not S.exists();after=shutil.disk_usage(B)
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'removed_path':str(S),'scope':'Only inactive ignored generated build. Full verified restore archive retained on local mounted Windows partition on the same NVMe. CDT source/Git, current remote toolchain, chain data and user worktrees retained.','archive':str(archive),'archive_SHA256':record['archive_SHA256'],'entries_reverified':len(entries),'tracked_files':0,'process_references':refs,'removed_allocated_bytes':allocated,'root_free_before':before.free,'root_free_after':after.free,'net_free_increase':after.free-before.free}
(D/'CLEANUP_BUILD_REMOVAL.json').write_text(json.dumps(result,indent=2)+'\n');(archive.parent/'CDT_BUILD_REMOVAL.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
