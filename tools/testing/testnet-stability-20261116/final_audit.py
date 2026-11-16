from pathlib import Path
import os,json,sys,time,datetime,subprocess,urllib.request,concurrent.futures,hashlib,http.client,socket
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');sys.path.insert(0,str(R));import stability_guard as g
assert not(R/'FINAL_AUDIT_STARTED.json').exists();assert g.load('STABILITY_INSTALL_RESULT.json')['status'].startswith('PASS_');assert g.load('GUARD_MANUAL_QUALIFICATION_RESULT.json')['status']=='PASS_ONE_GUARDED_RECYCLE';g.save('FINAL_AUDIT_STARTED.json',{'utc':g.now(),'scope':'Newreadonly boundedsoak aftercompletecleanup/replacement/guardian; no paidcontrollers rerun'})
nodes=g.load('GUARD_NODE_MAP.json');samples=[]
for k in range(30):
 i=g.rpc('get_info');f=g.rpc('get_finalizer_info');samples.append({'utc':g.now(),'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'head_age':g.age(i['head_block_time']),'LIB_age':g.age(i['last_irreversible_block_time']),'strong_recent_15s':len([v for v in f['last_tracked_votes']if v['is_vote_strong']and g.age(v['voted_for_block_timestamp'])<15]),'policy_count':len(f['active_finalizer_policy']['finalizers']),'threshold':f['active_finalizer_policy']['threshold']});assert i['chain_id']==g.CHAIN and samples[-1]['head_age']<10 and samples[-1]['LIB_age']<30;assert samples[-1]['policy_count']==21 and samples[-1]['threshold']==15;g.save('FINAL_SOAK_PROGRESS.json',{'utc':g.now(),'samples':samples});time.sleep(5)
assert samples[-1]['LIB']>samples[0]['LIB']+150
common=g.rpc('get_info');anchor=common['last_irreversible_block_num'];commonblock=g.rpc('get_block',{'block_num_or_id':anchor});allnodes={};physical=[]
for n,v in list(nodes.items())+[('history',{'unit':'pureeos-devnet-history-20261105.service','rpc_port':8890})]:
 st=g.state(v['unit']);assert st['MainPID']!='0';info=g.rpc('get_info',port=v['rpc_port']);assert info['chain_id']==g.CHAIN and g.age(info['head_block_time'])<10 and g.age(info['last_irreversible_block_time'])<30;block=g.rpc('get_block',{'block_num_or_id':anchor},port=v['rpc_port']);assert block['id']==commonblock['id'];m={l.split(':')[0]:int(l.split()[1])*1024 for l in(Path('/proc')/st['MainPID']/'status').read_text().splitlines()if l.startswith(('VmRSS:','VmSwap:'))};allnodes[n]={'unit':v['unit'],'PID':st['MainPID'],'memory':m,'info':info,'common_irreversible_block_id':block['id']}
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if str(g.NATIVE)in(p/'maps').read_text():physical.append(int(p.name))
 except OSError:pass
assert len(physical)==26 and set(physical)=={int(v['PID'])for v in allnodes.values()}
ro=g.rows();bans=sorted(n for n,x in ro.items()if x['banned']);assert bans==g.load('BANNED_STOP_PLAN.json')['banned']and len(ro)==47
for n in nodes:assert not ro[n]['banned']
for u in g.load('BANNED_STOP_PLAN.json')['units_to_stop']:assert g.state(u)['MainPID']=='0'
for row in g.load('BANNED_DELETE_PLAN.json')['targets']:
 p=Path(row['path']);assert p.exists()
 if row['kind']=='configuration':assert not list(p.iterdir())
 else:assert all(x.name=='finalizers'for x in p.iterdir())
maintenance=g.rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'bpmaint','json':True,'limit':1000});assert maintenance['rows'][0]['owner']=='montrealmila'and maintenance['rows'][0]['state']==2
oldanchor='00081f5fa4d3716fca57d7516e19e0c32e50bcb6be7e016c1d838b5856b5866d';assert g.rpc('get_block',{'block_num_or_id':532319})['id']==oldanchor and g.rpc('get_block',{'block_num_or_id':532319},port=8890)['id']==oldanchor;genesis={str(p):g.rpc('get_block',{'block_num_or_id':1},port=p)['id']for p in [8888,8890]};assert len(set(genesis.values()))==1
assert hashlib.sha256(g.NATIVE.read_bytes()).hexdigest()==g.PIN
baseline=g.load('REPLACEMENT_LIVE_BASELINE.json');pins={}
for n,pin in baseline['code_ABI_pins'].items():
 v=g.rpc('get_raw_abi',{'account_name':n});assert all(v[k]==h for k,h in pin.items());pins[n]=pin
