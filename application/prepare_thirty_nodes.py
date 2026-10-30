from pathlib import Path
import os,json,subprocess,hashlib,time,traceback,pwd,shutil
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
B=Path('/opt/aic-bp-discovery-live-20261030');C=Path('/opt/aic-bp-discovery-30nodes-20261030');P=Path('/opt/aic-bp-discovery-30peer-20261030');J=Path('/opt/aic-bp-gossip-roster-resync-20261030')
def read(p):return json.loads(p.read_text())
def write(p,d):
 p.parent.mkdir(parents=True,exist_ok=True);q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def sha(p):
 with p.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
def save(d):d['UTC']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());write(C/'PREPARATION_STATUS.json',d)
opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
def ssh(ip,code):
 r=subprocess.run(['ssh',*opts,'root@'+ip,'python3 -B -'],input=code,capture_output=True,text=True,timeout=180)
 if r.returncode:
  p=C/('private-preparation-error-'+str(time.time_ns())+'.log');p.write_text(r.stdout+r.stderr);p.chmod(0o600)
  raise RuntimeError('Owned peer preparation failed; protected log: '+str(p))
 return json.loads(r.stdout)if r.stdout.strip()else None
def operate(n,code):
 if n['primary']:
  r=subprocess.run(['python3','-B','-c',code],capture_output=True,text=True,timeout=180);assert r.returncode==0,r.stderr[-1000:]
  return json.loads(r.stdout)if r.stdout.strip()else None
 return ssh(n['ip'],code)
