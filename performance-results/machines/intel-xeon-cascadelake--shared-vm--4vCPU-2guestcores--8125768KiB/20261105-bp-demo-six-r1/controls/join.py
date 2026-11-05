"""Enroll and synchronize only the six newly authorized BPs."""
from common import *
import pwd,shlex,traceback
assert not(R/'JOIN_PROGRESS.json').exists(),'Reconcile retained signed stages before a controlled resume'
plan=read('PLAN.json');assert plan['status']=='PREPARED'and len(plan['nodes'])==6
keys=read('private/keys.json');baseline=read('BASELINE.json')
progress={'status':'STARTING','UTC':now(),'completed':[],'current':None,'chain_id':CHAIN}
def report(status,**data):progress.update(status=status,UTC=now(),**data);save('JOIN_PROGRESS.json',progress);print(status,progress.get('current'),flush=True)
def healthy_guard():
 original_pids();i=rpc('get_info');assert i['chain_id']==CHAIN
 old=set(x['owner']for x in baseline['old_producers']);assert not any(x['banned']for x in table('rotcands')if x['owner']in old),'Original BP unexpectedly banned'
 return i
def start_node(node,key):
 root=R/'nodes'/node['name'];assert not root.exists(),'Existing node needs explicit reconciliation'
 (R/'nodes').mkdir(exist_ok=True);(R/'nodes').chmod(0o755);R.chmod(0o755)
 (root/'config').mkdir(parents=True);(root/'data').mkdir()
 source=(C/'bps/openaiclouds/config/config.ini').read_text();identity={'producer-name','signature-provider','peer-private-key','p2p-bp-gossip-endpoint','p2p-peer-address','p2p-server-address','http-server-address','p2p-listen-endpoint','agent-name'}
 lines=[line for line in source.splitlines()if '='not in line or line.split('=',1)[0].strip()not in identity]
 public_profile='\n'.join(lines);assert 'vote-threads = 2'in public_profile and 'chain-state-db-size-mb = 4096'in public_profile and 'validation-mode = full'in public_profile
 extra=['http-server-address = 127.0.0.1:'+str(node['rpc_port']),'p2p-listen-endpoint = 127.0.0.1:'+str(node['p2p_port']),'producer-name = '+node['name'],'p2p-peer-address = 127.0.0.1:9876','p2p-bp-gossip-endpoint = '+node['name']+',127.0.0.1:'+str(node['p2p_port'])+',127.0.0.1','peer-private-key = '+json.dumps([key['peer']['Public key'],key['peer']['Private key']])]
 extra+=['signature-provider = '+key[role]['Public key']+'=KEY:'+key[role]['Private key']for role in ['production','bls']]
 config=root/'config/config.ini';config.write_text(public_profile+'\n'+'\n'.join(extra)+'\n');config.chmod(0o600)
 account=pwd.getpwnam('pureeostest')
 (root/'nodeos.log').touch(mode=0o600)
 for p in [root,*root.rglob('*')]:os.chown(p,account.pw_uid,account.pw_gid)
 root.chmod(0o750)
 assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
 command=[str(V/'bin/nodeos'),'--config-dir',str(root/'config'),'--data-dir',str(root/'data'),'--genesis-json',str(C/'founder/genesis.json')]
 unit=Path('/etc/systemd/system')/node['unit'];assert not unit.exists()
 unit.write_text('[Unit]\nDescription=PureEOS TEST feature-demo BP '+node['name']+'\nAfter=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory='+str(root)+'\nExecStart='+shlex.join(command)+'\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:'+str(root/'nodeos.log')+'\nStandardError=append:'+str(root/'nodeos.log')+'\n[Install]\nWantedBy=multi-user.target\n')
 subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',node['unit']],check=True,capture_output=True)
 deadline=time.monotonic()+900;last_guard=0;sync_start=healthy_guard();advanced=time.monotonic();previous_LIB=sync_start['last_irreversible_block_num'];samples=[]
 while True:
  current=healthy_guard()
  if current['last_irreversible_block_num']>previous_LIB:previous_LIB=current['last_irreversible_block_num'];advanced=time.monotonic()
  assert time.monotonic()-advanced<90,'Native finality stalled while adding a new unregistered node'
  try:
   info=rpc('get_info',port=node['rpc_port']);assert info['chain_id']==CHAIN
   if current['head_block_num']-info['head_block_num']<=12 and info['last_irreversible_block_num']>=sync_start['last_irreversible_block_num']:
    b=rpc('get_block',{'block_num_or_id':info['last_irreversible_block_num']});assert b['id']==info['last_irreversible_block_id'];samples.append({'UTC':now(),'info':info})
    if len(samples)>=3:break
  except (OSError,urllib.error.URLError,RuntimeError):
   assert state(node['unit'])['ActiveState']=='active','New process failed before registration; inspect private node log'
  assert time.monotonic()<deadline,'New node sync deadline; never register an unsynchronized BP';time.sleep(2)
 return {'unit':node['unit'],'PID':int(state(node['unit'])['MainPID']),'public_profile_SHA256':hashlib.sha256(public_profile.encode()).hexdigest(),'native_binary_SHA256':BINARY,'sync':samples}