transactions={p.stem:json.loads(p.read_text())for p in(R/'transactions').glob('*.json')if '-rejection-'not in p.name};assert len(transactions)==25 and all(v['status']=='IRREVERSIBLE'for v in transactions.values())
# Recheck exact canonical receipts; never resubmit or re-sign.
for v in transactions.values():
 c=v['canonical_receipt'];b=g.rpc('get_block',{'block_num_or_id':c['block_num']});assert b['id']==c['block_id']and any((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==c['transaction_id']and t['status']=='executed'for t in b['transactions'])
timers={}
for u in ['pureeos-stability-memory-20261116.timer','pureeos-stability-logs-20261116.timer']:
 d=dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','ActiveState,UnitFileState'],text=True).splitlines()if'='in l);assert d['ActiveState']=='active'and d['UnitFileState']=='enabled';timers[u]=d
for u in ['pureeos-testnet-memory-guard-20261111.timer','pureeos-testnet-health-priority-20261111.timer','pureeos-syslog-rotation-20261111.timer']:assert subprocess.run(['systemctl','is-active',u],capture_output=True,text=True).stdout.strip()=='inactive'
assert g.state('pureeos-devnet-wallet-20261105.service')['MainPID']=='0'
health={}
for name,url,post in [('reader_vhost','http://127.0.0.1/api/v1/chain/get_info',True),('writer_vhost','http://127.0.0.1/rpc/v1/chain/get_info',True),('history_vhost','http://127.0.0.1/history/v2/health',False),('history_api','http://127.0.0.1:7000/v2/health',False),('key_history','http://127.0.0.1:7002/health',False)]:
 try:
  q=urllib.request.Request(url,b'{}'if post else None,{'Content-Type':'application/json','Host':'aic-test.matrixai.one','User-Agent':'Mozilla/5.0'});r=urllib.request.urlopen(q,timeout=10);health[name]={'status':r.status,'value':json.load(r)}
 except Exception as e:health[name]={'error':str(e)}
# Verify the actual origin vhost with correct SNI and normal certificate validation.
for name,path,post in [('tls_reader','/api/v1/chain/get_info',True),('tls_writer','/rpc/v1/chain/get_info',True),('tls_history','/history/v2/health',False)]:
 try:
  conn=http.client.HTTPSConnection('aic-test.matrixai.one',timeout=12);conn._create_connection=lambda addr,timeout,*args:socket.create_connection(('127.0.0.1',443),timeout);conn.request('POST'if post else'GET',path,body=b'{}'if post else None,headers={'Content-Type':'application/json','User-Agent':'Mozilla/5.0'});response=conn.getresponse();health[name]={'status':response.status,'value':json.loads(response.read())};conn.close()
 except Exception as e:health[name]={'error':str(e)}
for n in ['tls_reader','tls_writer']:
 assert health[n].get('status')==200 and health[n]['value']['chain_id']==g.CHAIN and g.age(health[n]['value']['head_block_time'])<15,health[n]
mem={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapFree:','SwapTotal:'))};assert mem['MemAvailable']>1024**3 and mem['SwapFree']>30*1024**3
latest=g.load('GUARD_LATEST.json');assert latest['status']in ['COOLDOWN','NO_SAFE_OVERSIZED_TARGET','CHECK_ONLY_WOULD_RECYCLE_ONE'];assert(R/'LOG_CLEANUP_LATEST.json').exists();logs=g.load('LOG_CLEANUP_LATEST.json');assert logs['status']=='PASS_BOUNDED_LOG_CLEANUP'
result={'status':'PASS_25BPS_HISTORY26_STABLE_REPLACEMENTS_AND_AUTOMATION','utc':g.now(),'samples':samples,'max_head_age':max(x['head_age']for x in samples),'max_LIB_age':max(x['LIB_age']for x in samples),'min_recent15s_strong':min(x['strong_recent_15s']for x in samples),'all26_nodes':allnodes,'physical_native_PIDs':physical,'registered':47,'eligible':25,'banned':bans,'maintenance':maintenance,'common_IR':anchor,'common_IR_id':commonblock['id'],'old_anchor532319_retained':True,'founder_reader_full_genesis_block1':genesis,'new6_bootstrap_only_no_fullgenesis_claim':True,'native_SHA256':g.PIN,'contract_code_ABI_pins_unchanged':pins,'canonical_new_transactions':{n:v['canonical_receipt']for n,v in transactions.items()},'memory':mem,'pressure':{n:Path('/proc/pressure/'+n).read_text()for n in ['memory','cpu','io']},'disk_available_bytes':os.statvfs('/').f_bavail*os.statvfs('/').f_frsize,'timers':timers,'guardian_latest':latest,'log_cleanup':logs,'origin_services':health,'signer_closed':True,'not_a_TPS_or_indefinite_availability_claim':True};g.save('FINAL_AUDIT_RESULT.json',result);print(json.dumps({'status':result['status'],'max_head_age':result['max_head_age'],'max_LIB_age':result['max_LIB_age'],'memory':mem,'health':health}),flush=True)
