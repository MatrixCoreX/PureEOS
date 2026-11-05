"""Read-only current/new and archived/old separation plus authority audit."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import datetime,hashlib,json,os,subprocess,urllib.request,urllib.error,time
R=Path('/opt/pureeos-devnet-20261105-r1');phase=os.environ.get('AUDIT_PHASE','final');out=R/('CURRENT_AUDIT_'+phase+'.json');assert not out.exists()
plan=json.loads((R/'PLAN.json').read_text());roll=json.loads((R/'DEPTH_RUNTIME_UPGRADE.json').read_text());assert roll['status']=='PASS'
read_retries=[]
def rpc(n,data={},port=8888):
 for attempt in range(4):
  try:
   with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+n,json.dumps(data).encode(),{'Content-Type':'application/json'}),timeout=15)as x:return json.load(x)
  except urllib.error.HTTPError as e:
   body=e.read().decode(errors='replace');read_retries.append({'method':n,'account':data.get('account_name'),'HTTP':e.code,'body':body[:3000],'attempt':attempt+1})
   if e.code not in[500,502,503,504]or attempt==3:raise RuntimeError(f'Read {n}: HTTP{e.code}: '+body[:3000])from e
   time.sleep(.3*(attempt+1))
def table(n):
 x=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':n,'json':True,'limit':500});assert not x.get('more');return x['rows']
def unit(u):
 x=subprocess.check_output(['systemctl','show',u,'-p','ActiveState','-p','SubState','-p','MainPID','-p','NRestarts','-p','UnitFileState'],text=True);return dict(l.split('=',1)for l in x.splitlines()if'='in l)
start=rpc('get_info');assert start['chain_id']==json.loads((R/'BOOTSTRAP_RESULT.json').read_text())['chain_id']
gov=rpc('get_upgrade_state');assert gov['sealed']and gov['developer']=='eosio.dev'
bps=[x['name']for x in plan['BPs']];accounts={}
with ThreadPoolExecutor(max_workers=3)as pool:
 for n,a in zip([*gov['protected_accounts'],'eosio.dev',*bps],pool.map(lambda n:rpc('get_account',{'account_name':n}),[*gov['protected_accounts'],'eosio.dev',*bps])):accounts[n]={'privileged':a['privileged'],'permissions':a['permissions']}
for n in bps+['eosio.dev']:assert not accounts[n]['privileged']
for n in gov['protected_accounts']:
 p={v['perm_name']:v['required_auth']for v in accounts[n]['permissions']};owner=p['owner'];active=p['active'];assert owner=={'threshold':1,'keys':[],'accounts':[],'waits':[]},n
 expected=[]if n in{'eosio.prods','eosio.wrap','eosio.msig','eosio.null'}else ([{'permission':{'actor':n,'permission':'eosio.code'},'weight':1}]if n in{'eosio'}else[{'permission':{'actor':'eosio','permission':'eosio.code'},'weight':1},{'permission':{'actor':n,'permission':'eosio.code'},'weight':1}])
 assert not active['keys']and not active['waits']and active['threshold']==1 and active['accounts']==expected,n
p={v['perm_name']:v['required_auth']for v in accounts['eosio.dev']['permissions']}
for role,actor,ref in [('owner','civicgovlead','owner'),('active','civicgovlead','active'),('upgrade','civicgovauth','active')]:assert p[role]=={'threshold':1,'keys':[],'accounts':[{'permission':{'actor':actor,'permission':ref},'weight':1}],'waits':[]}
if phase=='final':
 assert json.loads((R/'POST_CREATION_R2_RESULT.json').read_text())['status']=='PASS'
 for n in ['usd','cny']:
  a=rpc('get_account',{'account_name':n});assert not a['privileged']and n not in gov['protected_accounts'];p={v['perm_name']:v['required_auth']for v in a['permissions']}
  assert p['owner']=={'threshold':1,'keys':[],'accounts':[{'permission':{'actor':'eosio.dev','permission':'owner'},'weight':1}],'waits':[]}
  assert p['active']['threshold']==1 and not p['active']['keys']and not p['active']['waits']and {(x['permission']['actor'],x['permission']['permission'],x['weight'])for x in p['active']['accounts']}=={('eosio.dev','active',1),(n,'eosio.code',1)}
  accounts[n]={'privileged':False,'permissions':a['permissions']}
 assert {'eosio.key','eosio.signup'}<=set(gov['protected_accounts'])
bonds=table('bpbonds');prod=table('producersv2');voters=table('votersv2');assert len(bonds)==len(prod)==24 and {p['owner']for p in prod}==set(bps);assert all(int(x['balance'])==100000000 and int(x['forfeited'])==0 for x in bonds)
assert len({int(x['total_votes'])for x in prod})==24
voters_by={x['owner']:x for x in voters};assert all(voters_by[n]['producers']==[n]for n in bps)
ordinary=json.loads((R/'voting/PLAN.json').read_text())['voters'];assert len(ordinary)==90
for x in ordinary:assert voters_by[x['name']]['producers']==[x['producer']]
assert {x['producer']for x in ordinary}==set(bps)
fin=rpc('get_finalizer_info');assert len(fin['active_finalizer_policy']['finalizers'])==21 and fin['active_finalizer_policy']['threshold']==15
assert hashlib.sha256(Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos').read_bytes()).hexdigest()==plan['nodeos_SHA256']
records=roll['completed']
def checknode(e):
 u=unit(e['unit']);assert int(u['MainPID'])==e['new_PID']and u['ActiveState']=='active';p=Path('/proc',u['MainPID']);elf=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos');assert str(elf).encode()in(p/'cmdline').read_bytes().split(b'\0')and str(elf)in(p/'maps').read_text()
 port=8890 if e['name']=='history'else next(x['rpc_port']for x in plan['BPs']if x['name']==e['name']);i=rpc('get_info',port=port);assert i['chain_id']==start['chain_id']and i['last_irreversible_block_num']>=start['last_irreversible_block_num']-80
 return{'name':e['name'],'unit':e['unit'],'process':u,'info':i}
with ThreadPoolExecutor(max_workers=3)as pool:nodes=list(pool.map(checknode,records))
assert len(nodes)==25
old=json.loads((R/'archive/OLD_FINAL.json').read_text());oldunits={u:unit(u)for u in old['original_nodes']};assert len(oldunits)==26 and all(v['ActiveState']=='inactive'and v['UnitFileState']=='disabled'and v['MainPID']=='0'for v in oldunits.values())
assert Path(old['old_root']).exists()
stats=rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC'];assert stats['supply']=='100000000.0000 AIC'and stats['max_supply']=='2100000000.0000 AIC'
assert rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']=='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
assert rpc('get_code_hash',{'account_name':'eosio'})['code_hash']=='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
deadline=time.monotonic()+90
while True:
 end=rpc('get_info')
 if end['last_irreversible_block_num']>start['last_irreversible_block_num']:break
 assert time.monotonic()<deadline,'Read-only finality progress timeout';time.sleep(.5)
a=R/'activity/STATE.json';activity=json.loads(a.read_text());activity={k:activity.get(k)for k in ['status','completed_batches','business_transactions','transfers','ticks','notes','empty_blocks','batch_sizes','UTC']}
result={'status':'PASS','phase':phase,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':start['chain_id'],'start':start,'end':end,'governance':gov,'accounts':accounts,'read_retries':read_retries,'nodes':nodes,'actual_native_finalizers':fin,'consensus_parameters':rpc('get_consensus_parameters'),'BP_bonds':bonds,'self_and_ordinary_votes':{'BPs':24,'ordinary':90,'all_BPs_covered':True,'pairwise_distinct_totals':True},'AIC_stats':stats,'archived_old26_disabled':oldunits,'old_receipt_SHA256':hashlib.sha256((R/'archive/OLD_FINAL.json').read_bytes()).hexdigest(),'ordinary':activity,'scope':'Read-only, no real-user mutation attempts; current controls supported by native58 real/hostile private fixtures. No TPS/capacity/absolute consensus-censorship guarantee.'}
out.write_text(json.dumps(result,indent=2)+'\n');print('CURRENT_READONLY_AUTHORITY_NODE_VOTE_OLD_ARCHIVE_AUDIT_PASS '+phase)
