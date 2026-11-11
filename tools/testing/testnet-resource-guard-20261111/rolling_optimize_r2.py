"""One-shot sequential remaining20 BP runtime tuning, with18-other-vote guards."""
from ops_common import *
import traceback

assert not(R/'ROLL_RESUME_R2_STARTED.json').exists(),'Never replay the resume phase'
assert json.loads((R/'ROLL_FAILURE.json').read_text())['current']=='perplexityai'
assert json.loads((R/'CANARY_RESULT.json').read_text())['status']=='PASS_ONE_SAME_DATA_CANARY'
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
import ops_common as common
BANNED=sorted(x['owner']for x in table('rotcands')if x['banned']);assert len(BANNED)==15;common.BANNED=BANNED
nodes={n:x for n,x in nodes.items()if n not in BANNED}
before={n:state(x['unit'])for n,x in nodes.items()}
assert(R/'roll-events').is_dir()
progress=json.loads((R/'ROLL_PROGRESS.json').read_text())
assert progress['remaining'] and not progress['completed']
# R1 stopped after its first successful restart because native durable voting
# atomically renames safety.dat. Reconcile it read-only; do not restart it again.
name='perplexityai';node=nodes[name];prior=json.loads((R/'roll-events'/('perplexityai-started.json')).read_text())
i=rpc('get_info',port=node['rpc_port']);f=rpc('get_finalizer_info');st=state(node['unit'])
assert int(st['MainPID'])!=prior['oldPID'] and st['ActiveState']=='active' and age(i['head_block_time'])<6
assert any(v['description']==name and v['is_vote_strong'] and age(v['voted_for_block_timestamp'])<10 for v in f['last_tracked_votes'])
item={'name':name,'oldPID':prior['oldPID'],'newPID':int(st['MainPID']),'before_memory':prior['oldmemory'],'after_memory':process(st['MainPID']),'reconciled_after_normal_atomic_safety_rename':True,'not_restarted_again':True,'after_guard':guard(name),'state':st}
save('roll-events/perplexityai-result.json',item)
progress['completed'].append(item);progress['remaining'].remove(name);progress.pop('error',None);progress.update(status='RUNNING_R2_NATIVE_SAFETY_RENAME_CORRECTED',utc=now())
save('ROLL_RESUME_R2_STARTED.json',progress)
def report():save('ROLL_PROGRESS.json',progress)
try:
 while progress['remaining']:
  sched=rpc('get_producer_schedule')['active']['producers'];members=[v['producer_name']for v in sched]
  info=rpc('get_info');ts=datetime.datetime.fromisoformat(info['head_block_time']+'+00:00');epoch=datetime.datetime(2000,1,1,tzinfo=datetime.timezone.utc)
  slot=int((ts-epoch).total_seconds()*2);turn=(slot//12)%len(members)
  def until(n):
   if n not in members:return 1000
   steps=(members.index(n)-turn)%len(members)
   return(steps*12-slot%12)/2
  name=max(progress['remaining'],key=until)
  if until(name)<45:time.sleep(2);continue
  node=nodes[name];progress['current']=name;progress['next_slot_seconds']=until(name);progress['utc']=now();report()
  # No further node restart until all other expected native processes exist.
  for n,x in nodes.items():assert state(x['unit'])['MainPID']!='0',n
  g=guard(name);oldpid=int(state(node['unit'])['MainPID']);oldmem=process(oldpid)
  cfg=Path(node['config_dir'])/'config.ini'if'config_dir'in node else OLD/'nodes'/name/'config/config.ini'
  raw=cfg.read_bytes();backup=R/'runtime-backups'/name;backup.mkdir(parents=True,exist_ok=True)
  assert not(backup/'config.ini').exists();(backup/'config.ini').write_bytes(raw)
  out=[];changed=[]
  for line in raw.decode().splitlines():
   if '='in line and line.split('=',1)[0].strip()in['chain-threads','net-threads']:
    k=line.split('=',1)[0].strip();changed.append(k);line=k+' = 1'
   out.append(line)
  assert sorted(changed)==['chain-threads','net-threads']
  cfg.write_text('\n'.join(out)+'\n')
  safety=Path(node['data_dir'])/'finalizers/safety.dat'if'data_dir'in node else OLD/'nodes'/name/'data/finalizers/safety.dat'
  assert safety.is_file()and safety.stat().st_size<1024**2
  safety_inode=safety.stat().st_ino
  drop=Path('/etc/systemd/system')/(node['unit']+'.d')/'30-local-memory.conf';assert not drop.exists();drop.parent.mkdir(exist_ok=True)
  drop.write_text('[Service]\nEnvironment=MALLOC_ARENA_MAX=2\nEnvironment=MALLOC_TRIM_THRESHOLD_=131072\n')
  subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
  save('roll-events/'+name+'-started.json',{'utc':now(),'guard':g,'oldPID':oldpid,'oldmemory':oldmem,'safety_inode':safety_inode})
  started=time.monotonic();subprocess.run(['systemctl','stop',node['unit']],check=True,capture_output=True,timeout=90)
  safety_bytes=safety.read_bytes();safety_sha=hashlib.sha256(safety_bytes).hexdigest()
  (backup/'safety.after-graceful-stop.dat').write_bytes(safety_bytes)
  assert hashlib.sha256(safety.read_bytes()).hexdigest()==safety_sha
  subprocess.run(['systemctl','start',node['unit']],check=True,capture_output=True,timeout=30)
  samples=[];deadline=time.monotonic()+100
  while time.monotonic()<deadline:
   try:
    i=rpc('get_info',port=node['rpc_port']);live=rpc('get_info')
    assert i['chain_id']==CHAIN
    if age(i['head_block_time'])<6 and live['head_block_num']-i['head_block_num']<8 and i['last_irreversible_block_num']>=g['info']['last_irreversible_block_num']:
     samples.append({'utc':now(),'info':i})
     if len(samples)>=3:break
   except(OSError,TimeoutError):pass
   time.sleep(2)
  assert len(samples)>=3,'Stop rollout and reconcile this same-data node'
  s=state(node['unit']);assert int(s['MainPID'])!=oldpid and s['ActiveState']=='active'
  assert safety.is_file() and safety.stat().st_size>=len(safety_bytes)
  # Native writes/atomic renames while voting are expected; no agent reset.
  assert any(v['description']==name and v['is_vote_strong'] and age(v['voted_for_block_timestamp'])<10 for v in rpc('get_finalizer_info')['last_tracked_votes'])
  after=guard(name)
  item={'name':name,'oldPID':oldpid,'newPID':int(s['MainPID']),'before_memory':oldmem,'after_memory':process(s['MainPID']),'elapsed_seconds':time.monotonic()-started,'after_guard':after,'state':s,'sync_samples':samples,'finalizer_safety_after_clean_stop_sha256':safety_sha,'safety_not_reset_or_replaced_by_agent':True}
  save('roll-events/'+name+'-result.json',item);progress['completed'].append(item);progress['remaining'].remove(name);progress['utc']=now();progress['memory']=memory();report()
 progress.update(status='PASS_ALL21_RUNTIME_OPTIMIZED',utc=now(),final_memory=memory(),final_guard=guard())
 save('ROLL_RESULT.json',progress);report();print(json.dumps({'status':progress['status'],'completed':len(progress['completed']),'memory':memory()}),flush=True)
except BaseException:
 progress.update(status='STOPPED_NEEDS_READONLY_RECONCILIATION',utc=now(),error=traceback.format_exc());save('ROLL_FAILURE_R2.json',progress);report();raise
