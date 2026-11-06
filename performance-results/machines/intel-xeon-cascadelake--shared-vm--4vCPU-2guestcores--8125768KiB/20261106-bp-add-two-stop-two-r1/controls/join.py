"""Enroll two owned BPs, synchronize before registration, and verify actual outcomes."""
from common import *
import pwd,shlex,traceback

assert not(R/'JOIN_PROGRESS.json').exists(),'Reconcile immutable packets and canonical receipts before resuming'
plan=read('PLAN.json');keys=read('private/keys.json');baseline=read('BASELINE.json');checkpoint=read('SNAPSHOT_RESULT.json');assert checkpoint['status']=='PASS'
progress={'status':'STARTING','UTC':now(),'chain_id':CHAIN,'completed':[]}
def report(status,**data):progress.update(status=status,UTC=now(),**data);save('JOIN_PROGRESS.json',progress);print(status,progress.get('current'),flush=True)
def guard():
 original_pids();i=rpc('get_info');assert i['chain_id']==CHAIN
 assert i['head_block_num']-i['last_irreversible_block_num']<=80,'Original finality lag too high for new enrollment'
 return i
def start_node(node,key):
 root=R/'nodes'/node['name'];assert not root.exists();(R/'nodes').mkdir(exist_ok=True,mode=0o755);(R/'nodes').chmod(0o755);(root/'config').mkdir(parents=True);data=root/'data-checkpoint';data.mkdir()
 source=(C/'bps/openaiclouds/config/config.ini').read_text();identity={'producer-name','signature-provider','peer-private-key','p2p-bp-gossip-endpoint','p2p-peer-address','p2p-server-address','http-server-address','p2p-listen-endpoint','agent-name'}
 public='\n'.join(line for line in source.splitlines()if '='not in line or line.split('=',1)[0].strip()not in identity)
 assert 'validation-mode = full'in public and 'vote-threads = 2'in public and 'chain-state-db-size-mb = 4096'in public
 extra=['http-server-address = 127.0.0.1:'+str(node['rpc_port']),'p2p-listen-endpoint = 127.0.0.1:'+str(node['p2p_port']),'producer-name = '+node['name'],'p2p-peer-address = 127.0.0.1:9876','p2p-bp-gossip-endpoint = '+node['name']+',127.0.0.1:'+str(node['p2p_port'])+',127.0.0.1','peer-private-key = '+json.dumps([key['peer']['Public key'],key['peer']['Private key']])]
 extra+=['signature-provider = '+key[role]['Public key']+'=KEY:'+key[role]['Private key']for role in ['production','bls']]
 config=root/'config/config.ini';config.write_text(public+'\n'+'\n'.join(extra)+'\n');config.chmod(0o600);(root/'nodeos.log').touch(mode=0o600)
 owner=pwd.getpwnam('pureeostest')
 for p in [root,*root.rglob('*')]:os.chown(p,owner.pw_uid,owner.pw_gid)
 root.chmod(0o750)
 command=['/usr/bin/python3',str(R/'node_entry.py'),node['name'],str(root/'config'),str(data),checkpoint['checkpoint_path']]
 unit=Path('/etc/systemd/system')/node['unit'];assert not unit.exists()
 unit.write_text('[Unit]\nDescription=PureEOS additional owned TEST BP '+node['name']+'\nAfter=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory='+str(root)+'\nExecStart='+shlex.join(command)+'\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:'+str(root/'nodeos.log')+'\nStandardError=append:'+str(root/'nodeos.log')+'\n[Install]\nWantedBy=multi-user.target\n')
 subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',node['unit']],check=True,capture_output=True)
 before=guard();previous=before['last_irreversible_block_num'];advanced=time.monotonic();deadline=time.monotonic()+900;samples=[]
 while True:
  current=guard()
  if current['last_irreversible_block_num']>previous:previous=current['last_irreversible_block_num'];advanced=time.monotonic()
  assert time.monotonic()-advanced<90,'Finality did not advance; no additional registration'
  try:
   i=rpc('get_info',port=node['rpc_port']);assert i['chain_id']==CHAIN
   if current['head_block_num']-i['head_block_num']<=12 and i['last_irreversible_block_num']>=before['last_irreversible_block_num']:
    b=rpc('get_block',{'block_num_or_id':i['last_irreversible_block_num']});assert b['id']==i['last_irreversible_block_id'];samples.append({'UTC':now(),'info':i})
    if len(samples)>=3:break
  except (OSError,urllib.error.URLError,RuntimeError):assert state(node['unit'])['ActiveState']=='active','New node failed; retain private logs'
  assert time.monotonic()<deadline,'New-node synchronization deadline';time.sleep(2)
 return {'unit':node['unit'],'PID':int(state(node['unit'])['MainPID']),'public_profile_SHA256':hashlib.sha256(public.encode()).hexdigest(),'native_binary_SHA256':BINARY,'sync_samples':samples,'checkpoint':checkpoint['canonical_snapshot_block']}
