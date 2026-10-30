from pathlib import Path
import os,sys,json,hashlib,time,subprocess,shutil,fcntl,traceback,concurrent.futures
C=Path('/opt/aic-bp-discovery-30nodes-20261030');K=Path('/opt/aic-bp-standby-seed-failover-20261030')
sys.path.insert(0,str(C/'input'));import functional_30_common as m
f=m.f;E=K/'live-qualification';E.mkdir(exist_ok=True)
def report(status,**kw):f.write(E/'STATUS.json',dict(status=status,UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),baseline_promoted=False,pressure_load=False,**kw))
def persist():f.write(C/'LIVE_TOPOLOGY.json',m.topology)
def stop(n):
 return m.operate(n,"import subprocess,json\nr=subprocess.run(['systemctl','stop',"+repr(n['unit'])+"],capture_output=True,text=True);assert r.returncode==0,r.stderr;print(json.dumps(dict(stopped=True)))")
def wait_caught(n,others,timeout=90):
 deadline=time.monotonic()+timeout;last=None
 while True:
  try:
   last=f.rpc(n,'get_info');ref=f.rpc(others[0],'get_info')
   if last['chain_id']==m.topology['chain_id'] and ref['head_block_num']-last['head_block_num']<=4 and last['last_irreversible_block_num']>=ref['last_irreversible_block_num']-4:return last
  except BaseException as e:last=str(e)
  assert time.monotonic()<deadline,(n['account'],'restart did not catch up',last)
  time.sleep(.5)
