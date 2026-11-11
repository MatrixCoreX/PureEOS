"""Fresh four identities and public baseline, only after same-chain optimization passes."""
from finance_common import *
import pwd
OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
MAPPING={'alibabacloud':'alibabaailab','deepmindlabs':'deepmindtech','openaiclouds':'openaimodels','sydneyunswai':'sydneyunswcs'}
assert not(R/'PREPARE_STARTED.json').exists(),'Never generate duplicate keys or accounts'
assert read('ROLL_RESULT.json')['status']=='PASS_ALL21_RUNTIME_OPTIMIZED'
i=rpc('get_info');assert i['chain_id']==CHAIN
assert(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(i['head_block_time']+'+00:00')).total_seconds()<5
m={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapFree:'))}
assert m['SwapFree']>20*1024**3 and m['MemAvailable']>700*1024**2,'Optimize before refilling all slots'
assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
rot=table('rotcands');banned={x['owner']for x in rot if x['banned']};assert len(banned)==15 and set(MAPPING)<=banned
for n in MAPPING.values():
 assert len(n)==12
 try:rpc('get_account',{'account_name':n});raise AssertionError('Account already exists '+n)
 except RuntimeError as e:assert'account_query_exception'in str(e)
nodes=read('ROLL_STARTED.json')['initial_PIDs']
oldnodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text());oldnodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
pids={x['unit']:int(state(x['unit'])['MainPID'])for n,x in oldnodes.items()if n not in banned}
pids['pureeos-devnet-history-20261105.service']=int(state('pureeos-devnet-history-20261105.service')['MainPID'])
assert len(pids)==22 and all(pids.values())
voters=table('votersv2');affected=[x for x in voters if x['owner']not in banned and set(MAPPING)&set(x['producers'])]
# Use only the existing owned TEST voter key files; no unowned ballot or authority changes.
index={}
for source in [C/'private/ordinary-keys.json',Path('/opt/pureeos-bp-demo-six-20261105-r1/private/keys.json'),Path('/opt/pureeos-bp-add-two-stop-two-20261106-r1/private/keys.json')]:
 def walk(v,path):
  if isinstance(v,dict):
   if isinstance(v.get('Public key'),str)and isinstance(v.get('Private key'),str):index[v['Public key']]={'source':str(source),'json_path':path,'public_field':'Public key','private_field':'Private key'}
   for k,x in v.items():walk(x,path+[k])
  elif isinstance(v,list):
   for k,x in enumerate(v):walk(x,path+[k])
 walk(json.loads(source.read_text()),[])
owned=[];intents={};moves=[];unowned=[]
for x in affected:
 a=rpc('get_account',{'account_name':x['owner']});auth=next(v['required_auth']for v in a['permissions']if v['perm_name']=='active')
 if auth['threshold']!=1 or len(auth['keys'])!=1 or auth['accounts']or auth['waits']or auth['keys'][0]['key']not in index:
  unowned.append(x['owner']);continue
 pub=auth['keys'][0]['key'];assert auth==permission(pub)
 owned.append({'account':x['owner'],'public_key_sha256':hashlib.sha256(pub.encode()).hexdigest(),'owned_test_signer_found':True,'key_source':index[pub]})
 after=sorted(set(MAPPING.get(n,n)for n in x['producers']));assert not set(after)&banned
 intents[x['owner']]=after;moves.append({'owner':x['owner'],'before':x['producers'],'after':after,'staked_before':x['staked']})
