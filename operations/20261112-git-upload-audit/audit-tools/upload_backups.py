"""Immutable source backups with isolated indexes; no original source, branch or index edits."""
from pathlib import Path
import json,subprocess,os,hashlib,re,configparser,io,datetime,shutil
D=Path(__file__).resolve().parent;ROOT=D.parents[1];P=json.loads((D/'PLAN.json').read_text());assert not P['secret_findings'];G={x['repository']:x for x in json.loads((D/'GITHUB_REMOTE_HEADS.json').read_text())['repositories']};E={e['path']:e for e in P['entries']}
name=subprocess.check_output(['git','-C',str(ROOT/'PureEOS-Wallet'),'config','user.name'],text=True).strip();email=subprocess.check_output(['git','-C',str(ROOT/'PureEOS-Wallet'),'config','user.email'],text=True).strip();BASE={**os.environ,'GIT_OPTIONAL_LOCKS':'0','GIT_TERMINAL_PROMPT':'0','GIT_AUTHOR_NAME':name,'GIT_AUTHOR_EMAIL':email,'GIT_COMMITTER_NAME':name,'GIT_COMMITTER_EMAIL':email}
statefile=D/'UPLOAD_PROGRESS.json';S=json.loads(statefile.read_text())if statefile.exists()else{'status':'IN_PROGRESS','snapshots':{},'markers':{},'published':[],'coverage':[]};(D/'indices').mkdir(exist_ok=True)
def save():statefile.write_text(json.dumps(S,indent=2)+'\n')
def git(p,*args,data=None,extra=None):
 q=subprocess.run(['git','-c','gc.auto=0','-c','pack.threads=2','-c','pack.windowMemory=64m','-C',str(p),*args],input=data,capture_output=True,env={**BASE,**({'GIT_ALTERNATE_OBJECT_DIRECTORIES':E[str(p)]['alternate_objects']}if str(p)in E and E[str(p)].get('alternate_objects')else{}),**(extra or{})},timeout=300)
 if q.returncode:
  (D/'LAST_GIT_ERROR.json').write_text(json.dumps({'path':str(p),'args':list(args),'returncode':q.returncode,'stdout':q.stdout.decode(errors='replace'),'stderr':q.stderr.decode(errors='replace')},indent=2));raise RuntimeError('Git command failed; see LAST_GIT_ERROR.json')
 return q.stdout
def slug(path):return re.sub('[^a-z0-9]+','-',str(Path(path).relative_to(ROOT)).lower()).strip('-')[-100:]
def url(dest):return'https://github.com/MatrixCoreX/'+dest+'.git'
def verify_original(e):
 p=Path(e['path']);assert git(p,'rev-parse','HEAD').decode().strip()==e['head'],('HEAD moved',p)
 index=Path(e['index_path']);assert(hashlib.sha256(index.read_bytes()).hexdigest()if index.exists()else None)==e['index_SHA256'],('Original index moved',p)
 for f in e['files']:
  q=p/f['path']
  if f.get('deleted'):assert not q.exists(),q
  elif'symlink'in f:assert q.is_symlink()and os.readlink(q)==f['symlink'],q
  else:assert hashlib.sha256(q.read_bytes()).hexdigest()==f['SHA256'],q

def publish(e,commit,ref,kind):
 key=url(e['destination'])+'|'+ref
 old=next((r for r in S['published']if r['key']==key),None)
 if old:assert old['commit']==commit;return
 git(e['path'],'push',url(e['destination']),commit+':refs/heads/'+ref)
 S['published'].append({'key':key,'destination':e['destination'],'ref':ref,'commit':commit,'source':e['path'],'kind':kind});save();print('UPLOADED',e['destination'],ref,commit[:12],flush=True)

def marker(e,head,label):
 key=e['common']+'|'+head
 if key not in S['markers']:
  tree=git(e['path'],'rev-parse',head+'^{tree}').decode().strip();commit=git(e['path'],'commit-tree',tree,'-p',head,data=('Archive original source head '+label+'; not a release [skip ci]\n').encode()).decode().strip();S['markers'][key]={'commit':commit,'original':head,'tree':tree};save()
 return S['markers'][key]['commit']

