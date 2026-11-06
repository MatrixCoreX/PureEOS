"""Prepare only the two newly authorized owned BP identities and current baseline."""
from common import *

assert not(R/'BASELINE.json').exists()and not(R/'PLAN.json').exists(),'Reconcile existing setup instead of generating keys again'
stops=read('NODE_STOP_RESULT.json');assert stops['status']=='PASS'
info=rpc('get_info');assert info['chain_id']==CHAIN
with(V/'libexec/nodeos').open('rb')as f:assert hashlib.file_digest(f,'sha256').hexdigest()==BINARY
names=['bostonmitlab','torontovectr'];voters=['cambridgeoak','ontariomaple']
assert len(set(names+voters))==4 and all(len(n)==12 for n in names+voters)
for name in names+voters:
 try:rpc('get_account',{'account_name':name})
 except RuntimeError as e:assert any(x in str(e).lower()for x in ['unknown key','unknown account','account_query_exception'])
 else:raise AssertionError('Choose a new identity; account already exists: '+name)
for port in [8941,8942,9941,9942]:
 with socket.socket()as s:s.settimeout(.3);assert s.connect_ex(('127.0.0.1',port))!=0,'Port is already occupied'
producers=table('producersv2');vote_rows=table('votersv2');bonds=table('bpbonds');cfg=table('bpbondcfg')[0]
assert len(producers)==30 and len({int(p['total_votes'])for p in producers})==30
stats={n:rpc('get_currency_stats',{'code':n,'symbol':sym})for n,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]}
pins={}
for name in ['eosio','eosio.token','eosio.key','eosio.signup','usd','cny']:
 p=rpc('get_raw_abi',{'account_name':name});pins[name]={'code_hash':p['code_hash'],'abi_hash':p['abi_hash']}
assert balance(BANK)>354000000
save('BASELINE.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'node_pids':stops['preserved24_PIDs'],'info':info,'old_producers':producers,'old_voters':vote_rows,'old_bonds':bonds,'old_candidates':table('rotcands'),'maintenance':table('bpmaint'),'bond_config':cfg,'bank_balance_atoms':balance(BANK),'token_stats':stats,'code_ABI_pins':pins,'relay_rows':table('relays','eosio.key','eosio.key'),'scope':'After user-authorized Baidu/Montreal stop; these24other current-node PIDs are the preservation baseline, not the historical pre-OOM25.'})
original_pids();(R/'private').mkdir(mode=0o700)
keys={};nodes=[]
for i,(name,voter,other)in enumerate(zip(names,voters,['251.0000','367.0000'])):
 roles={role:new_key(role=='bls')for role in ['account','production','peer','bls','voter']};keys[name]=roles
 nodes.append({'name':name,'voter':voter,'index':41+i,'rpc_port':8941+i,'p2p_port':9941+i,'unit':'pureeos-bp-extra-'+name+'-20261106.service','CPU_stake_AIC':'100.00'+str(41+i),'NET_stake_AIC':'10.0000','other_CPU_stake_AIC':other,'public_keys':{k:v['Public key']for k,v in roles.items()}})
save('private/keys.json',keys)
save('PLAN.json',{'status':'PREPARED','UTC':now(),'chain_id':CHAIN,'remote_root':str(R),'new_BPs':names,'nodes':nodes,'maintenance_target':'','own_bond_AIC_each':'10000.0000','ordinary_voters':voters,'registration_count_before':30,'registration_count_expected':32,'sole_founder_HTTP_relay_retained':True,'snapshot_scope':'Fresh same-chain native irreversible checkpoint, then full validation; no original-state directory copy.','user_scope':'Two nodes on existing host; stop only bannedBaidu and already-maintainedMontreal. No new server, reset, TPS, protocol/contract/key/cap change or old payment replay.'})
print('TWO_NEW_OWNED_IDENTITIES_PREPARED',flush=True)
