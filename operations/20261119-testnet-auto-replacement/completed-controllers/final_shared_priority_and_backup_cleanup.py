from pathlib import Path
import sys,os,json,hashlib,time,fcntl,subprocess
R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');sys.path.insert(0,str(G));import stability_guard as g
assert not(R/'FINAL_SHARED_PRIORITY_STARTED.json').exists();audit=json.loads((R/'FINAL_AUDIT_RESULT.json').read_text());assert audit['status'].startswith('PASS_');lock=(G/'native-operation.lock').open('a')
for _ in range(30):
 try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
 except BlockingIOError:time.sleep(1)
else:raise RuntimeError('Existing native operation active')
nodes=g.load('GUARD_NODE_MAP.json');s,i,pids,rows,mem=g.status_context(nodes);assert s['all_running']and s['head_age']<5 and s['lib_age']<20 and len(s['recent'])>=19;v=nodes['gptgenesisbp'];assert not rows['gptgenesisbp']['banned'];u=v['unit'];before=dict(x.split('=',1)for x in subprocess.check_output(['systemctl','show',u,'-p','CPUWeight,IOWeight,CPUQuotaPerSecUSec'],text=True).splitlines()if'='in x);assert before['CPUQuotaPerSecUSec']=='infinity'
record={'utc':g.now(),'target':u,'old_properties':before,'purpose':'Protect shared writer/relay/genesis source with soft scheduling priority; no restart or hard CPU cap','before_PIDs':pids};(R/'FINAL_SHARED_PRIORITY_STARTED.json').write_text(json.dumps(record,indent=2)+'\n');drop=Path('/etc/systemd/system')/(u+'.d')/'99-shared-source-priority-20261119.conf';assert not drop.exists();drop.write_text('[Service]\nCPUWeight=1000\nIOWeight=1000\n');subprocess.run(['systemctl','set-property','--runtime',u,'CPUWeight=1000','IOWeight=1000'],capture_output=True,check=True,timeout=15);subprocess.run(['systemctl','daemon-reload'],capture_output=True,check=True,timeout=15)
# Remove only this task's own full-config rollback copies after qualification.
# Original live keys/configs/safety and immutable event proofs remain untouched.
private=R/'private';private.chmod(0o700);backups=private/'config-backups';targets=json.loads((R/'VOTE_ONE_ROLL_RESULT.json').read_text())['targets'];plan=[];assert {p.name for p in backups.iterdir()}=={n+'.before-vote-one.ini'for n in targets}
for n in targets:
 p=backups/(n+'.before-vote-one.ini');assert p.is_file()and not p.is_symlink();raw=p.read_bytes();old=raw.decode();hits=[l for l in old.splitlines()if'='in l and l.split('=',1)[0].strip()=='vote-threads'];assert len(hits)==1 and hits[0].split('=',1)[1].strip()=='2';wanted=old.replace(hits[0],hits[0].split('=')[0]+'= 1')
 if n=='gptgenesisbp':wanted='\n'.join(l for l in wanted.splitlines()if not('='in l and l.split('=',1)[0].strip()in ['abi-serializer-max-time-ms','http-max-response-time-ms']))+'\nabi-serializer-max-time-ms = 100\nhttp-max-response-time-ms = 500\n'
 assert (Path(nodes[n]['config_dir'])/'config.ini').read_text()==wanted,n
 plan.append({'name':n,'copy_SHA256':hashlib.sha256(raw).hexdigest(),'bytes':len(raw),'live_config_SHA256':nodes[n]['config_sha256']})
(R/'OWN_CONFIG_BACKUP_DELETE_PLAN.json').write_text(json.dumps({'scope':'Only this task23 qualified temporary full-config copies; no live or independent keys/safety deletion','files':plan},indent=2)+'\n')
for x in plan:(backups/(x['name']+'.before-vote-one.ini')).unlink()
backups.rmdir();(R/'OWN_CONFIG_BACKUP_DELETE_RESULT.json').write_text(json.dumps({'status':'PASS23_OWN_TEMPORARY_CONFIG_COPIES_DELETED','utc':g.now(),'files':plan,'live_config_and_independent_keys_safety_retained':True},indent=2)+'\n');after={n:g.state(x['unit'])['MainPID']for n,x in nodes.items()};assert after==pids;record['after_properties']=dict(x.split('=',1)for x in subprocess.check_output(['systemctl','show',u,'-p','CPUWeight,IOWeight,CPUQuotaPerSecUSec'],text=True).splitlines()if'='in x);assert record['after_properties']=={'CPUWeight':'1000','CPUQuotaPerSecUSec':'infinity','IOWeight':'1000'};fcntl.flock(lock,fcntl.LOCK_UN)
subprocess.run(['systemctl','start','pureeos-auto-replacement-20261119.service'],capture_output=True,check=True,timeout=120)
samples=[]
for k in range(12):
 s,i,livepids,ro,m=g.status_context(nodes);samples.append({'utc':g.now(),'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'head_age':s['head_age'],'LIB_age':s['lib_age'],'recent':len(s['recent'])});assert s['head_age']<6 and s['lib_age']<20 and len(s['recent'])>=19 and s['all_running'];assert livepids==pids
 if k<11:time.sleep(5)
record.update(status='PASS_SHARED_PRIORITY_CONFIG_CLEANUP_AND60SEC_NO_RESTART',utc=g.now(),samples=samples,automatic_replacement_latest=json.loads((R/'AUTO_REPLACEMENT_LATEST.json').read_text()),automatic_job_count=len(list((R/'jobs').glob('*/STATE.json'))),source_SHA256={n:g.sha(R/n)for n in json.loads((R/'AUTO_SOURCE_CURRENT_SHA256.json').read_text())},guardian_SHA256={n:g.sha(G/n)for n in ['stability_guard.py','stability_policy.py','retire_banned_runtime.py','log_cleanup.py']},founder_zero_windows=ro['gptgenesisbp']['zero_windows'],founder_banned=bool(ro['gptgenesisbp']['banned']));(R/'FINAL_SHARED_PRIORITY_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
