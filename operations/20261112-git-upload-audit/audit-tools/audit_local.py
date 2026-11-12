from pathlib import Path
import subprocess,json,os,concurrent.futures,urllib.parse,datetime
R=Path('/home/guagua/AntelopeIO');D=Path(__file__).resolve().parent;env={**os.environ,'GIT_OPTIONAL_LOCKS':'0'}
def run(p,*args):
 x=subprocess.run(['git','-C',str(p),*args],capture_output=True,env=env,timeout=120);return x.returncode,(x.stdout.decode(errors='replace') if '-z'in args else x.stdout.decode(errors='replace').strip()),x.stderr.decode(errors='replace').strip()
def safeurl(u):
 if '://'in u:
  x=urllib.parse.urlsplit(u);host=x.hostname or'';return urllib.parse.urlunsplit((x.scheme,host+(':'+str(x.port)if x.port else''),x.path,'',''))
 return u
commons={}
for p in R.iterdir():
 if p.is_dir()and(p/'.git').exists():
  c=run(p,'rev-parse','--git-common-dir');assert c[0]==0;cpath=(p/c[1]).resolve();commons.setdefault(str(cpath),p)
repos=[];worktrees={}
for c,p in sorted(commons.items()):
 remotes={n:safeurl(run(p,'remote','get-url',n)[1])for n in run(p,'remote')[1].splitlines()};record={'common':c,'representative':str(p),'remotes':remotes,'worktrees':[]};repos.append(record)
 raw=run(p,'worktree','list','--porcelain');assert raw[0]==0
 for block in raw[1].split('\n\n'):
  fields={line.split(' ',1)[0]:line.split(' ',1)[1]if' 'in line else True for line in block.splitlines()}
  path=fields.get('worktree');
  if path:record['worktrees'].append(path);worktrees[path]=fields

def inspect(item):
 path,registration=item;p=Path(path);out={'path':path,'registration':registration}
 if not p.exists():return dict(out,status='MISSING_REGISTERED_PATH')
 rc,status,error=run(p,'status','--porcelain=v1','-z','--untracked-files=normal');assert rc==0,(path,error)
 changes=[];parts=status.split('\x00')if status else[];i=0
 while i<len(parts):
  part=parts[i]
  if not part:i+=1;continue
  code=part[:2];name=part[3:];row={'status':code,'path':name}
  if'R'in code or'C'in code:i+=1;row['old_path']=parts[i]
  changes.append(row);i+=1
 out.update(status='DIRTY'if changes else'CLEAN',branch=run(p,'symbolic-ref','--short','-q','HEAD')[1]or None,head=run(p,'rev-parse','HEAD')[1],upstream=run(p,'rev-parse','--abbrev-ref','--symbolic-full-name','@{upstream}')[1]or None,last_commit=run(p,'log','-1','--format=%cs %s')[1],changes=changes);return out
with concurrent.futures.ThreadPoolExecutor(max_workers=6)as pool:rows=list(pool.map(inspect,sorted(worktrees.items())))
result={'status':'LOCAL_AUDIT_COMPLETE','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'repositories':repos,'worktrees':rows};(D/os.environ.get('AUDIT_OUTPUT','LOCAL_AUDIT.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'repositories':repos,'worktree_summary':[{'path':r['path'],'status':r['status'],'branch':r.get('branch'),'head':r.get('head'),'change_count':len(r.get('changes',[])),'samples':r.get('changes',[])[:3]}for r in rows]},ensure_ascii=False,indent=2))