try:
 limit=time.monotonic()+900;gate=K/'component-gates/REPORT.json'
 while not gate.exists():
  report('waiting-for-component-and-sanitizer-gates')
  assert time.monotonic()<limit,'Component gates did not finish; live chain untouched'
  time.sleep(3)
 gates=json.loads(gate.read_text());assert gates['status']=='BP-gossip-component-reproducers-and-regressions-passed'
 binary=K/'build/nodeos';sha=hashlib.sha256(binary.read_bytes()).hexdigest()
 build=json.loads((K/'BUILD_REPORT.json').read_text());assert sha=='6e9c668586dfae11f82a461028a734704e853b32f7acc1232559ffbf4de68cf7'
 f.write(E/'INPUTS.json',dict(components=gates,build=build,installed_binary_sha256=sha))
 lock=open('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock','a')
 fcntl.flock(lock,fcntl.LOCK_EX)
 m.topology=json.loads((C/'LIVE_TOPOLOGY.json').read_text());nodes=m.topology['nodes'];f.N=nodes;f.T=m.topology
 f.write(E/'BEFORE_INFOS.json',f.infos())
 schedule=f.rpc(nodes[0],'get_producer_schedule')['active'];active={p['producer_name']for p in schedule['producers']};assert len(active)==21
 backups=[]
 for machine in m.topology['machines']:
  root=C if machine['primary']else m.P
  if machine['primary']:
   target=root/'runtime/nodeos';back=root/'runtime/nodeos.pre-standby-fix'
   if not back.exists():shutil.copy2(target,back)
   tmp=target.with_suffix('.standby-fix-new');shutil.copy2(binary,tmp);tmp.chmod(0o755);tmp.replace(target)
  else:
   target=root/'runtime/nodeos';tmp=target.with_suffix('.standby-fix-new')
   r=subprocess.run(['scp',*m.opts,str(binary),'root@'+machine['ip']+':'+str(tmp)],capture_output=True,text=True,timeout=90);assert r.returncode==0,r.stderr[-1000:]
   m.operate(machine,"from pathlib import Path\nimport hashlib,shutil,json\np=Path("+repr(str(target))+");n=Path("+repr(str(tmp))+");b=p.with_name('nodeos.pre-standby-fix');assert hashlib.sha256(n.read_bytes()).hexdigest()=="+repr(sha)+"\nif not b.exists():shutil.copy2(p,b)\nn.chmod(0o755);n.replace(p);print(json.dumps(dict(installed=True)))")
  backups.append(dict(host=machine['ip'],runtime=str(target),sha256=sha))
 f.write(E/'RUNTIME_INSTALLATION.json',backups)
 order=sorted(nodes,key=lambda n:(n['index']<3,n['account']in active,n['index']))
 changes=[]
 for n in order:
  before=f.rpc(n,'get_info');old_unit=n['unit'];running=False
  report('rolling-upgrade-with-existing-data-and-safety',account=n['account'],completed=len(changes),total=30)
  try:
   stop(n)
   started=m.start_node(n,initial=False,suffix='standbyfix')
   running=True;persist()
   after=wait_caught(n,[x for x in nodes if x!=n])
  finally:
   if not running:
    started=m.start_node(n,initial=False,suffix='standbyfix-recover');persist()
  changes.append(dict(account=n['account'],old_unit=old_unit,new_unit=n['unit'],before=before,after=after,snapshot_reused=False))
  f.write(E/'ROLLING_UPGRADE.json',changes)
 m.topology.update(source_commit=gates['source_commit'],binary_production_source_commit='ce13ad23934dda44d418d8c6dd9882ba73496c97',nodeos_sha256=sha)
 persist();f.T=m.topology
 f.write(E/'AFTER_UPGRADE_INFOS.json',f.infos())
 cid='M30-08';D=f.begin(cid,'R04');stopped=[];failure=None;f.N=nodes
 report('retesting-both-seeds-down-with-all-live-nodes-on-new-binary')
 try:
  active={p['producer_name']for p in f.rpc(nodes[0],'get_producer_schedule')['active']['producers']}
  victims=nodes[:3];remaining=nodes[3:];assert len(active)==21 and len(active-{n['account']for n in victims})>=15
  for n in victims:stop(n);stopped.append(n)
  f.N=remaining
  account=f.F['business_accounts'][0];dest=f.F['business_accounts'][1]
  acts=[f.action('eosio.token','transfer',account,dict(from_=account))] if False else [f.action('eosio.token','transfer',account,{'from':account,'to':dest,'quantity':'0.0001 SYS','memo':'both static seeds down standby failover regression'})]
  cmd=f.signed(acts);label='standby-seed-failover-'+str(time.time_ns());f.COMMANDS[label]=cmd;f.write(D/'SUBMITTED_PACKET.json',cmd)
  before={n['account']:[f.balance(n,'eosio.token',account,'SYS'),f.balance(n,'eosio.token',dest,'SYS')]for n in remaining};f.write(D/'BALANCES_BEFORE.json',before)
  result=f.send(label)
  after={n['account']:[f.balance(n,'eosio.token',account,'SYS'),f.balance(n,'eosio.token',dest,'SYS')]for n in remaining}
  assert all(after[a]==[v[0]-1,v[1]+1]for a,v in before.items())
  f.write(D/'SEEDS_DOWN_RESULT.json',dict(transaction=result,balances_after=after,remaining=27,quorum_available=len(active-{n['account']for n in victims}),original_confirmation_budget_seconds=60))
  f.ACTIVE['successful_business_transactions'].append(cmd['id'])
 except BaseException as e:failure=e;f.failed(e)
 finally:
  recovered=[]
  for n in stopped:
   recovered.append(dict(account=n['account'],started=m.start_node(n,initial=False,suffix='seedret')))
   persist()
  f.N=nodes;f.write(D/'FAULT_FINALLY_RESTARTED.json',recovered)
  for n in stopped:wait_caught(n,[x for x in remaining if x['account']!=n['account']])
 if failure:raise failure
 copies={n['account']:f.rpc(n,'get_block',dict(block_num_or_id=result['included']['number']))['id']for n in nodes}
 assert set(copies.values())=={result['included']['id']}
 f.finish(cid,dict(transaction=result,all_30_block_ids=copies,finally_restored=True,rolling_upgrade_nodes=30,initial_failure_preserved=True,scope='Both configured seed nodes stopped plus third process, single low-frequency transfer; no pressure/TPS'))
 f.record_status('NET-03','PASS',E/'ROLLING_UPGRADE.json','All30 upgraded one at a time from existing data and safety files; seed-fault recovery separately qualified')
 f.write(E/'FINAL_INFOS.json',f.infos())
 f.write(E/'QUALIFICATION.json',dict(status='PASS',source_commit=gates['source_commit'],nodeos_sha256=sha,rolling_restarts=30,seed_failure_case=str(D),all_30_rejoined=True,baseline_promoted=False,TPS='unmeasured'))
 report('standby-seed-failover-multinode-regression-passed-next-functional-rounds-pending')
except BaseException as e:
 report('stopped-on-preserved-failure',error=str(e),traceback=traceback.format_exc())
 if f.ACTIVE and f.ACTIVE.get('status')!='FAIL':f.failed(e)
 raise
