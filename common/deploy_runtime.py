import os,pathlib,json,subprocess,shlex,hashlib,time,traceback
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
B=pathlib.Path('/opt/aic-bp-discovery-live-20261030');G=pathlib.Path('/opt/aic-bp-discovery-parallel-qualification-20261030')
def write(name,d):
 p=B/name;q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def sha(p):
 with p.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()
try:
 d=json.loads((G/'NODE_BUILD_REPORT.json').read_text());node=pathlib.Path(d['nodeos']);assert sha(node)==d['nodeos_sha256']
 T=json.loads((B/'TOPOLOGY.json').read_text())
 assert {p['id']for p in T['peers']}=={'db8ee7ed-2357-436c-8614-d0f485f52ed6','e93d8e9e-43df-4a7b-b203-38e94b612f2e'}
 write('DEPLOY_STATUS.json',dict(status='installing-candidate-runtime-on-existing-three-test-machines',discovery_active=False))
 os.link(node,B/'runtime/nodeos')
 deployed=[dict(role='producer',sha256=sha(B/'runtime/nodeos'))]
 opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
 for peer in T['peers']:
  target='root@'+peer['main_ip']
  setup="import pathlib,os,shutil,subprocess,json;assert subprocess.run(['pgrep','-x','nodeos'],capture_output=True).returncode==1;p=pathlib.Path('/opt/aic-bp-discovery-peer-20261030');p.mkdir();[(p/x).mkdir()for x in ['runtime','fixture','input','cases']];old=pathlib.Path('/opt/aic-all-parallel-peer-20261030/runtime');[(shutil.copytree(x,p/'runtime'/x.name,copy_function=os.link)if x.is_dir()else os.link(x,p/'runtime'/x.name))for x in old.iterdir()if x.name!='nodeos'];print(json.dumps({'ready':True}))"
  p=subprocess.run(['ssh',*opts,target,shlex.join(['python3','-B','-c',setup])],capture_output=True,text=True,timeout=180);assert p.returncode==0,p.stderr[-1500:]
  p=subprocess.run(['scp','-q',*opts,str(node),target+':/opt/aic-bp-discovery-peer-20261030/runtime/nodeos'],capture_output=True,text=True,timeout=180);assert p.returncode==0,p.stderr[-1500:]
  verify="import pathlib,hashlib,json;p=pathlib.Path('/opt/aic-bp-discovery-peer-20261030/runtime/nodeos');p.chmod(0o755);h=hashlib.sha256(p.read_bytes()).hexdigest();assert h=="+repr(d['nodeos_sha256'])+";print(json.dumps({'role':"+repr(peer['role'])+",'sha256':h}))"
  p=subprocess.run(['ssh',*opts,target,shlex.join(['python3','-B','-c',verify])],capture_output=True,text=True,timeout=180);assert p.returncode==0,p.stderr[-1500:]
  deployed.append(json.loads(p.stdout))
 write('RUNTIME_DEPLOYED.json',dict(status='candidate-runtime-installed-and-hash-verified-on-three-existing-machines',source_commit=d['source_commit'],nodeos_sha256=d['nodeos_sha256'],machines=deployed,new_servers_created=False,chain_deployment_pending=True,discovery_active=False))
 write('DEPLOY_STATUS.json',dict(status='three-runtime-installs-complete-preparing-signed-system-contract-and-peer-key-chain-state',discovery_active=False))
except BaseException as e:write('DEPLOY_STATUS.json',dict(status='deployment-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),discovery_active=False));raise
