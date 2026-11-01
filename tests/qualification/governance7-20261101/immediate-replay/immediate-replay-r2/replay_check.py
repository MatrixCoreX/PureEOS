from pathlib import Path
import json,time,subprocess,hashlib,datetime,shutil,concurrent.futures,traceback
G=Path('/opt/pureeos-governance-seven-day-20261101-r1');R=G/'immediate-replay-r2';BIN=G/'runtime-package/bin/nodeos';runtime=G/'runtime-package/libexec/nodeos';START=time.monotonic();inspection='pureeos-immediate-seven-day-inspect-20261101-r2.service';inspection_started=False
result={'status':'WAITING_FOR_SIGNED_BLOCKLOG','started_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'production_binary_modified':False,'host_clock_modified':False,'existing_browser_chain_modified':False,'TPS':'unmeasured'}
def save():
 (R/'PROGRESS.json').write_text(json.dumps(result,indent=2)+'\n')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def systemd_state(unit):return{key:subprocess.check_output(['systemctl','show',unit,'--property='+key,'--value'],text=True).strip()for key in ['ActiveState','SubState','Result','ExecMainStatus','MainPID']}
def namespace_rpc(pid,method,args={}):
 script="import urllib.request,json,sys;v=json.load(sys.stdin);r=urllib.request.Request('http://127.0.0.1:15090/v1/chain/'+v['method'],data=json.dumps(v['args']).encode(),headers={'Content-Type':'application/json'});print(json.dumps(json.load(urllib.request.urlopen(r,timeout=30))))"
 p=subprocess.run(['nsenter','--target',str(pid),'--net','/usr/bin/python3','-c',script],input=json.dumps({'method':method,'args':args}),text=True,capture_output=True,timeout=45)
 assert p.returncode==0,p.stderr[-1500:];return json.loads(p.stdout)
try:
 save();end=time.monotonic()+1800
 while True:
  p=R/'GENERATOR_RESULT.json'
  if p.exists():
   q=json.loads(p.read_text());assert q['status']!='FAIL','Generator failed; retain failure, never replay invalid corpus'
   if q['status']=='PASS':break
  assert time.monotonic()<end;time.sleep(2)
 expected=json.loads((R/'export/EXPECTED.json').read_text());assert expected['chain_id']!='03c98ab62ea764e74d2d80ac862b10f4d9c4dc1f0b7a3370692a44427f3f7aea'
 assert expected['system_WASM']=='373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb'and expected['packed_ABI']=='038ffbab99e3c261dfc0d1655ddd3acd75b89bfeaae350eb86e4df930564f959'
 assert sha(runtime)=='e6fe4d55986d7ff3ccbccf3cc54943be0555ada07b28483c7954ebc4ab5a35b6'
 (R/'node-data').mkdir();shutil.copytree(R/'export/blocks',R/'node-data/blocks');cfg=R/'node-config';cfg.mkdir()
 source=Path('/opt/pureeos-rotation-demo-20261031-r1/nodes/asindiabpaaa/config')
 shutil.copytree(source/'protocol_features',cfg/'protocol_features')
 allowed={'experimental-parallel-production','experimental-parallel-validation-threads','experimental-parallel-validation-batch-size','experimental-parallel-early-adoption-group','experimental-parallel-early-adoption','experimental-parallel-native-capture','experimental-parallel-code-hash','chain-threads','incoming-transaction-queue-size-mb','chain-state-db-size-mb','max-transaction-time'}
 lines=[x for x in(source/'config.ini').read_text().splitlines()if '='in x and x.split('=',1)[0].strip()in allowed]
 assert 'experimental-parallel-production = true'in lines
 lines+=['plugin = eosio::chain_api_plugin','http-server-address = 127.0.0.1:15090','p2p-listen-endpoint = 127.0.0.1:15999','http-validate-host = false','http-max-response-time-ms = 10000','validation-mode = full','read-mode = head','api-accept-transactions = false','p2p-accept-transactions = false']
 (cfg/'config.ini').write_text('\n'.join(lines)+'\n');(cfg/'config.ini').chmod(0o600)
 cmd=[str(BIN),'--data-dir',str(R/'node-data'),'--config-dir',str(cfg),'--genesis-json',str(R/'export/genesis.json'),'--replay-blockchain','--disable-replay-opts','--force-all-checks','--terminate-at-block',str(expected['head']),'--resource-monitor-space-absolute-gb','8']
 result.update(status='PRODUCTION_NODEOS_FORCE_ALL_CHECKS_REPLAY',exported_chain_id=expected['chain_id'],exported_blocks=expected['head'],nodeos_SHA256=sha(runtime),config_SHA256=sha(cfg/'config.ini'),replay_command=cmd,blocklog_files={str(p.relative_to(R/'export/blocks')):{'SHA256':sha(p),'bytes':p.stat().st_size}for p in(R/'export/blocks').rglob('*')if p.is_file()});save()
 replay_unit='pureeos-immediate-seven-day-replay-20261101-r2'

 q=subprocess.run(['systemd-run','--no-block','--unit='+replay_unit,'--property=Type=simple','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=30','--property=RuntimeMaxSec=1800','--property=StandardOutput=append:'+str(R/'nodeos-replay.log'),'--property=StandardError=append:'+str(R/'nodeos-replay.log')]+cmd,capture_output=True,text=True)
 assert q.returncode==0,q.stderr
 end=time.monotonic()+1800
 while True:
  state=systemd_state(replay_unit+'.service');pid=int(state['MainPID'])
  if pid:
   try:
    probe=namespace_rpc(pid,'get_info')
    if probe['head_block_num']==expected['head']and probe['head_block_id']==expected['head_id']and probe['chain_id']==expected['chain_id']:break
   except Exception:pass
  assert state['ActiveState']not in ['failed','inactive'],'Replay stopped before exact expected canonical target'
  assert time.monotonic()<end,'Replay target deadline';time.sleep(.5)
 result['replay_target_verified_via_RPC']=probe
 subprocess.run(['systemctl','stop',replay_unit+'.service'],check=True,timeout=150)
 assert systemd_state(replay_unit+'.service')['ExecMainStatus']=='0','Replay process did not shut down cleanly'
 (R/'replay-systemd-result.log').write_text('PASS: full replay reached exact chain/head; simple service stopped cleanly after verification.\n')
 result['replay_service']=systemd_state(replay_unit+'.service');result['status']='READING_ACTUAL_NODEOS_REPLAY_STATE';save()
 cmd=[str(BIN),'--data-dir',str(R/'node-data'),'--config-dir',str(cfg),'--disable-replay-opts','--force-all-checks','--resource-monitor-space-absolute-gb','8']
 q=subprocess.run(['systemd-run','--no-block','--unit='+inspection.removesuffix('.service'),'--property=Type=simple','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=30','--property=RuntimeMaxSec=600','--property=TimeoutStopSec=120','--property=StandardOutput=append:'+str(R/'nodeos-inspect.log'),'--property=StandardError=append:'+str(R/'nodeos-inspect.log')]+cmd,capture_output=True,text=True);assert q.returncode==0,q.stderr;inspection_started=True
 end=time.monotonic()+120
 while True:
  state=systemd_state(inspection);pid=int(state['MainPID'])
  if pid:
   try:info=namespace_rpc(pid,'get_info');break
   except Exception:pass
  assert time.monotonic()<end,'Inspect node API did not start';time.sleep(1)
 assert info['chain_id']==expected['chain_id']and info['head_block_num']==expected['head']and info['head_block_id']==expected['head_id']
 assert info['server_full_version_string']=='v2.0.0-5d95aa18fea665f4ee60eda13b01dec82ff43535'
 code=namespace_rpc(pid,'get_code_hash',{'account_name':'eosio'});abi=namespace_rpc(pid,'get_raw_abi',{'account_name':'eosio','abi_hash':expected['packed_ABI']});gov=namespace_rpc(pid,'get_upgrade_state')
 assert code['code_hash']==expected['system_WASM']and abi['abi_hash']==expected['packed_ABI'];assert gov==expected['governance'],'Native governance state differs after actual nodeos replay'
 def verify_table(t):
  lower='';records=[]
  while True:
   v=namespace_rpc(pid,'get_table_rows',{'code':t['code'],'scope':t['scope']or'0','table':t['table'],'json':False,'show_payer':True,'limit':1000,'lower_bound':lower})
   records.extend({'SHA256':hashlib.sha256(bytes.fromhex(x['data'])).hexdigest(),'payer':x['payer']}for x in v['rows'])
   if not v.get('more'):break
   following=str(v['next_key']);assert following and following!=lower;lower=following
  wanted=[{'SHA256':x['SHA256'],'payer':x['payer']}for x in t['records']];assert records==wanted,'Table/payer mismatch '+str((t['code'],t['scope'],t['table']))
  return{'code':t['code'],'scope':t['scope'],'table':t['table'],'rows':len(records),'all_row_hashes_and_payers_match':True}
 with concurrent.futures.ThreadPoolExecutor(max_workers=8)as pool:table_results=list(pool.map(verify_table,expected['tables']))
 params=namespace_rpc(pid,'get_consensus_parameters')['chain_config'];assert params['max_block_cpu_usage']==3200000 and params['max_block_net_usage']==4194304 and params['max_transaction_net_usage']==524288
 schedule=namespace_rpc(pid,'get_producer_schedule');finalizers=namespace_rpc(pid,'get_finalizer_info');assert len(schedule['active']['producers'])==21 and len(finalizers['active_finalizer_policy']['finalizers'])==21 and finalizers['active_finalizer_policy']['threshold']==15
 candidates=namespace_rpc(pid,'get_table_rows',{'code':'eosio','scope':'eosio','table':'rotcands','json':True,'limit':1000})['rows'];assert len(candidates)==212 and all(len(c['owner'])==12 for c in candidates)
 bans=[c for c in candidates if c['banned']];assert len(bans)==1 and int(bans[0]['ticket'])==0 and int(bans[0]['rank'])>=(((1<<64)-1)<<64)
 tokens=namespace_rpc(pid,'get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC'];assert tokens['supply']=='100000000.0000 AIC'and tokens['max_supply']=='2100000000.0000 AIC'
 ram=namespace_rpc(pid,'get_table_rows',{'code':'eosio','scope':'eosio','table':'global2','json':True,'limit':1})['rows'][0];assert int(ram['new_ram_per_block'])==1024
 assert not gov['pending']and gov['review_seconds']==604800 and gov['governance_v2_activated']
 codes={s['value']for f in namespace_rpc(pid,'get_activated_protocol_features',{'limit':100})['activated_protocol_features']for s in f.get('specification',[])if s['name']=='builtin_feature_codename'};assert {'DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2','PUREEOS_SEVEN_DAY_GOVERNANCE'}<=codes
 result.update(status='PASS',finished_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),wall_seconds=round(time.monotonic()-START,3),production_nodeos_full_replay_pass=True,actual_nodeos_info=info,actual_governance=gov,system_code=code,ABI_hash=abi['abi_hash'],all_tables=table_results,table_count=len(table_results),row_count=sum(t['rows']for t in table_results),tokens=tokens,finalizers=21,threshold=15,permanent_ban=bans[0]['owner'],RAM_bytes_per_block=1024,block_cpu_us=params['max_block_cpu_usage'],block_NET_bytes=params['max_block_net_usage'],scope='Independent accelerated historical signed-chain functional acceptance. Not real7day wall-time stability, not current browser-chain contract activation, not multi-machine/TPS evidence.');save();(R/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
except BaseException:
 result.update(status='FAIL',error=traceback.format_exc());save();(R/'FAILURE.json').write_text(json.dumps(result,indent=2)+'\n');raise
finally:
 if inspection_started:subprocess.run(['systemctl','stop',inspection],capture_output=True,timeout=150)
