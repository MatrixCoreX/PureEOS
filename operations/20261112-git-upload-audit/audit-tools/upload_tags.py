from pathlib import Path
import json,subprocess,os,re,datetime
D=Path(__file__).resolve().parent;R=D.parents[1];rows=json.loads((D/'TAG_INVENTORY.json').read_text());state=D/'TAG_RESULT.json';S=json.loads(state.read_text())if state.exists()else{'status':'IN_PROGRESS','tags':[]};env={**os.environ,'GIT_OPTIONAL_LOCKS':'0','GIT_TERMINAL_PROMPT':'0'}
def git(p,*args):
 q=subprocess.run(['git','-c','gc.auto=0','-c','pack.threads=2','-c','pack.windowMemory=64m','-C',p,*args],capture_output=True,env=env,timeout=300);assert q.returncode==0,(p,args,q.stderr.decode()[-1500:]);return q.stdout.decode()
def save():state.write_text(json.dumps(S,indent=2)+'\n')
remote={}
for e in rows:
 if not e['tags']:continue
 url='https://github.com/MatrixCoreX/'+e['destination']+'.git'
 if e['destination']not in remote:remote[e['destination']]={l.split()[1]:l.split()[0]for l in git(e['path'],'ls-remote','--tags',url).splitlines()}
 for name,oid,peeled,typ in e['tags']:
  if any(x['source']==e['path']and x['original_ref']==name for x in S['tags']):continue
  m=remote[e['destination']];slug=re.sub('[^a-z0-9]+','-',str(Path(e['path']).relative_to(R)).lower()).strip('-')[-90:]
  ref=name if m.get(name)==oid else 'refs/tags/backup/git-upload-20261112/'+slug+'/'+name.removeprefix('refs/tags/')
  if m.get(ref)!=oid:
   assert ref not in m,('Remote archive tag collision',ref)
   git(e['path'],'push',url,oid+':'+ref);m[ref]=oid;print('UPLOADED_TAG',e['destination'],ref,flush=True);action='UPLOADED_ARCHIVE_TAG'
  else:action='EXACT_REMOTE_TAG_ALREADY_PRESENT'
  S['tags'].append({'source':e['path'],'destination':e['destination'],'original_ref':name,'object':oid,'peeled':peeled,'object_type':typ,'remote_ref':ref,'action':action});save()
for dest in remote:
 e=next(x for x in rows if x['destination']==dest);m={l.split()[1]:l.split()[0]for l in git(e['path'],'ls-remote','--tags','https://github.com/MatrixCoreX/'+dest+'.git').splitlines()}
 for x in S['tags']:
  if x['destination']==dest:assert m.get(x['remote_ref'])==x['object'];x['remote_ref_verified']=True
S.update(status='PASS_ALL_LOCAL_TAGS_VERIFIED',verified_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat());save();print('ALL_TAGS_VERIFIED',len(S['tags']),flush=True)
