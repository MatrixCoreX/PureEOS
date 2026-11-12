from pathlib import Path
import json,subprocess,os,hashlib,shutil,re
D=Path(__file__).resolve().parent;R=D.parents[1];p=D/'PLAN.json';P=json.loads(p.read_text());rows=json.loads((D/'ADDITIONAL_REPOSITORIES.json').read_text());findings=[]
if not(D/'PLAN_R1.json').exists():shutil.copy2(p,D/'PLAN_R1.json')
if not(D/'BACKUP_RESULT_R1.json').exists():shutil.copy2(D/'BACKUP_RESULT.json',D/'BACKUP_RESULT_R1.json')
def git(path,*a):
 q=subprocess.run(['git','-C',str(path),*a],capture_output=True,env={**os.environ,'GIT_OPTIONAL_LOCKS':'0'},timeout=90);assert q.returncode==0,q.stderr.decode();return q.stdout
for row in rows[-2:]:
 path=row['path'];w=Path(path)
 if any(e['path']==path for e in P['entries']):continue
 raw=git(w,'status','--porcelain=v1','-z').decode();names=set();parts=raw.split('\0');i=0;excluded=[]
 while i<len(parts):
  v=parts[i];i+=1
  if not v:continue
  if v[:2]=='??':continue
  names.add(v[3:])
  if'R'in v[:2]or'C'in v[:2]:names.add(parts[i]);i+=1
 for n in git(w,'ls-files','--others','--exclude-standard','-z').decode().split('\0'):
  if not n:continue
  if '__pycache__'in Path(n).parts or n.endswith('.pyc'):excluded.append(n)
  else:names.add(n)
 files=[]
 for n in sorted(names):
  f=w/n
  if f.exists():
   b=f.read_bytes();files.append({'path':n,'SHA256':hashlib.sha256(b).hexdigest(),'bytes':len(b)});t=b.decode(errors='replace')
   for pat in [r'(?<![A-Za-z0-9])(?:PVT_[KR]1_[1-9A-HJ-NP-Za-km-z]{45,60}|5[HJK][1-9A-HJ-NP-Za-km-z]{49})(?![A-Za-z0-9])',r'ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}',r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----']:
    for h in re.findall(pat,t):
     if h!='5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3':findings.append({'path':path,'file':n,'SHA256':hashlib.sha256(h.encode()).hexdigest()})
  else:files.append({'path':n,'deleted':True})
 idx=Path(git(w,'rev-parse','--git-path','index').decode().strip());idx=idx if idx.is_absolute()else w/idx
 common=Path(git(w,'rev-parse','--git-common-dir').decode().strip());common=common if common.is_absolute()else w/common
 heads=[dict(zip(['branch','commit'],l.split('\t')))for l in git(w,'for-each-ref','--format=%(refname:short)%09%(objectname)','refs/heads').decode().splitlines()]
 e={'path':path,'head':row['head']['stdout'],'branch':row['branch']['stdout'],'destination':'PureHistory'if'incomplete-history'in path else'PureEOS','common':str(common.resolve()),'local_heads':heads,'dirty':bool(raw),'files':files,'excluded_runtime':excluded,'gitlinks':{},'index_path':str(idx.resolve()),'index_SHA256':hashlib.sha256(idx.read_bytes()).hexdigest(),'legacy_submodule':False}
 if'incomplete-history'in path:e['alternate_objects']=str(R/'PureHistory/.git/objects');e['note']='Incomplete historical bundle uses existing complete local PureHistory objects, without editing its alternates/config/source/index.'
 P['entries'].append(e)
assert not findings,findings
P['supplement']='Two additional historical checkout repositories; three clean CDT dependencies independently match official GitHub main heads. Root .git is an invalid empty marker, not a repository.'
p.write_text(json.dumps(P,indent=2)+'\n');print('PLAN',len(P['entries']),'entries',sum(e['dirty']for e in P['entries']),'dirty')
u=D/'upload_backups.py';s=u.read_text();s=s.replace("env={**BASE,**(extra or{})}","env={**BASE,**({'GIT_ALTERNATE_OBJECT_DIRECTORIES':E[str(p)]['alternate_objects']}if str(p)in E and E[str(p)].get('alternate_objects')else{}),**(extra or{})}");u.write_text(s)
