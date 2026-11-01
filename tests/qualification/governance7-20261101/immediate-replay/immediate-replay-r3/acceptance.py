from pathlib import Path
import json,hashlib,time,datetime,subprocess,re,traceback
G=Path('/opt/pureeos-governance-seven-day-20261101-r1');R=G/'immediate-replay-r3';SOURCE=G/'immediate-replay-r2';EXPORTED=G/'immediate-replay-r1/export';unit='pureeos-immediate-seven-day-inspect-20261101-r3.service';started=False;begin=time.monotonic()
result={'status':'WAITING_FOR_NATIVE_FULL_STATE_REFERENCE','started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'production_binary_modified':False,'host_clock_modified':False,'existing_browser_chain_modified':False,'TPS':'unmeasured'}
def save():
 (R/'PROGRESS.json').write_text(json.dumps(result,indent=2)+'\n')
def state():return{k:subprocess.check_output(['systemctl','show',unit,'--property='+k,'--value'],text=True).strip()for k in ['MainPID','ActiveState','ExecMainStatus']}
def rpc(method,args={}):
 script="import json,urllib.request,sys;v=json.load(sys.stdin);req=urllib.request.Request('http://127.0.0.1:15090/v1/chain/'+v['method'],data=json.dumps(v['args']).encode(),headers={'Content-Type':'application/json'});print(json.dumps(json.load(urllib.request.urlopen(req,timeout=30))))"
 p=subprocess.run(['nsenter','--target',str(pid),'--net','/usr/bin/python3','-c',script],input=json.dumps({'method':method,'args':args}),text=True,capture_output=True,timeout=45);assert p.returncode==0,method+': '+p.stderr[-1000:];return json.loads(p.stdout)
try:
 save();deadline=time.monotonic()+1200
 while not(G/'immediate-replay-integrity/EXPECTED_INTEGRITY.json').exists():
  p=G/'immediate-replay-integrity/RESULT.json'
  if p.exists():assert json.loads(p.read_text())['status']!='FAIL'
  assert time.monotonic()<deadline;time.sleep(2)
 reference=json.loads((G/'immediate-replay-integrity/EXPECTED_INTEGRITY.json').read_text());expected=json.loads((EXPORTED/'EXPECTED.json').read_text());prior=json.loads((SOURCE/'PROGRESS.json').read_text());assert prior['replay_target_verified_via_RPC']['head_block_id']==expected['head_id']and prior['replay_service']['ExecMainStatus']=='0'
 result.update(status='QUERYING_ACTUAL_NODEOS_WITH_COMPLETE_STATE_HASH',replay_source=str(SOURCE),chain_id=expected['chain_id'],head=expected['head'],head_id=expected['head_id']);save()
 cmd=[str(G/'runtime-package/bin/nodeos'),'--data-dir',str(SOURCE/'node-data'),'--config-dir',str(SOURCE/'node-config'),'--disable-replay-opts','--force-all-checks','--integrity-hash-on-start','--integrity-hash-on-stop','--resource-monitor-space-absolute-gb','8']
 q=subprocess.run(['systemd-run','--no-block','--unit='+unit.removesuffix('.service'),'--property=Type=simple','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=30','--property=RuntimeMaxSec=600','--property=TimeoutStopSec=120','--property=StandardOutput=append:'+str(R/'nodeos-inspect.log'),'--property=StandardError=append:'+str(R/'nodeos-inspect.log')]+cmd,capture_output=True,text=True);assert q.returncode==0,q.stderr;started=True
 deadline=time.monotonic()+90
 while True:
  pid=int(state()['MainPID'])
  if pid:
   try:info=rpc('get_info');break
   except Exception:pass
  assert time.monotonic()<deadline;time.sleep(.5)
 assert info['chain_id']==expected['chain_id']and info['head_block_num']==expected['head']and info['head_block_id']==expected['head_id']
 log=(R/'nodeos-inspect.log').read_text();match=re.search(r'chain database started with hash: ([0-9a-f]{64})',log);assert match,'Missing native integrity hash log';assert match[1]==reference['integrity_hash'],'Full canonical state hash mismatch'
 result['full_state_integrity_hash']=match[1];result['full_state_hash_equal_to_independent_native_replay']=True;save()
 actual_tables=[]
 for code in sorted({t['code']for t in reference['table_metadata']}):
  response=rpc('get_table_by_scope',{'code':code,'limit':1000,'time_limit_ms':5000});assert not response.get('more');actual_tables.extend(response['rows'])
 key=lambda t:(t['code'],t['scope'],t['table'],t['payer'],int(t['count']))
 assert sorted(map(key,actual_tables))==sorted(map(key,reference['table_metadata'])),'Physical table metadata/count/payer mismatch'
 gov=rpc('get_upgrade_state');assert gov==expected['governance'];assert not gov['pending']and gov['review_seconds']==604800 and gov['governance_v2_activated']
 code=rpc('get_code_hash',{'account_name':'eosio'});abi=rpc('get_raw_abi',{'account_name':'eosio','abi_hash':expected['packed_ABI']});assert code['code_hash']==expected['system_WASM']and abi['abi_hash']==expected['packed_ABI']
 def rows(table,scope='eosio',**more):
  v=rpc('get_table_rows',{'code':'eosio','scope':scope,'table':table,'json':True,'limit':1000,**more});assert not v.get('more');return v['rows']
 candidates=rows('rotcands');assert len(candidates)==212 and all(len(c['owner'])==12 for c in candidates)
 indexes=[]
 for field,pos,kind in [('rank','2','i128'),('previous_rank','3','i128'),('ticket','4','i64'),('frozen_epoch','5','i64')]:
  actual=rows('rotcands',index_position=pos,key_type=kind);wanted=sorted(candidates,key=lambda x:(int(x[field]),x['owner']));assert [r['owner']for r in actual]==[r['owner']for r in wanted],field+' index mismatch';indexes.append({'field':field,'position':pos,'rows':len(actual),'exact_order_match':True})
 banned=[c for c in candidates if c['banned']];assert len(banned)==1 and int(banned[0]['ticket'])==0 and int(banned[0]['rank'])>=(((1<<64)-1)<<64)
 rotation=rows('rotation')[0];assert int(rotation['ranked_count'])==int(rotation['capacity'])==211
 tokens=rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC'];assert tokens['supply']=='100000000.0000 AIC'and tokens['max_supply']=='2100000000.0000 AIC'
 params=rpc('get_consensus_parameters')['chain_config'];assert params['max_block_cpu_usage']==3200000 and params['max_block_net_usage']==4194304 and params['max_transaction_net_usage']==524288
 assert int(rows('global2')[0]['new_ram_per_block'])==1024
 schedule=rpc('get_producer_schedule');f=rpc('get_finalizer_info')['active_finalizer_policy'];assert len(schedule['active']['producers'])==len(f['finalizers'])==21 and f['threshold']==15
 feats=rpc('get_activated_protocol_features',{'limit':100})['activated_protocol_features'];names={s['value']for p in feats for s in p.get('specification',[])if s['name']=='builtin_feature_codename'};assert {'DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2','PUREEOS_SEVEN_DAY_GOVERNANCE'}<=names
 start=datetime.datetime.fromisoformat(gov['proposed_at']);end=datetime.datetime.fromisoformat(gov['execute_after']);assert(end-start).total_seconds()==604800
 result.update(status='PASS',finished_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),wall_seconds=round(time.monotonic()-begin,3),production_nodeos_full_replay_pass=True,actual_info=info,actual_governance=gov,system_WASM=code['code_hash'],packed_ABI=abi['abi_hash'],table_count=len(actual_tables),table_metadata_count_and_payers_match=True,canonical_table_record_count=sum(len(t['records'])for t in expected['tables']),secondary_indexes=indexes,candidate_count=212,permanent_ban=banned[0]['owner'],candidate_capacity=211,ranked_count=211,actual_tokens=tokens,finalizers=21,threshold=15,CPU=3200000,NET=4194304,RAM_per_block=1024,review_span_seconds=604800,producer_names_exactly12=True,scope='Accelerated isolated historical signed-chain functional acceptance with unchanged production binary and all checks. Not wall-time7day stability, not current browser-chain upgrade or multi-node/TPS evidence.');save();(R/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
except BaseException:
 result.update(status='FAIL',error=traceback.format_exc());save();(R/'FAILURE.json').write_text(json.dumps(result,indent=2)+'\n');raise
finally:
 if started:subprocess.run(['systemctl','stop',unit],capture_output=True,timeout=150)
