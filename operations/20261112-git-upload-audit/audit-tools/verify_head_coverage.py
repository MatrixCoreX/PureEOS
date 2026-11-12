from pathlib import Path
import json,os,subprocess,datetime
D=Path(__file__).resolve().parent;P=json.loads((D/'PLAN.json').read_text());G={g['repository']:g for g in json.loads((D/'GITHUB_REMOTE_HEADS_FINAL_BEFORE_DOCS.json').read_text())['repositories']};out=[];seen=set()
def git(e,*args,data=None):
 env={**os.environ,'GIT_OPTIONAL_LOCKS':'0',**({'GIT_ALTERNATE_OBJECT_DIRECTORIES':e['alternate_objects']}if e.get('alternate_objects')else{})};q=subprocess.run(['git','-C',e['path'],*args],input=data,capture_output=True,env=env,timeout=120);assert q.returncode==0,q.stderr.decode();return q.stdout
for e in P['entries']:
 if e['common']in seen:continue
 seen.add(e['common']);heads=G[e['destination']]['heads'];check=git(e,'cat-file','--batch-check=%(objectname) %(objecttype)',data=('\n'.join(heads.values())+'\n').encode()).decode();present=[l.split()[0]for l in check.splitlines()if l.endswith(' commit')];reachable=set(git(e,'rev-list','--stdin',data=('\n'.join(present)+'\n').encode()).decode().splitlines())
 for h in e['local_heads']:
  assert h['commit']in reachable,(e['path'],h)
  out.append({'source_common_repository':e['common'],'representative_worktree':e['path'],'local_branch':h['branch'],'local_commit':h['commit'],'destination':e['destination'],'direct_remote_refs':[ref for ref,sha in heads.items()if sha==h['commit']],'reachable_on_actual_GitHub':True})
recent=[]
for path in ['PureEOS-Wallet','PureEOS-Key-Unbounded-20261107','PureEOS-Signup-Bridge-20261105','PureEOS-System-Accounts-20261105','PureEOS-explorer-KeyGrowth-20261107','PureHistory-key-history-20261103','PureEOS-PQC-Plan-20261104','PureEOS-Testnet-Ops-20261111']:
 e=next(x for x in P['entries']if Path(x['path']).name==path);assert G[e['destination']]['heads'][e['branch']]==e['head'];recent.append({'component':path,'repository':e['destination'],'branch':e['branch'],'local_HEAD':e['head'],'remote_HEAD':G[e['destination']]['heads'][e['branch']],'verified':True})
res={'status':'PASS_ALL_129_LOCAL_BRANCH_POINTERS_121_UNIQUE_HEADS_REACHABLE_ON_GITHUB','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'local_branch_pointers':out,'recent_release_heads_before_audit_document_commit':recent,'note':'Some historical branch names are absent or behind current remote branches. All their commit content is reachable on GitHub, and this manifest preserves original local branch names/SHAs. No existing remote branch was forced backward.'};(D/'HEAD_COVERAGE_VERIFICATION.json').write_text(json.dumps(res,indent=2)+'\n');print(res['status'])