try:
 with Wallet()as wallet:
  for node in plan['nodes']:
   name=node['name'];voter=node['voter'];key=keys[name];guard();report('CREATING_OWNED_ACCOUNTS',current=name)
   for role in ['account','voter']:wallet.import_key(key[role])
   account=permission(key['account']['Public key']);va=permission(key['voter']['Public key'])
   create=wallet.send('create-'+name,[sysact('newaccount',BANK,{'creator':BANK,'name':name,'owner':account,'active':account}),sysact('buyram',BANK,{'payer':BANK,'receiver':name,'quant':'100.0000 AIC'}),sysact('newaccount',BANK,{'creator':BANK,'name':voter,'owner':va,'active':va}),sysact('buyram',BANK,{'payer':BANK,'receiver':voter,'quant':'100.0000 AIC'})])
   report('FUNDING_SELF_BOND_AND_RESOURCES')
   fund=wallet.send('fund-'+name,[transfer(BANK,name,'16000.0000 AIC','TEST BP own bond/resources '+name),transfer(BANK,voter,'1500.0000 AIC','TEST independent owned voter '+voter),sysact('delegatebw',name,{'from':name,'receiver':name,'stake_net_quantity':node['NET_stake_AIC']+' AIC','stake_cpu_quantity':node['CPU_stake_AIC']+' AIC','transfer':False}),sysact('powerup',name,{'payer':name,'receiver':name,'days':30,'net_frac':5000000000000,'cpu_frac':5000000000000,'max_payment':'100.0000 AIC'}),sysact('bondbp',name,{'producer':name}),sysact('delegatebw',voter,{'from':voter,'receiver':voter,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':node['other_CPU_stake_AIC']+' AIC','transfer':False})])
   report('SYNCHRONIZING_UNREGISTERED_NEW_NODE');runtime=start_node(node,key);report('REGISTERING_KEYS_SELF_AND_OTHER_VOTES')
   actions=[sysact('regproducer',name,{'producer':name,'producer_key':key['production']['Public key'],'url':'https://aic-test.matrixai.one','location':0}),sysact('regfinkey',name,{'finalizer_name':name,'finalizer_key':key['bls']['Public key'],'proof_of_possession':key['bls']['Proof of Possession']}),sysact('regpeerkey',name,{'proposer_finalizer_name':name,'key':key['peer']['Public key']}),sysact('voteproducer',name,{'voter':name,'proxy':'','producers':[name]}),sysact('voteproducer',voter,{'voter':voter,'proxy':'','producers':[name]})];actions[0]['authorization'].insert(0,{'actor':BANK,'permission':'active'})
   registration=wallet.send('register-'+name,actions);progress['completed'].append({'name':name,'voter':voter,'create':create,'fund':fund,'registration':registration,'runtime':runtime});report('NEW_BP_REGISTERED_SYNCED_AND_VOTED')
 producers=table('producersv2');voters={x['owner']:x for x in table('votersv2')};bonds={x['owner']:x for x in table('bpbonds')};cfg=table('bpbondcfg')[0]
 assert len(producers)==32 and len({int(x['total_votes'])for x in producers})==32
 for old in baseline['old_producers']:assert next(p for p in producers if p['owner']==old['owner'])['total_votes']==old['total_votes']
 assert all(voters[x['owner']]==x for x in baseline['old_voters']if x['producers'])
 assert all(bonds[x['owner']]==x for x in baseline['old_bonds'])
 for node in plan['nodes']:
  assert voters[node['name']]['producers']==[node['name']]and voters[node['voter']]['producers']==[node['name']]
  assert int(bonds[node['name']]['balance'])==100000000 and not int(bonds[node['name']]['forfeited'])
 assert int(cfg['locked'])==int(baseline['bond_config']['locked'])+200000000
 for name in ['slash_pending','slash_settled']:assert cfg[name]==baseline['bond_config'][name]
 for name,sym in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]:assert rpc('get_currency_stats',{'code':name,'symbol':sym})==baseline['token_stats'][name]
 for name,pin in baseline['code_ABI_pins'].items():
  p=rpc('get_raw_abi',{'account_name':name});assert p['code_hash']==pin['code_hash']and p['abi_hash']==pin['abi_hash']
 assert table('relays','eosio.key','eosio.key')==baseline['relay_rows'];original_pids()
 report('PASS',registered_count=32,new_self_votes=2,new_other_votes=2,all32_vote_totals_distinct=True,old_votes_bonds_and_contracts_unchanged=True,existing24_PIDs_unchanged=True)
 save('JOIN_RESULT.json',{**progress,'status':'PASS','scope':'Two new owned BP processes synchronized and registered. Real duty/production waits for native FIFO; no duty/throughput qualification claim.'})
except BaseException:
 report('NEEDS_RECONCILIATION',error=traceback.format_exc());raise
