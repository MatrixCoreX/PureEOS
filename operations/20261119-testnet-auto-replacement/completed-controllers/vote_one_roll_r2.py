from pathlib import Path
import sys,os,json,hashlib,time,fcntl,traceback
G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');sys.path.insert(0,str(G));import stability_guard as g
os.umask(0o077);assert not(R/'VOTE_ONE_ROLL_R2_STARTED.json').exists();assert (R/'VOTE_ONE_ROLL_FAILURE.json').exists();canary=json.loads((R/'VOTE_ONE_CANARY_RESULT.json').read_text());assert canary['status']=='PASS_ONE_VOTE_THREAD_CANARY_60SEC';nodes=g.load('GUARD_NODE_MAP.json');rows=g.rows();targets=sorted(n for n in nodes if not rows[n]['banned']);assert len(targets)==23
progress=json.loads((R/'VOTE_ONE_ROLL_PROGRESS.json').read_text());assert len(progress['completed'])==5 and len(set(progress['completed']))==5;assert all(g.load('GUARD_NODE_MAP.json')[n]['config_sha256']==g.sha(Path(g.load('GUARD_NODE_MAP.json')[n]['config_dir'])/'config.ini')for n in progress['completed']);progress.update(status='ROLLING_R2',utc=g.now());gR=g.R
# Evidence under the new case; guardian state and native-operation lock remain shared.
def save(n,d):
 p=R/n;t=p.with_suffix('.tmp');t.write_text(json.dumps(d,indent=2)+'\n');os.replace(t,p)
save('VOTE_ONE_ROLL_R2_STARTED.json',progress);deadline=time.monotonic()+1800
try:
 while len(progress['completed'])<len(targets):
  assert time.monotonic()<deadline,'Bounded roll deadline; leave remaining nodes running'
  lock=(G/'native-operation.lock').open('a')
  try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
  except BlockingIOError:lock.close();time.sleep(3);continue
  try:
   e=g.load('GUARD_INFLIGHT.json');assert e['status']in ['PASS_ONE_GUARDED_RECYCLE','PASS_RECONCILED_SAME_OPERATION','ABORTED_BEFORE_NATIVE_STOP_RETRY_AFTER_COOLDOWN'],'Unfinished operation requires reconciliation'
   nodes=g.load('GUARD_NODE_MAP.json')
   try:s,i,pids,ro,m=g.status_context(nodes)
   except Exception:
    save('VOTE_ONE_ROLL_WAIT.json',{'utc':g.now(),'status':'WAIT_READ_RETRY_BEFORE_ANY_NEW_CHANGE','error':traceback.format_exc()});continue
   conditions={'head_fresh':s['head_age']<5,'LIB_fresh':s['lib_age']<20,'native_policy':s['policy_count']==21 and s['threshold']==15,'recent_quorum':len(s['recent'])>=19,'all_running':s['all_running'],'RAM_headroom':s['mem_available']>512*1024**2,'swap_headroom':s['swap_free']>8*1024**3}
   if not all(conditions.values()):
    save('VOTE_ONE_ROLL_WAIT.json',{'utc':g.now(),'status':'WAIT_QUALIFICATION_BEFORE_ANY_NEW_CHANGE','conditions':conditions,'recent_count':len(s['recent']),'head_age':s['head_age'],'LIB_age':s['lib_age']});continue
   pending=[n for n in targets if n not in progress['completed']];assert not any(ro[n]['banned']for n in pending),'New ban during rolling: retire and reconcile, do not restart'
   if len(pending)>1:pending=[n for n in pending if n!='gptgenesisbp']
   candidates=[n for n in pending if s['nodes'][n]['safety_exists']and s['nodes'][n]['distance']>=(110 if ro[n]['zero_windows']==2 else 100)and len(set(s['recent'])-{n})>=19]
   if not candidates:continue
   name=max(candidates,key=lambda n:(s['nodes'][n]['standby'],s['nodes'][n]['distance']));v=nodes[name];cfg=Path(v['config_dir'])/'config.ini';raw=cfg.read_bytes();old=raw.decode();lines=old.splitlines();hits=[l for l in lines if '='in l and l.split('=',1)[0].strip()=='vote-threads'];assert len(hits)==1 and hits[0].split('=',1)[1].strip()=='2';new=old.replace(hits[0],hits[0].split('=')[0]+'= 1')
   if name=='gptgenesisbp':
    # Local RPC serialization budget only; no signature/consensus checks change.
    kept=[l for l in new.splitlines()if not('='in l and l.split('=',1)[0].strip()in ['abi-serializer-max-time-ms','http-max-response-time-ms'])];new='\n'.join(kept)+'\nabi-serializer-max-time-ms = 100\nhttp-max-response-time-ms = 500\n'
   private=R/'private/config-backups';private.mkdir(mode=0o700,parents=True,exist_ok=True);backup=private/(name+'.before-vote-one.ini');assert not backup.exists();backup.write_bytes(raw)
   record={'target':name,'utc':g.now(),'oldPID':pids[name],'old_config_SHA256':g.sha(cfg),'new_config_SHA256':hashlib.sha256(new.encode()).hexdigest(),'next_duty':s['nodes'][name]['distance'],'only_vote_thread_and_founder_local_RPC_budgets':True};save('VOTE_ONE_ROLL_CURRENT.json',record);stat=cfg.stat();tmp=cfg.with_name('config.cpu-one.tmp');assert not tmp.exists();tmp.write_text(new);os.chown(tmp,stat.st_uid,stat.st_gid);tmp.chmod(stat.st_mode&0o777);os.replace(tmp,cfg);v['config_sha256']=g.sha(cfg);g.save('GUARD_NODE_MAP.json',nodes)
   s,i,pids,ro,m=g.status_context(nodes);assert s['nodes'][name]['distance']>=(105 if ro[name]['zero_windows']==2 else 95)and len(set(s['recent'])-{name})>=19
   result=g.perform(name,nodes,(s,i,pids,ro,m));record['operation']=result;assert result['status']=='PASS_ONE_GUARDED_RECYCLE',result
   progress['completed'].append(name);progress['operations'].append(record);progress['utc']=g.now();save('VOTE_ONE_ROLL_PROGRESS.json',progress)
  finally:fcntl.flock(lock,fcntl.LOCK_UN);lock.close();time.sleep(2)
 progress.update(status='PASS_ALL23_ONE_VOTE_THREAD_SAME_DATA',utc=g.now());save('VOTE_ONE_ROLL_RESULT.json',progress)
except BaseException:
 progress.update(status='HOLD_RECONCILE_CURRENT_NODE_NO_REPLAY',utc=g.now(),error=traceback.format_exc());save('VOTE_ONE_ROLL_R2_FAILURE.json',progress);raise
