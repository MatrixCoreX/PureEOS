"""Controlled bootstrap refinement; existing canonical funding is never repeated."""
from common import *
import pwd,shlex,traceback
assert read('SNAPSHOT_RESULT.json')['status']=='PASS'
assert state(UNIT)['ActiveState']=='inactive'
assert read('SYNC_BOOTSTRAP_CHANGE.json')['status']=='CONTROLLED_BOOTSTRAP_CHANGE'
assert not(R/'JOIN_RESULT.json').exists()
source=(R/'join.py').read_text();expected=read('NATIVE_CONTROL_SOURCE_MANIFEST.json')['join.py'];assert hashlib.sha256(source.encode()).hexdigest()==expected
split=source.index("\ntry:\n report('ENROLLING_SIX_NEW_BPS')")
prefix=source[:split].replace("assert not(R/'JOIN_PROGRESS.json').exists(),'Reconcile retained signed stages before a controlled resume'","assert (R/'SYNC_BOOTSTRAP_CHANGE.json').exists()")
exec(compile(prefix,'original-join-definitions','exec'),globals())
checkpoint=read('SNAPSHOT_RESULT.json');snapshot=Path(checkpoint['checkpoint_path']);snapshot.parent.chmod(0o755)
with snapshot.open('rb')as h:assert hashlib.file_digest(h,'sha256').hexdigest()==checkpoint['checkpoint_SHA256']
assert subprocess.run(['runuser','-u','pureeostest','--','test','-r',str(snapshot)]).returncode==0
old=read('JOIN_PROGRESS_BEFORE_SNAPSHOT.json');assert old['current']=='tokyoaistlab'and not old['completed']
assert not any(x['owner']=='tokyoaistlab'for x in table('producersv2')),'Unexpected registration must be reconciled before restarting new node'
first=plan['nodes'][0];assert first['name']=='tokyoaistlab'
subprocess.run(['systemctl','stop',first['unit']],check=True);assert state(first['unit'])['ActiveState']=='inactive'
save('SLOW_SYNC_ATTEMPT_RETAINED.json',{'status':'RETAINED','UTC':now(),'name':'tokyoaistlab','stopped_only_new_unregistered_unit':first['unit'],'original_data_kept':str(R/'nodes/tokyoaistlab/data'),'original_node_PIDs':original_pids(),'native_snapshot':checkpoint,'old_create_and_fund_receipts':[read('transactions/create-tokyoaistlab.json'),read('transactions/fund-tokyoaistlab.json')],'scope':'Setup refinement on a new unregistered node; not the later fault stop. Original chain/node data unchanged.'})
def start_node(node,key):
 root=R/'nodes'/node['name'];existing=root.exists();account=pwd.getpwnam('pureeostest')
 assert not existing or node['name']=='tokyoaistlab'
 (R/'nodes').mkdir(exist_ok=True);(R/'nodes').chmod(0o755);R.chmod(0o755)
 if not existing:(root/'config').mkdir(parents=True)
 data=root/'data-checkpoint';assert not data.exists();data.mkdir()
 source=(C/'bps/openaiclouds/config/config.ini').read_text();identity={'producer-name','signature-provider','peer-private-key','p2p-bp-gossip-endpoint','p2p-peer-address','p2p-server-address','http-server-address','p2p-listen-endpoint','agent-name'}
 lines=[line for line in source.splitlines()if '='not in line or line.split('=',1)[0].strip()not in identity];public_profile='\n'.join(lines)
 assert 'validation-mode = full'in public_profile and 'vote-threads = 2'in public_profile
 extra=['http-server-address = 127.0.0.1:'+str(node['rpc_port']),'p2p-listen-endpoint = 127.0.0.1:'+str(node['p2p_port']),'producer-name = '+node['name'],'p2p-peer-address = 127.0.0.1:9876','p2p-bp-gossip-endpoint = '+node['name']+',127.0.0.1:'+str(node['p2p_port'])+',127.0.0.1','peer-private-key = '+json.dumps([key['peer']['Public key'],key['peer']['Private key']])]
 extra+=['signature-provider = '+key[role]['Public key']+'=KEY:'+key[role]['Private key']for role in ['production','bls']]
 config=root/'config/config.ini';text=public_profile+'\n'+'\n'.join(extra)+'\n'
 if existing:assert config.read_text()==text,'Keep existing private node identities and public settings'
 else:config.write_text(text);config.chmod(0o600)
 (root/'nodeos.log').touch(mode=0o600)
 for p in [root,*root.rglob('*')]:os.chown(p,account.pw_uid,account.pw_gid)
 root.chmod(0o750)
 assert hashlib.sha256((V/'libexec/nodeos').read_bytes()).hexdigest()==BINARY
 command=['/usr/bin/python3',str(R/'node_entry.py'),node['name'],str(root/'config'),str(data),str(snapshot)]
 unit=Path('/etc/systemd/system')/node['unit']
 if existing:save('OLD_NEW_NODE_UNIT.json',{'unit':node['unit'],'text':unit.read_text()})
 else:assert not unit.exists()
 unit.write_text('[Unit]\nDescription=PureEOS TEST feature-demo BP '+node['name']+'\nAfter=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory='+str(root)+'\nExecStart='+shlex.join(command)+'\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:'+str(root/'nodeos.log')+'\nStandardError=append:'+str(root/'nodeos.log')+'\n[Install]\nWantedBy=multi-user.target\n')
 subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',node['unit']],check=True,capture_output=True)
 sync_start=healthy_guard();deadline=time.monotonic()+900;previous=sync_start['last_irreversible_block_num'];last_advance=time.monotonic();samples=[]
 while True:
  current=healthy_guard()
  if current['last_irreversible_block_num']>previous:previous=current['last_irreversible_block_num'];last_advance=time.monotonic()
  assert time.monotonic()-last_advance<90,'Original finality stalled during new-node checkpoint bootstrap'
  try:
   info=rpc('get_info',port=node['rpc_port']);assert info['chain_id']==CHAIN
   if current['head_block_num']-info['head_block_num']<=12 and info['last_irreversible_block_num']>=sync_start['last_irreversible_block_num']:
    b=rpc('get_block',{'block_num_or_id':info['last_irreversible_block_num']});assert b['id']==info['last_irreversible_block_id'];samples.append({'UTC':now(),'info':info})
    if len(samples)>=3:break
  except (OSError,urllib.error.URLError,RuntimeError):assert state(node['unit'])['ActiveState']=='active','New checkpoint node failed; preserve private logs'
  assert time.monotonic()<deadline;time.sleep(2)
 return {'unit':node['unit'],'PID':int(state(node['unit'])['MainPID']),'public_profile_SHA256':hashlib.sha256(public_profile.encode()).hexdigest(),'native_binary_SHA256':BINARY,'sync':samples,'bootstrap':'Native same-chain irreversible state checkpoint, then full validation','checkpoint_SHA256':checkpoint['checkpoint_SHA256'],'checkpoint_block':checkpoint['canonical_snapshot_block']}
exec(compile(source[split:],'original-canonical-idempotent-enrollment-stages','exec'),globals())
