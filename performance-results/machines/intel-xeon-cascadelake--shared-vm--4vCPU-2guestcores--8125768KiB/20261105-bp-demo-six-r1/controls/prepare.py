"""Read current baseline and generate only the six authorized identities."""
from common import *
import random,shutil
assert not(R/'BASELINE.json').exists(),'Reconcile this campaign; do not generate identities twice'
assert rpc('get_info')['chain_id']==CHAIN and rpc('get_info',port=8890)['chain_id']==CHAIN
assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
previous=json.loads(Path('/opt/pureeos-relay-execution-income-20261105-r1/BEFORE.json').read_text())['node_pids']
current={u:int(state(u)['MainPID'])for u in previous};assert current==previous and len(current)==25
producers=table('producersv2');voters=table('votersv2');bonds=table('bpbonds');candidates=table('rotcands');rotation=table('rotation')[0]
assert len(producers)==len(bonds)==24 and not any(x['banned']for x in candidates)
assert all(int(x['balance'])==100000000 and not int(x['forfeited'])for x in bonds)
assert balance(BANK)>1100000000
cfg=table('bpbondcfg')[0];assert int(cfg['locked'])==2400000000 and not int(cfg['slash_pending'])and not int(cfg['slash_settled'])
stats={n:rpc('get_currency_stats',{'code':n,'symbol':sym})for n,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]}
assert stats['eosio.token']['AIC']['supply']=='100000000.0000 AIC'and stats['eosio.token']['AIC']['max_supply']=='2100000000.0000 AIC'
assert all(stats[n][n.upper()]['supply']=='100010000.0000 '+n.upper()and stats[n][n.upper()]['max_supply']=='1000000000.0000 '+n.upper()for n in ['usd','cny'])
pins={n:rpc('get_raw_abi',{'account_name':n})for n in ['eosio','eosio.token','eosio.key','eosio.signup','usd','cny']}
assert pins['eosio.key']['code_hash']=='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'
plan=read('PLAN.json');names=plan['new_BPs'];voter_names=['cedarharbors','mapleharbors','birchmeadows','olivegardens','pinemoonvale','oakriverbank']
assert len(set(names+voter_names))==12 and all(len(n)==12 for n in names+voter_names)
for n in names+voter_names:
 try:rpc('get_account',{'account_name':n})
 except RuntimeError as e:assert 'unknown key'in str(e).lower()or'unknown account'in str(e).lower()or'account_query_exception'in str(e).lower()
 else:raise AssertionError('Identity already exists '+n)
ports=set()
with socket.socket()as s:pass
for port in list(range(8931,8937))+list(range(9931,9937)):
 s=socket.socket();s.settimeout(.2);result=s.connect_ex(('127.0.0.1',port));s.close();assert result!=0,'Port already in use';ports.add(port)
key_snapshot={'wallets':table('wallets','eosio.key','eosio.key'),'assets':table('assets','eosio.key','eosio.key'),'fees':table('feebuckets','eosio.key','eosio.key')}
key_snapshot['balances']={str(w['id']):table('balances','eosio.key',str(w['id']))for w in key_snapshot['wallets']}
save('BASELINE.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'node_pids':current,'old_producers':producers,'old_voters':voters,'old_bonds':bonds,'old_candidates':candidates,'bond_config':cfg,'rotation':rotation,'token_stats':stats,'code_ABI_pins':pins,'KEY_snapshot':key_snapshot,'bank_balance_atoms':balance(BANK),'maintenance':table('bpmaint')})
(R/'private').mkdir(mode=0o700,exist_ok=True);assert not(R/'private/keys.json').exists()
keys={};nodes=[];seed=hashlib.sha256((CHAIN+'|six-native-feature-demo-20261105-r1').encode()).hexdigest();rng=random.Random(seed);weights=rng.sample(range(100,901),6)
for i,(name,voter,weight)in enumerate(zip(names,voter_names,weights)):
 key={'account':new_key(),'production':new_key(),'peer':new_key(),'bls':new_key(True),'voter':new_key()};keys[name]=key
 nodes.append({'name':name,'voter':voter,'index':31+i,'rpc_port':8931+i,'p2p_port':9931+i,'role':'permanent-blacklist-demo'if i<5 else'maintenance-demo','unit':'pureeos-bp-demo-'+name+'-20261105.service','CPU_stake_AIC':'100.'+str(31+i).zfill(4),'other_CPU_stake_AIC':str(weight)+'.0000','NET_stake_AIC':'10.0000','public_keys':{role:value['Public key']for role,value in key.items()}})
 save('private/keys.json',keys)
plan.update(status='PREPARED',nodes=nodes,vote_seed=seed,native_binary_SHA256=BINARY,registration_count_before=24,stake_vote_scope='Six additional owned ordinary accounts plus six BP self-votes; all old votes remain unchanged',start_LIB=rpc('get_info')['last_irreversible_block_num'])
save('PLAN.json',plan);print('SIX_NEW_IDENTITIES_AND_UNCHANGED_CURRENT_BASELINE_PREPARED',flush=True)
