from pathlib import Path
import json,subprocess,os,hashlib,datetime,configparser,io
D=Path(__file__).resolve().parent;P=json.loads((D/'PLAN.json').read_text());S=json.loads((D/'BACKUP_RESULT.json').read_text());env={**os.environ,'GIT_OPTIONAL_LOCKS':'0'};rows=[];staged=[]
def git(e,*args):
 x={**env,**({'GIT_ALTERNATE_OBJECT_DIRECTORIES':e['alternate_objects']}if e.get('alternate_objects')else{})};q=subprocess.run(['git','-C',e['path'],*args],capture_output=True,env=x,timeout=90);assert q.returncode==0,q.stderr.decode();return q.stdout
def tree(e,commit):return {r.split(b'\t',1)[1].decode():(r.split()[0].decode(),r.split()[2].decode())for r in git(e,'ls-tree','-r','-z',commit).split(b'\0')if r}
for e in P['entries']:
 if e['path']in S['snapshots']:
  commit=S['snapshots'][e['path']]['commit'];t=tree(e,commit);flags={r[2:].decode():r[:1].decode().upper()for r in git(e,'ls-files','-v','-z').split(b'\0')if r};sparse=[];checked=0
  index={r.split(b'\t',1)[1].decode():(r.split()[0].decode(),r.split()[1].decode())for r in git(e,'ls-files','--stage','-z').split(b'\0')if r}
  for f in e['files']:
   n=f['path']
   if f.get('deleted'):
    if flags.get(n)=='S':assert t.get(n)==index[n],('Sparse index source lost',e['path'],n);sparse.append(n)
    else:assert n not in t,('Deleted source retained',e['path'],n)
   else:
    b=git(e,'cat-file','blob',commit+':'+n)
    if'symlink'in f:assert b.decode()==f['symlink']
    elif n=='.gitmodules'and S['snapshots'][e['path']]['submodules']:
     cfg=configparser.RawConfigParser();cfg.read(Path(e['path'])/'.gitmodules')
     for binding in P['module_bindings']:
      if binding['parent']==e['path']and any(m['path']==binding['path']for m in S['snapshots'][e['path']]['submodules']):cfg.set(binding['section'],'url','https://github.com/MatrixCoreX/PureEOS.git')
     stream=io.StringIO();cfg.write(stream);assert b==stream.getvalue().encode(),('Archive dependency URL rewrite mismatch',e['path'])
    else:assert hashlib.sha256(b).hexdigest()==f['SHA256'],(e['path'],n)
   checked+=1
  rows.append({'source':e['path'],'commit':commit,'reviewed_paths_verified':checked,'sparse_unmaterialized_index_sources_preserved':sparse})
 if e['path']in S.get('staged',{}):
  commit=S['staged'][e['path']]['commit'];t=tree(e,commit);idx={r.split(b'\t',1)[1].decode():(r.split()[0].decode(),r.split()[1].decode())for r in git(e,'ls-files','--stage','-z').split(b'\0')if r};assert t==idx,('Original staged tree mismatch',e['path']);staged.append({'source':e['path'],'commit':commit,'exact_original_index_entries':len(idx)})
out={'status':'PASS_ARCHIVE_CONTENT_MATCHES_REVIEWED_SOURCE_AND_ORIGINAL_STAGED_TREES','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'working_snapshots':rows,'staged_snapshots':staged};(D/'CONTENT_VERIFICATION.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps({'status':out['status'],'working_snapshots':len(rows),'reviewed_paths':sum(x['reviewed_paths_verified']for x in rows),'staged_snapshots':len(staged),'sparse_preserved_paths':sum(len(x['sparse_unmaterialized_index_sources_preserved'])for x in rows)},indent=2))
