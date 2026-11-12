from pathlib import Path
import json,subprocess,concurrent.futures,os,datetime
D=Path(__file__).resolve().parent;A=json.loads((D/'LOCAL_AUDIT.json').read_text());env={**os.environ,'GIT_TERMINAL_PROMPT':'0'}
selected={}
for r in A['repositories']:
 for n,url in r['remotes'].items():
  if 'github.com'in url and 'MatrixCoreX/'in url:
   key=url.split('MatrixCoreX/',1)[1].removesuffix('.git');selected.setdefault(key,{'repository':key,'path':r['representative'],'remote':n,'URL':url})
def check(r):
 p=subprocess.run(['git','-C',r['path'],'ls-remote','--heads',r['remote']],capture_output=True,text=True,timeout=90,env=env);out={**r,'returncode':p.returncode}
 if p.returncode:out['error']=p.stderr[-1000:];return out
 out['heads']={line.split()[1].removeprefix('refs/heads/'):line.split()[0]for line in p.stdout.splitlines()};return out
with concurrent.futures.ThreadPoolExecutor(max_workers=5)as pool:rows=list(pool.map(check,selected.values()))
(D/os.environ.get('AUDIT_REMOTE_OUTPUT','GITHUB_REMOTE_HEADS.json')).write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'repositories':rows},indent=2)+'\n')
for r in rows:print(r['repository'],r['returncode'],len(r.get('heads',{})),r.get('error',''))
for w in A['worktrees']:
 if w['status']=='CLEAN'and w.get('branch'):
  names=[r for r in rows if w['path']in next((a['worktrees']for a in A['repositories']if a['representative']==r['path']),[])]
  if not names:continue
  r=names[0];remote=r.get('heads',{}).get(w['branch']);status='EXACT_GITHUB_MATCH'if remote==w['head']else'MISSING_BRANCH'if not remote else'DIFFERENT_HEAD'
  print(Path(w['path']).name,status,w['branch'],w['head'][:12],(remote or'')[:12])
