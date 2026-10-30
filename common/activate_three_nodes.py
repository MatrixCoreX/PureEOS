from pathlib import Path
import os,json,subprocess,shlex,hashlib,time,traceback,urllib.request,concurrent.futures
B=Path('/opt/aic-bp-discovery-live-20261030');P=Path('/opt/aic-bp-discovery-peer-20261030')
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
def read(p):return json.loads(p.read_text())
def write(p,d):
 p.parent.mkdir(parents=True,exist_ok=True);q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def sha(p):
 with p.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
def save(d):d['UTC']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());write(B/'ACTIVATION_STATUS.json',d)
opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
def ssh(n,code):
 p=subprocess.run(['ssh',*opts,'root@'+n['ip'],'python3 -B -'],input=code,capture_output=True,text=True,timeout=180)
 log=B/('private-remote-operation-'+str(time.time_ns())+'.log');log.write_text(p.stderr);log.chmod(0o600)
 assert p.returncode==0,'Remote operation failed; private setup output retained only in protected log'
 return p.stdout
def rpc(port,method,arg=None,namespace='chain'):
 req=urllib.request.Request('http://127.0.0.1:'+str(port)+'/v1/'+namespace+'/'+method,data=json.dumps(arg or{}).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=5)as r:return json.load(r)
def filter_script(root,ips):
 return "import pathlib,subprocess;root=pathlib.Path("+repr(str(root))+");rules="+repr('\n'.join(['table inet aic_bp_live_20261030 {',' set peers { type ipv4_addr; elements = { '+', '.join(ips)+' }; }',' chain ingress { type filter hook input priority -6; policy accept; tcp dport 9876 ip saddr != @peers reject with tcp reset; }','}','']))+";p=root/'NETWORK_FILTER.nft';exists=subprocess.run(['nft','list','table','inet','aic_bp_live_20261030'],capture_output=True).returncode==0;assert not exists or p.exists();q=subprocess.run(['nft','-f','-'],input=rules,capture_output=True,text=True) if not exists else None;assert q is None or q.returncode==0;p.write_text(rules)"