try:
 assert read(C/'NATIVE_PREPARATION_STATUS.json')['status']=='native-contract-fixture-prepared-awaiting-three-live-parallel-node-activation'
 public=read(C/'fixture/PUBLIC_FIXTURE.json');private=read(C/'fixture/PRIVATE_NODES.json');build=read(J/'BUILD_REPORT.json')
 assert len(public['nodes'])==len(private)==30 and len(public['active_producer_accounts'])==21
 assert len({n['account']for n in private})==30
 assert all(len({n[field]for n in public['nodes']})==30 for field in ['block_key','finalizer_key','peer_key','next_peer_key'])
 assert sha(Path(build['nodeos']))==build['nodeos_sha256']
 snapshot_sha=sha(C/'fixture/bootstrap.snapshot')
 machines=read(B/'LIVE_TOPOLOGY.json')['nodes'];ips=[n['ip']for n in machines]
 save(dict(status='preparing-thirty-independent-identities-configs-users-and-ports; no-nodes-started',pressure_load=False))
 (C/'runtime').mkdir(exist_ok=False);os.link(Path(build['nodeos']),C/'runtime/nodeos')
 # Native test output contains public chain state and separate private test signers.
 (C/'fixture').chmod(0o755)
 for p in (C/'fixture').iterdir():
  if 'PRIVATE'in p.name:p.chmod(0o600);os.chown(p,0,0)
  else:p.chmod(0o644)
 for machine in machines[1:]:
  setup="from pathlib import Path\nimport os,shutil\nroot=Path("+repr(str(P))+");root.mkdir(exist_ok=False)\n"
  setup+="for name in ['runtime','fixture','live','input']:(root/name).mkdir()\n"
  setup+="old=Path('/opt/aic-bp-discovery-peer-20261030/runtime')\nfor p in old.iterdir():\n if p.name not in ['nodeos','lib','ld-linux-x86-64.so.2']:continue\n if p.is_dir():shutil.copytree(p,root/'runtime'/p.name,copy_function=os.link)\n else:os.link(p,root/'runtime'/p.name)\n"
  ssh(machine['ip'],setup)
  for filename in ['bootstrap.snapshot','PUBLIC_FIXTURE.json','genesis.json']:
   r=subprocess.run(['scp','-q',*opts,str(C/'fixture'/filename),'root@'+machine['ip']+':'+str(P/'fixture'/filename)],capture_output=True,text=True,timeout=180);assert r.returncode==0
  verify="from pathlib import Path\nimport hashlib,json\nroot=Path("+repr(str(P))+");assert hashlib.sha256((root/'runtime/nodeos').read_bytes()).hexdigest()=="+repr(build['nodeos_sha256'])+";p=root/'fixture/bootstrap.snapshot';p.chmod(0o644);assert hashlib.sha256(p.read_bytes()).hexdigest()=="+repr(snapshot_sha)+"\nprint(json.dumps(dict(runtime_verified=True,snapshot_verified=True)))"
  assert ssh(machine['ip'],verify)['runtime_verified']
 nodes=[];third=read(B/'third-party-build/STATUS.json');codehashes={public['system_code_hash'],public['token_code_hash'],public['tokenone_code_hash']}|{c['wasm_sha256']for c in third['contracts']if c['status']=='built'}
 for i,secret in enumerate(private):
  host=i%3;ordinal=i//3
  n=dict(index=i,local_index=ordinal,account=secret['account'],ip=ips[host],primary=host==0,host_index=host,http_server_port=9000+ordinal,http_port=9000+ordinal if host==0 else 39000+i,p2p_port=11000+ordinal,uid=1200+i,user='aicbp30n'+str(i).zfill(2),unit='aic-bp30-'+secret['account']+'-20261030')
  root=C if n['primary']else P;D=root/'live'/n['account'];seed=1 if i in [0,2]else 0;seed_host=seed%3;seed_port=11000+seed//3
  cfg=['plugin = eosio::chain_api_plugin','plugin = eosio::producer_api_plugin','plugin = eosio::net_plugin','plugin = eosio::net_api_plugin','chain-state-db-size-mb = 1024','chain-state-db-guard-size-mb = 4','resource-monitor-space-absolute-gb = 1','http-server-address = 127.0.0.1:'+str(n['http_server_port']),'http-validate-host = true','unix-socket-path =','p2p-listen-endpoint = 0.0.0.0:'+str(n['p2p_port']),'p2p-server-address = '+n['ip']+':'+str(n['p2p_port']),'p2p-peer-address = '+ips[seed_host]+':'+str(seed_port),'p2p-max-nodes-per-host = 16','p2p-accept-transactions = true','allowed-connection = any','p2p-bp-gossip-endpoint = '+n['account']+','+n['ip']+':'+str(n['p2p_port'])+','+n['ip'],'wasm-runtime = eos-vm-jit','validation-mode = full','read-mode = head','performance-metrics = false','max-transaction-time = 100','incoming-transaction-queue-size-mb = 1024','experimental-parallel-validation-threads = 8','experimental-parallel-validation-batch-size = 24','experimental-parallel-engine = jit','experimental-parallel-production = true','experimental-parallel-early-adoption = true','experimental-parallel-early-adoption-group = 8','experimental-parallel-native-capture = true','experimental-parallel-native-capture-staged = false','experimental-parallel-input-fingerprints = true','experimental-parallel-trace-reserve = true','producer-name = '+n['account'],'enable-stale-production = true','signature-provider = '+secret['block_provider'],'signature-provider = '+secret['finalizer_provider'],'peer-private-key = '+json.dumps([secret['peer_public'],secret['peer_private']],separators=(',',':')),'peer-private-key = '+json.dumps([secret['next_peer_public'],secret['next_peer_private']],separators=(',',':'))]
  cfg +=['experimental-parallel-code-hash = '+h for h in sorted(codehashes)]
  text='\n'.join(cfg)+'\n';n['configuration_sha256']=hashlib.sha256(text.encode()).hexdigest();n['seed_endpoint']=ips[seed_host]+':'+str(seed_port)
  code="from pathlib import Path\nimport os,pwd,subprocess,json\nD=Path("+repr(str(D))+");uid="+str(n['uid'])+";user="+repr(n['user'])+"\n"
  code+="try:p=pwd.getpwnam(user);assert p.pw_uid==uid\nexcept KeyError:\n try:pwd.getpwuid(uid)\n except KeyError:subprocess.run(['useradd','--system','--no-create-home','--shell','/usr/sbin/nologin','--uid',str(uid),user],check=True)\n else:raise RuntimeError('Requested test UID already belongs to another user')\np=pwd.getpwnam(user);gid=p.pw_gid\n"
  code+="D.mkdir(parents=True,exist_ok=False);D.chmod(0o755)\nfor name in ['config','data','tmp']:\n q=D/name;q.mkdir();os.chown(q,uid,gid);q.chmod(0o700)\nf=D/'config/config.ini';f.write_text("+repr(text)+");f.chmod(0o600);os.chown(f,uid,gid)\nprint(json.dumps(dict(gid=gid,user=user,uid=uid)))"
  installed=operate(n,code);n['gid']=installed['gid'];nodes.append(n);write(C/'PREPARED_NODES.partial.json',nodes)
 # Network filter affects only the ten owned P2P listen ports, never SSH/RPC.
 for machine in machines:
  root=C if machine['primary']else P
  rules='\n'.join(['table inet aic_bp30_live_20261030 {',' set peers { type ipv4_addr; elements = { '+', '.join(['127.0.0.1',*ips])+' }; }',' chain ingress { type filter hook input priority -6; policy accept; tcp dport 11000-11009 ip saddr != @peers reject with tcp reset; }','}',''])
  code="from pathlib import Path\nimport subprocess,json\nroot=Path("+repr(str(root))+");assert subprocess.run(['nft','list','table','inet','aic_bp30_live_20261030'],capture_output=True).returncode!=0\nr=subprocess.run(['nft','-f','-'],input="+repr(rules)+",capture_output=True,text=True);assert r.returncode==0,r.stderr;(root/'NETWORK_FILTER.nft').write_text("+repr(rules)+");print(json.dumps(dict(filter_installed=True)))"
  operate(machine,code)
 topology=dict(chain_id=public['chain_id'],source_commit=build['source_commit'],nodeos_sha256=build['nodeos_sha256'],snapshot_sha256=snapshot_sha,nodes=nodes,machines=[dict(ip=n['ip'],primary=n['primary'],host_index=n['index'])for n in machines],active_producer_accounts=public['active_producer_accounts'],active_finalizer_policy=public['active_finalizer_policy'],CPU_multiplier=16,NET_bytes_per_block=16777216,workers_per_node=8,instances_per_host=10,per_node_chain_state_MiB=1024,per_node_MemoryMax_GiB=5,placement_scope='functional/topology simulation on 3 shared 8-core/16-thread machines; no 30-machine performance claim',pressure_load=False,started=False)
 write(C/'LIVE_TOPOLOGY.json',topology);write(C/'PREPARED.json',dict(status='thirty-configurations-identities-unique-users-and-public-port-guards-prepared; no-node-started',topology=topology,source_commit=build['source_commit'],nodeos_sha256=build['nodeos_sha256'],pressure_load=False,baseline_promoted=False))
 save(dict(status='thirty-independent-parallel-node-configurations-prepared-awaiting-three-node-roster-fix-qualification-and-controlled-switch',nodes_started=0,pressure_load=False))
except BaseException as e:save(dict(status='thirty-node-preparation-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),pressure_load=False));raise