assert balance(BANK)>650000000
baseline={'status':'PASS','utc':now(),'info':i,'memory':m,'node_pids':pids,'banned':sorted(banned),'voters':voters,'affected_external':affected,'unowned_unchanged':unowned,'bonds':table('bpbonds'),'bond_config':table('bpbondcfg'),'producers':table('producersv2'),'maintenance':table('bpmaint'),'token_stats':{c:rpc('get_currency_stats',{'code':c,'symbol':s})for c,s in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]},'code_ABI_pins':{a:{k:v for k,v in rpc('get_raw_abi',{'account_name':a}).items()if k in['code_hash','abi_hash']}for a in['eosio','eosio.token','eosio.key','eosio.signup','usd','cny']},'relays':table('relays','eosio.key','eosio.key')}
save('PREPARE_STARTED.json',{'utc':now(),'mapping':MAPPING,'accounts_absent':True,'no_chain_submissions_yet':True})
save('REPLACEMENT_LIVE_BASELINE.json',baseline);save('BASELINE.json',{'node_pids':pids});save('VOTE_SIGNER_OWNERSHIP.json',{'status':'PASS','accounts':owned,'unowned_unchanged':unowned})
save('AUTHORIZED_VOTE_INTENTS.json',{'intents':intents,'moves':moves,'scope':'Only owned external voters of these4newlybanned identities; keep all nonbanned choices and all oldbanned selfvotes'})
save('AUTHORIZED_VOTE_TARGETS.json',{'eligible':sorted(({x['owner']for x in rot}-banned)|set(MAPPING.values()))})
private=R/'private';private.mkdir(mode=0o700,exist_ok=True);keys={};plan=[]
for idx,(old,n)in enumerate(MAPPING.items(),start=61):
 roles={}
 for role in ['account','production','peer','bls']:
  roles[role]=new_key(bls=role=='bls');keys[n]=roles;save('private/replacement-keys.json',keys)
 plan.append({'name':n,'replaces':old,'rpc_port':8900+idx,'p2p_port':9900+idx,'unit':'pureeos-bp-replacement-'+n+'-20261111.service','CPU_stake_AIC':'100.00'+str(idx),'NET_stake_AIC':'10.0000','bond_AIC':'10000.0000','funding_AIC':'16000.0000','public_keys':{r:k['Public key']for r,k in roles.items()}})
owner=pwd.getpwnam('pureeostest');os.chown(R,0,owner.pw_gid);R.chmod(0o750)
save('REPLACEMENTS_PREPARED.json',{'status':'PREPARED_NOT_SUBMITTED','utc':now(),'nodes':plan,'mapping':MAPPING,'physical_BP_target':25,'history_target':1,'registered_before':37,'registered_after':41,'banned_unchanged':15,'native_or_consensus_changes':False})
os.chown(R/'REPLACEMENTS_PREPARED.json',0,owner.pw_gid);(R/'REPLACEMENTS_PREPARED.json').chmod(0o640)
save('SNAPSHOT_REQUEST_STARTED.json',{'utc':now(),'info':rpc('get_info')})
q=urllib.request.Request('http://127.0.0.1:8888/v1/producer/create_snapshot',b'{}',{'Content-Type':'application/json'})
with urllib.request.urlopen(q,timeout=90)as f:snap=json.load(f)
save('SNAPSHOT_RESPONSE.json',snap);source=Path(snap['snapshot_name']);assert source.is_file()and not source.is_symlink()
target=R/'replacement-checkpoint.bin';assert not target.exists();os.link(source,target);os.chown(target,0,owner.pw_gid);target.chmod(0o640)
assert rpc('get_info')['last_irreversible_block_num']>=snap['head_block_num'];b=rpc('get_block',{'block_num_or_id':snap['head_block_num']});assert b['id']==snap['head_block_id']
save('REPLACEMENT_CHECKPOINT.json',{'status':'PASS','utc':now(),'snapshot':str(target),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'bytes':target.stat().st_size,'block_num':b['block_num'],'block_id':b['id'],'chain_id':CHAIN})
original_pids();print(json.dumps({'status':'PASS_FOUR_KEYS_BASELINE_AND_CHECKPOINT_PREPARED','nodes':list(MAPPING.values()),'owned_votes_to_redirect':len(owned),'unowned_votes_unchanged':unowned,'financial_submissions':0}))