try:
 assert read(B/'RUNTIME_DEPLOYED.json')['nodeos_sha256']=='37666ac6d088eac784399e1a7ea3e957061c80dec4fa354e23ca4528e431f4b2'
 prep=read(B/'NATIVE_PREPARATION_STATUS.json');assert prep['status']=='native-contract-fixture-prepared-awaiting-three-live-parallel-node-activation'
 fixture=read(B/'fixture/PUBLIC_FIXTURE.json');private=read(B/'fixture/PRIVATE_NODES.json')
 assert len(private)==3 and fixture['governance_sealed'] and fixture['deferred_disabled']
 assert fixture['system_code_hash']=='eea11729b51d4c3eb72834345eaf7492373bc94bac664874c7186ebc696e471d'
 assert subprocess.run(['pgrep','-x','nodeos'],capture_output=True).returncode==1
 topology=read(B/'TOPOLOGY.json')
 ips=[topology['primary']['main_ip'],*[p['main_ip']for p in topology['peers']]]
 nodes=[dict(account=private[i]['account'],ip=ips[i],http_port=8888 if i==0 else 38890+i,
             unit='aic-bp-live-'+private[i]['account']+'-20261030-a3',primary=i==0,index=i)for i in range(3)]
 public=dict(chain_id=fixture['chain_id'],nodes=nodes,static_seed_edges=[['bpaaa','bpbbb'],['bpbbb','bpaaa'],['bpccc','bpbbb']],
  nodeos_sha256=sha(B/'runtime/nodeos'),snapshot_sha256=sha(B/'fixture/bootstrap.snapshot'),source_commit='7041fb612454179c475a71ce278c7d57035833dd',
  CPU_multiplier=16,NET_bytes_per_block=16777216,workers_per_node=8,pressure_load=False)
 write(B/'LIVE_TOPOLOGY.json',public)
 save(dict(status='starting-three-real-parallel-BPs-with-signed-gossip',discovery_active=False))
 exec(filter_script(B,['127.0.0.1',*ips]))
 cfg_hashes={}
 for n,secret in zip(nodes,private):
  seed=nodes[1] if n['index']in[0,2] else nodes[0]
  root=B if n['primary'] else P
  D=root/'live'/n['account']
  cfg=['plugin = eosio::chain_api_plugin','plugin = eosio::producer_api_plugin','plugin = eosio::net_plugin','plugin = eosio::net_api_plugin',
   'chain-state-db-size-mb = 4096','chain-state-db-guard-size-mb = 4','resource-monitor-space-absolute-gb = 1',
   'http-server-address = 127.0.0.1:8888','http-validate-host = true','unix-socket-path =',
   'p2p-listen-endpoint = 0.0.0.0:9876','p2p-server-address = '+n['ip']+':9876','p2p-peer-address = '+seed['ip']+':9876',
   'p2p-max-nodes-per-host = 16','p2p-accept-transactions = true','allowed-connection = any',
   'p2p-bp-gossip-endpoint = '+n['account']+','+n['ip']+':9876,'+n['ip'],
   'wasm-runtime = eos-vm-jit','validation-mode = full','read-mode = head',
   'performance-metrics = false','max-transaction-time = 100','incoming-transaction-queue-size-mb = 1024',
   'experimental-parallel-validation-threads = 8','experimental-parallel-validation-batch-size = 24','experimental-parallel-engine = jit',
   'experimental-parallel-production = true','experimental-parallel-early-adoption = true','experimental-parallel-early-adoption-group = 8',
   'experimental-parallel-native-capture = true','experimental-parallel-native-capture-staged = false',
   'experimental-parallel-input-fingerprints = true','experimental-parallel-trace-reserve = true',
   'producer-name = '+n['account'],'enable-stale-production = true',
   'signature-provider = '+secret['block_provider'],'signature-provider = '+secret['finalizer_provider'],
   'peer-private-key = '+json.dumps([secret['peer_public'],secret['peer_private']],separators=(',',':')),
   'peer-private-key = '+json.dumps([secret['next_peer_public'],secret['next_peer_private']],separators=(',',':'))]
  for h in sorted(set([fixture['system_code_hash'],fixture['token_code_hash'],fixture['tokenone_code_hash']])):cfg.append('experimental-parallel-code-hash = '+h)
  text='\n'.join(cfg)+'\n';cfg_hashes[n['account']]=hashlib.sha256(text.encode()).hexdigest()
  if n['primary']:
   for name in ['config','data','tmp']:
    (D/name).mkdir(parents=True,exist_ok=False);os.chown(D/name,1000,1000)
   os.chmod(D,0o755)
   (D/'config/config.ini').write_text(text);os.chmod(D/'config/config.ini',0o600);os.chown(D/'config/config.ini',1000,1000)
   args=['systemd-run','--unit='+n['unit'],'--property=RootDirectory=/opt/aic-tps/rootfs','--property=User=1000','--property=Group=1000',
    '--property=BindReadOnlyPaths=/opt:/opt','--property=BindPaths='+str(B)+':/campaign','--property=MemoryMax=56G','--property=MemorySwapMax=0',
    '--property=CPUAffinity=1-7 9-15','--property=RuntimeMaxSec=14400','--property=RemainAfterExit=yes',
    '--property=StandardOutput=append:'+str(D/'node.log'),'--property=StandardError=append:'+str(D/'node.log'),
    '/opt/aic-bp-discovery-live-20261030/runtime/nodeos','--config-dir','/campaign/live/'+n['account']+'/config',
    '--data-dir','/campaign/live/'+n['account']+'/data','--snapshot','/campaign/fixture/bootstrap.snapshot',
    '--chain-threads','6','--force-all-checks','--disable-replay-opts']
   r=subprocess.run(args,capture_output=True,text=True);assert r.returncode==0
  else:
   setup="import pathlib,os;root=pathlib.Path("+repr(str(P))+");assert not(root/'live').exists();(root/'live').mkdir();"
   ssh(n,setup)
   for filename in ['bootstrap.snapshot','PUBLIC_FIXTURE.json','genesis.json']:
    r=subprocess.run(['scp','-q',*opts,str(B/'fixture'/filename),'root@'+n['ip']+':'+str(P/'fixture'/filename)],capture_output=True,text=True,timeout=180);assert r.returncode==0
   code="import os,pathlib,subprocess,pwd,hashlib,json;D=pathlib.Path("+repr(str(D))+");uid=1100;gid=pwd.getpwuid(uid).pw_gid;\n"
   code+="for name in ['config','data','tmp']:\n (D/name).mkdir(parents=True,exist_ok=False);os.chown(D/name,uid,gid)\n"
   code+="os.chmod(D,0o755);p=D/'config/config.ini';p.write_text("+repr(text)+");p.chmod(0o600);os.chown(p,uid,gid)\n"
   code+="snapshot=pathlib.Path("+repr(str(P/'fixture/bootstrap.snapshot'))+");os.chown(snapshot,uid,gid);assert hashlib.sha256(snapshot.read_bytes()).hexdigest()=="+repr(public['snapshot_sha256'])+"\n"
   code+=filter_script(P,['127.0.0.1',*ips])+'\n'
   args=['systemd-run','--unit='+n['unit'],'--property=User=1100','--property=Group=988','--property=NoNewPrivileges=yes',
    '--property=MemoryMax=56G','--property=MemorySwapMax=0','--property=CPUAffinity=1-7 9-15','--property=RuntimeMaxSec=14400',
    '--property=RemainAfterExit=yes','--property=StandardOutput=append:'+str(D/'node.log'),'--property=StandardError=append:'+str(D/'node.log'),
    str(P/'runtime/ld-linux-x86-64.so.2'),'--library-path',str(P/'runtime/lib'),str(P/'runtime/nodeos'),
    '--config-dir',str(D/'config'),'--data-dir',str(D/'data'),'--snapshot',str(P/'fixture/bootstrap.snapshot'),
    '--chain-threads','6','--force-all-checks','--disable-replay-opts']
   code+="args="+repr(args)+";args=[('--property=Group='+str(gid)) if x.startswith('--property=Group=')else x for x in args];r=subprocess.run(args,capture_output=True,text=True);assert r.returncode==0\n"
   code+="pid=int(subprocess.check_output(['systemctl','show',"+repr(n['unit'])+",'-p','MainPID','--value'],text=True));assert pid>0;os.sched_setaffinity(pid,{0});print(json.dumps({'started':True,'pid':pid,'configuration_sha256':hashlib.sha256((D/'config/config.ini').read_bytes()).hexdigest()}))"
   result=json.loads(ssh(n,code));assert result['configuration_sha256']==cfg_hashes[n['account']]
   forward=['systemd-run','--unit=aic-bp-live-forward-'+n['account']+'-20261030-a3','--property=RuntimeMaxSec=14400',
    '--property=Restart=on-failure','--property=RestartSec=3','--property=StandardOutput=append:'+str(B/(n['account']+'-forward.log')),
    '--property=StandardError=append:'+str(B/(n['account']+'-forward.log')),
    '/usr/bin/ssh','-N',*opts,'-o','ExitOnForwardFailure=yes','-o','ServerAliveInterval=20','-L','127.0.0.1:'+str(n['http_port'])+':127.0.0.1:8888','root@'+n['ip']]
   r=subprocess.run(forward,capture_output=True,text=True);assert r.returncode==0
 pid=int(subprocess.check_output(['systemctl','show',nodes[0]['unit'],'-p','MainPID','--value'],text=True));assert pid>0;os.sched_setaffinity(pid,{0})
 write(B/'CONFIGURATION_HASHES.json',cfg_hashes)
 deadline=time.monotonic()+240;history=[]
 while True:
  observed={};good=True
  for n in nodes:
   try:
    info=rpc(n['http_port'],'get_info')
    assert info['chain_id']==fixture['chain_id']
    connections=rpc(n['http_port'],'connections',namespace='net')
    peers=rpc(n['http_port'],'bp_gossip_peers',namespace='net')
    observed[n['account']]=dict(head=info['head_block_num'],LIB=info['last_irreversible_block_num'],connections=connections,gossip=peers)
    if info['head_block_num']<=fixture['head_block_num']+3:good=False
   except BaseException as e:observed[n['account']]=dict(error=type(e).__name__);good=False
  history.append(dict(at_ns=time.monotonic_ns(),observed=observed));write(B/'ACTIVATION_OBSERVATIONS.json',history)
  direct={}
  for i,j in [(0,2),(2,0)]:
   account=nodes[i]['account'];target=nodes[j]['ip']+':9876';state=observed.get(account,{})
   direct[account+'-to-'+nodes[j]['account']]=any(
    c.get('is_socket_open') and c.get('is_bp_gossip_peer') and
    (c.get('peer','').startswith(target)or c.get('last_handshake',{}).get('p2p_address','').startswith(target))
    for c in state.get('connections',[]))
   learned=any(p['producer_name']==nodes[j]['account'] and p['server_endpoint']==target for p in state.get('gossip',[]))
   good=good and direct[account+'-to-'+nodes[j]['account']] and learned
  if good:
   result=dict(status='signed-BP-gossip-active-and-A-C-learned-without-static-A-C-seeds',
    chain_id=fixture['chain_id'],source_commit=public['source_commit'],nodeos_sha256=public['nodeos_sha256'],
    system_code_hash=fixture['system_code_hash'],configuration_hashes=cfg_hashes,static_seed_edges=public['static_seed_edges'],
    learned_direct_links=direct,all_nodes_parallel=True,workers=8,CPU_multiplier=16,NET_bytes_per_block=16777216,
    observed=observed,pressure_load=False,baseline_promoted=False,full_functional_suite_complete=False)
   write(B/'ACTIVATED.json',result);save(dict(status='signed-gossip-active-on-three-real-parallel-BPs-next-functional-rounds-pending',discovery_active=True))
   break
  assert time.monotonic()<deadline,'Live signed discovery did not converge; retain all logs and machines'
  time.sleep(2)
except BaseException as e:
 save(dict(status='live-activation-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),discovery_active=False));raise
