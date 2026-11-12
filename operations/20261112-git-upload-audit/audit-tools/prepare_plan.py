from pathlib import Path
import json,subprocess,os,hashlib,configparser,re,datetime
D=Path(__file__).resolve().parent;R=D.parents[1];A=json.loads((D/'LOCAL_AUDIT.json').read_text());G={x['repository']:x for x in json.loads((D/'GITHUB_REMOTE_HEADS.json').read_text())['repositories']};env={**os.environ,'GIT_OPTIONAL_LOCKS':'0','GIT_TERMINAL_PROMPT':'0'}
def git(p,*args,data=None):
 q=subprocess.run(['git','-C',str(p),*args],input=data,capture_output=True,env=env,timeout=90);assert q.returncode==0,q.stderr.decode()[-1000:];return q.stdout
modules={};roots=[]
for w in A['worktrees']:
 p=Path(w['path']);f=p/'.gitmodules'
 if not f.exists():continue
 cfg=configparser.RawConfigParser();cfg.read(f)
 for section in cfg.sections():
  url=cfg.get(section,'url',fallback='');name=cfg.get(section,'path',fallback='');m=p/name
  if ('github.com/PureEOSIO/'not in url and not any(c['path']==name for c in w.get('changes',[]))) or not(m/'.git').exists():continue
  if str(m)not in modules:
   raw=git(m,'status','--porcelain=v1','-z').decode();parts=raw.split('\0');changes=[];i=0
   while i<len(parts):
    x=parts[i]
    if x:
     row={'status':x[:2],'path':x[3:]};changes.append(row)
     if'R'in x[:2]or'C'in x[:2]:i+=1;row['old_path']=parts[i]
    i+=1
   modules[str(m)]={'path':str(m),'branch':git(m,'symbolic-ref','--short','-q','HEAD').decode().strip()if subprocess.run(['git','-C',str(m),'symbolic-ref','-q','HEAD'],capture_output=True).returncode==0 else None,'head':git(m,'rev-parse','HEAD').decode().strip(),'changes':changes,'status':'DIRTY'if changes else'CLEAN','legacy_submodule':True,'original_url':url}
  roots.append({'parent':str(p),'path':name,'section':section,'module':str(m)})
work=A['worktrees']+list(modules.values());plans=[];findings=[]
known_default='5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3'
patterns=[('private_key',re.compile(r'(?<![A-Za-z0-9])(?:PVT_[KR]1_[1-9A-HJ-NP-Za-km-z]{45,60}|5[HJK][1-9A-HJ-NP-Za-km-z]{49})(?![A-Za-z0-9])')),('api_token',re.compile(r'(?:ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}|xox[baprs]-[A-Za-z0-9-]{20,})')),('PEM_private',re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'))]
def destination(p):
 for r in A['repositories']:
  if str(p)in r['worktrees']:
   for url in r['remotes'].values():
    if'github.com/MatrixCoreX/'in url:return url.split('MatrixCoreX/',1)[1].removesuffix('.git')
 return'PureEOS'
def inspect_file(p,name):
 f=p/name
 if not f.is_file()or f.is_symlink():return
 b=f.read_bytes()
 if len(b)>10*1024**2:findings.append({'repo':str(p),'path':name,'kind':'large_changed_file','bytes':len(b)});return
 try:t=b.decode()
 except UnicodeError:return
 for kind,pattern in patterns:
  for hit in pattern.finditer(t):
   value=hit.group();known=value==known_default
   findings.append({'repo':str(p),'path':name,'line':t.count('\n',0,hit.start())+1,'kind':kind,'known_public_development_fixture':known,'value_SHA256':hashlib.sha256(value.encode()).hexdigest()})
for w in work:
 p=Path(w['path'])
 if not p.exists():continue
 dest='PureEOS'if w.get('legacy_submodule')else destination(p)
 if dest not in G:continue
 rc=git(p,'rev-parse','--git-common-dir').decode().strip();common=str((p/rc).resolve());heads=[]
 raw=git(p,'for-each-ref','--format=%(refname:short)%09%(objectname)','refs/heads').decode()
 for line in raw.splitlines():name,oid=line.split('\t');heads.append({'branch':name,'commit':oid})
 if not any(h['commit']==w['head']for h in heads):heads.append({'branch':'DETACHED','commit':w['head']})
 allowed=set();excluded=[];original_gitlinks={}
 for line in git(p,'ls-files','--stage').decode().splitlines():
  if line.startswith('160000 '):left,name=line.split('\t',1);original_gitlinks[name]=left.split()[1]
 for c in w.get('changes',[]):
  if c['status']=='??':continue
  for name in[c['path']]+([c['old_path']]if'old_path'in c else[]):
   if name not in original_gitlinks:allowed.add(name)
 untracked=[x.decode()for x in git(p,'ls-files','--others','--exclude-standard','-z').split(b'\0')if x]
 for name in untracked:
  parts=Path(name).parts;low=name.lower()
  if any(x.lower()in['config-dir','data-dir','node_modules','.cache','private','signing']for x in parts)or Path(name).suffix.lower()in['.ini','.wallet','.key','.p12','.jks','.apk','.wasm','.o','.a']or Path(name).name.startswith('PRIVATE_'):
   excluded.append(name);continue
  allowed.add(name)
 files=[]
 for name in sorted(allowed):
  f=p/name
  if f.is_file():
   inspect_file(p,name);files.append({'path':name,'SHA256':hashlib.sha256(f.read_bytes()).hexdigest(),'bytes':f.stat().st_size})
  elif f.is_symlink():files.append({'path':name,'symlink':os.readlink(f)})
  else:files.append({'path':name,'deleted':True})
 index=Path(git(p,'rev-parse','--git-path','index').decode().strip());index=index if index.is_absolute()else p/index
 plans.append({'path':str(p),'head':w['head'],'branch':w.get('branch'),'destination':dest,'common':common,'local_heads':heads,'dirty':w['status']=='DIRTY','files':files,'excluded_runtime':excluded,'gitlinks':original_gitlinks,'index_path':str(index.resolve()),'index_SHA256':hashlib.sha256(index.read_bytes()).hexdigest()if index.exists()else None,'legacy_submodule':w.get('legacy_submodule',False)})
result={'status':'PREPARED_NOT_YET_PUBLISHED','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'entries':plans,'module_bindings':roots,'secret_findings':findings,'scope':'Use temporaryindexes/immutablearchivecommits, originalHEAD/index/source untouched; onlyMatrixCoreXdestinations. Runtimeconfiguration/keys/packages excluded.'};(D/'PLAN.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'entries':len(plans),'dirty':sum(x['dirty']for x in plans),'legacy_modules':len(modules),'findings':findings,'excluded':[{'repo':x['path'],'paths':x['excluded_runtime']}for x in plans if x['excluded_runtime']]},indent=2))