try:
 report('ENROLLING_SIX_NEW_BPS')
 with Wallet()as wallet:
  for node in plan['nodes']:
   name=node['name'];voter=node['voter'];key=keys[name];healthy_guard();report('CREATING_ACCOUNTS',current=name)
   for role in ['account','voter']:wallet.import_key(key[role])
   auth=permission(key['account']['Public key']);voter_auth=permission(key['voter']['Public key'])
   create=wallet.send('create-'+name,[sysact('newaccount',BANK,{'creator':BANK,'name':name,'owner':auth,'active':auth}),sysact('buyram',BANK,{'payer':BANK,'receiver':name,'quant':'100.0000 AIC'}),sysact('newaccount',BANK,{'creator':BANK,'name':voter,'owner':voter_auth,'active':voter_auth}),sysact('buyram',BANK,{'payer':BANK,'receiver':voter,'quant':'100.0000 AIC'})])
   report('FUNDING_OWN_BOND_AND_STAKES')
   fund=wallet.send('fund-'+name,[transfer(BANK,name,'16000.0000 AIC','TEST new BP own bond/resources '+name),transfer(BANK,voter,'1500.0000 AIC','TEST additional independent voter '+voter),sysact('delegatebw',name,{'from':name,'receiver':name,'stake_net_quantity':node['NET_stake_AIC']+' AIC','stake_cpu_quantity':node['CPU_stake_AIC']+' AIC','transfer':False}),sysact('powerup',name,{'payer':name,'receiver':name,'days':30,'net_frac':5000000000000,'cpu_frac':5000000000000,'max_payment':'100.0000 AIC'}),sysact('bondbp',name,{'producer':name}),sysact('delegatebw',voter,{'from':voter,'receiver':voter,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':node['other_CPU_stake_AIC']+' AIC','transfer':False})])
   report('SYNCING_UNREGISTERED_NODE');runtime=start_node(node,key)
   report('REGISTERING_KEYS_AND_SELF_PLUS_OTHER_VOTES')
   actions=[sysact('regproducer',name,{'producer':name,'producer_key':key['production']['Public key'],'url':'https://aic-test.matrixai.one','location':0}),sysact('regfinkey',name,{'finalizer_name':name,'finalizer_key':key['bls']['Public key'],'proof_of_possession':key['bls']['Proof of Possession']}),sysact('regpeerkey',name,{'proposer_finalizer_name':name,'key':key['peer']['Public key']}),sysact('voteproducer',name,{'voter':name,'proxy':'','producers':[name]}),sysact('voteproducer',voter,{'voter':voter,'proxy':'','producers':[name]})]
   actions[0]['authorization'].insert(0,{'actor':BANK,'permission':'active'})
   registered=wallet.send('register-'+name,actions)
   rows=table('votersv2');by={x['owner']:x for x in rows};assert by[name]['producers']==[name]and by[voter]['producers']==[name]
   assert next(x for x in table('bpbonds')if x['owner']==name)['balance']==100000000
   progress['completed'].append({'name':name,'voter':voter,'create':create,'fund':fund,'registration':registered,'runtime':runtime});report('NEW_BP_REGISTERED_AND_SYNCED')
  producers=table('producersv2');assert len(producers)==30 and len({int(x['total_votes'])for x in producers})==30
  voters={x['owner']:x for x in table('votersv2')}
  assert all(voters[x['owner']]==x for x in baseline['old_voters'])
  assert all(next(y for y in producers if y['owner']==x['owner'])['total_votes']==x['total_votes']for x in baseline['old_producers'])
  cfg=table('bpbondcfg')[0];assert int(cfg['locked'])==3000000000 and not int(cfg['slash_pending'])
  original_pids();report('REGISTERED_AND_SYNCED',registered_count=30,new_self_votes=6,new_other_votes=6,pairwise_distinct_totals=30,old_votes_unchanged=True,bond_configuration=cfg)
 save('JOIN_RESULT.json',{**progress,'status':'PASS'})
except BaseException:
 report('NEEDS_RECONCILIATION',error=traceback.format_exc());raise