def snapshot(e,module_commits):
 if e['path']in S['snapshots']:return S['snapshots'][e['path']]['commit']
 verify_original(e);p=Path(e['path']);index=D/'indices'/(hashlib.sha256(e['path'].encode()).hexdigest()[:20]+'.index');extra={'GIT_INDEX_FILE':str(index)}
 if index.exists():
  failed=D/'indices-failed';failed.mkdir(exist_ok=True);copy=failed/(index.name+'.'+hashlib.sha256(index.read_bytes()).hexdigest()[:12]);
  if not copy.exists():shutil.copy2(index,copy)
 shutil.copy2(Path(e['index_path']),index)
 current_index_paths={x.decode()for x in git(p,'ls-files','-z',extra=extra).split(b'\0')if x}
 if e['files']:
  # Missing skip-worktree source is deliberately unmaterialized, not a deletion.
  flags={r[2:].decode():r[:1].decode().upper()for r in git(p,'ls-files','-v','-z',extra=extra).split(b'\0')if r}
  removed=[f['path']for f in e['files']if f.get('deleted')and f['path']in current_index_paths and flags.get(f['path'])!='S']
  if removed:git(p,'update-index','--force-remove','-z','--stdin',data=b'\0'.join(x.encode()for x in removed)+b'\0',extra=extra)
  selected=[f['path']for f in e['files']if not f.get('deleted')]
  if selected:git(p,'add','--all','--sparse','--pathspec-from-file=-','--pathspec-file-nul',data=b'\0'.join(x.encode()for x in selected)+b'\0',extra=extra)
 rewritten=[];bindings=[b for b in P['module_bindings']if b['parent']==e['path']and b['module']in module_commits]
 if bindings:
  cfg=configparser.RawConfigParser();cfg.read(p/'.gitmodules')
  for b in bindings:
   commit=module_commits[b['module']];git(p,'update-index','--add','--cacheinfo','160000,'+commit+','+b['path'],extra=extra);cfg.set(b['section'],'url',url('PureEOS'));rewritten.append({'path':b['path'],'commit':commit,'original':e['gitlinks'][b['path']],'archive_url':url('PureEOS')})
  stream=io.StringIO();cfg.write(stream);blob=git(p,'hash-object','-w','--stdin',data=stream.getvalue().encode()).decode().strip();git(p,'update-index','--add','--cacheinfo','100644,'+blob+',.gitmodules',extra=extra)
 meta={'scope':'Historical working-source backup; unqualified/not deployed. OriginalHEAD/index/files untouched.','source_path':e['path'],'original_head':e['head'],'original_branch':e['branch'],'original_index_SHA256':e['index_SHA256'],'excluded_runtime':e['excluded_runtime'],'source_files':e['files'],'submodule_backup_bindings':rewritten,'created_UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()};blob=git(p,'hash-object','-w','--stdin',data=(json.dumps(meta,indent=2)+'\n').encode()).decode().strip();git(p,'update-index','--add','--cacheinfo','100644,'+blob+',GIT_UPLOAD_BACKUP_20261112.json',extra=extra)
 tree=git(p,'write-tree',extra=extra).decode().strip();commit=git(p,'commit-tree',tree,'-p',e['head'],data=('Preserve historical working source '+slug(e['path'])+'; not a release [skip ci]\n').encode()).decode().strip();S['snapshots'][e['path']]={'commit':commit,'tree':tree,'submodules':rewritten};save();verify_original(e);return commit

# Establish actual GitHub heads, including the corrected ops publication, without fetching/building.
for dest,g in G.items():
 raw=git(g['path'],'ls-remote','--heads',url(dest)).decode();g['heads']={l.split()[1].removeprefix('refs/heads/'):l.split()[0]for l in raw.splitlines()}
module_commits={}
for e in P['entries']:
 if not e['legacy_submodule']:continue
 commit=snapshot(e,{})if e['dirty']else marker(e,e['head'],'dependency '+slug(e['path']));ref='backup/git-upload-20261112/dependencies/'+slug(e['path']);publish(e,commit,ref,'dependency-working-snapshot'if e['dirty']else'dependency-head');module_commits[e['path']]=commit
for e in P['entries']:
 if e['legacy_submodule']or not e['dirty']:continue
 commit=snapshot(e,module_commits);publish(e,commit,'backup/git-upload-20261112/working/'+slug(e['path']),'working-source-snapshot')
# Preserve distinct pre-existing staged source as well as final working-file snapshots.
S.setdefault('staged',{})
for e in P['entries']:
 if not e['dirty']:continue
 changed=git(e['path'],'diff','--cached','--name-only','-z').split(b'\0')
 if not any(changed):continue
 if e['path']not in S['staged']:
  verify_original(e);index=D/'indices'/(hashlib.sha256(e['path'].encode()).hexdigest()[:20]+'.staged.index');shutil.copy2(Path(e['index_path']),index);extra={'GIT_INDEX_FILE':str(index)}
  # These tracked source blobs are reviewed independently of their working copies.
  entries={line.split(b'\t',1)[1].decode():line.split()[1].decode()for line in git(e['path'],'ls-files','--stage',extra=extra).splitlines()if b'\t'in line}
  for raw in changed:
   if not raw:continue
   n=raw.decode()
   if n not in entries:continue
   data=git(e['path'],'cat-file','blob',entries[n]).decode(errors='replace')
   patterns=[r'(?<![A-Za-z0-9])(?:PVT_[KR]1_[1-9A-HJ-NP-Za-km-z]{45,60}|5[HJK][1-9A-HJ-NP-Za-km-z]{49})(?![A-Za-z0-9])',r'ghp_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16}',r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----']
   for pattern in patterns:
    hits=[v for v in re.findall(pattern,data)if v!='5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3'];assert not hits,('Sensitive staged source requires redaction',e['path'],n)
  tree=git(e['path'],'write-tree',extra=extra).decode().strip();commit=git(e['path'],'commit-tree',tree,'-p',e['head'],data=('Preserve original staged source '+slug(e['path'])+'; not a release [skip ci]\n').encode()).decode().strip();S['staged'][e['path']]={'commit':commit,'tree':tree};save()
 publish(e,S['staged'][e['path']]['commit'],'backup/git-upload-20261112/staged/'+slug(e['path']),'original-staged-source')
# Also preserve committed local heads not already reachable on actual GitHub or new backup refs.
bycommon={}
for e in P['entries']:bycommon.setdefault(e['common'],[]).append(e)
for common,entries in bycommon.items():
 e=entries[0];heads={h['commit']:h['branch']for x in entries for h in x['local_heads']};remote=list(G[e['destination']]['heads'].values())+[r['commit']for r in S['published']if r['destination']==e['destination']];remote=list(dict.fromkeys(remote));check=git(e['path'],'cat-file','--batch-check=%(objectname) %(objecttype)',data=('\n'.join(remote)+'\n').encode()).decode();present=[l.split()[0]for l in check.splitlines()if l.endswith(' commit')];reachable=set(git(e['path'],'rev-list','--stdin',data=('\n'.join(present)+'\n').encode()).decode().splitlines())if present else set()
 for head,branch in heads.items():
  if head in reachable:S['coverage'].append({'common':common,'head':head,'branch':branch,'status':'REACHABLE_ON_GITHUB_OR_NEW_BACKUP'});continue
  commit=marker(e,head,branch);ref='backup/git-upload-20261112/heads/'+slug(e['path'])+'-'+re.sub('[^a-z0-9]+','-',branch.lower()).strip('-')[-60:]+'-'+head[:10];publish(e,commit,ref,'unpublished-source-head');reachable.update(git(e['path'],'rev-list',commit).decode().splitlines());S['coverage'].append({'common':common,'head':head,'branch':branch,'status':'UPLOADED_AS_ARCHIVE','archive_ref':ref})
 save()
# Verify every new remote ref independently and recheck original workspaces/indexes.
remote_maps={}
for dest in {r['destination']for r in S['published']}:
 e=next(x for x in P['entries']if x['destination']==dest);raw=git(e['path'],'ls-remote','--heads',url(dest)).decode();remote_maps[dest]={l.split()[1].removeprefix('refs/heads/'):l.split()[0]for l in raw.splitlines()}
for r in S['published']:assert remote_maps[r['destination']].get(r['ref'])==r['commit'],r['ref'];r['remote_ref_verified']=True
for e in P['entries']:verify_original(e)
S.update(status='PASS_ALL_SOURCE_BACKUPS_PUBLISHED',original_HEAD_indexes_reviewed_files_unchanged=True,verified_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat());save();(D/'BACKUP_RESULT.json').write_text(json.dumps(S,indent=2)+'\n');print('ALL_BACKUP_REFS_VERIFIED',len(S['published']),flush=True)
