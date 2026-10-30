import functional_common as f
from pathlib import Path
import os,json,subprocess,hashlib,fcntl,concurrent.futures
B=f.B;P='/opt/aic-bp-discovery-peer-20261030'
opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
def inspect(n):
 root=str(B)if n['primary']else P
 code=r"""from pathlib import Path
import json,os,subprocess,hashlib
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
root=Path(ROOT);D=root/'live'/ACCOUNT
pid=int(subprocess.check_output(['systemctl','show',UNIT,'-p','MainPID','--value'],text=True));assert pid>0
cpu=json.loads(subprocess.check_output(['lscpu','-J'],text=True))
pairs=set()
for p in Path('/sys/devices/system/cpu').glob('cpu[0-9]*'):
 try:pairs.add(((p/'topology/physical_package_id').read_text().strip(),(p/'topology/core_id').read_text().strip()))
 except FileNotFoundError:pass
logical=sum(1 for p in Path('/sys/devices/system/cpu').glob('cpu[0-9]*'))
mem=next(int(line.split()[1])*1024 for line in Path('/proc/meminfo').read_text().splitlines()if line.startswith('MemTotal:'))
text=(D/'config/config.ini').read_text();cfg={}
for line in text.splitlines():
 if '='not in line or line.lstrip().startswith('#'):continue
 k,v=map(str.strip,line.split('=',1))
 if k in ['peer-private-key']:continue
 if k=='signature-provider':v=v.split('=KEY:')[0]
 cfg.setdefault(k,[]).append(v)
safety=[]
for p in (D/'data').rglob('*'):
 if p.is_file()and 'safety'in p.name.lower():safety.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size))
log=(D/'node.log').read_text(errors='replace')
startup=[line for line in log.splitlines()if ('parallel'in line.lower()or'minimum'in line.lower()or'physical'in line.lower())and not 'PVT_'in line][:15]
print(json.dumps(dict(account=ACCOUNT,pid=pid,CPU=lscpu if False else cpu,physical_cores=len(pairs),logical_threads=logical,MemTotal_bytes=mem,configuration=cfg,configuration_sha256=hashlib.sha256(text.encode()).hexdigest(),nodeos_sha256=hashlib.sha256((root/'runtime/nodeos').read_bytes()).hexdigest(),node_argv=(Path('/proc')/str(pid)/'cmdline').read_bytes().replace(b'\0',b' ').decode(),main_affinity=sorted(os.sched_getaffinity(pid)),safety=safety,startup=startup)))
"""
 code=code.replace('ROOT',repr(root)).replace('ACCOUNT',repr(n['account'])).replace('UNIT',repr(n['unit']))
 if n['primary']:r=subprocess.run(['python3','-B','-c',code],capture_output=True,text=True,timeout=60)
 else:r=subprocess.run(['ssh',*opts,'root@'+n['ip'],'python3 -B -'],input=code,capture_output=True,text=True,timeout=60)
 assert r.returncode==0,r.stderr[:1000]
 return json.loads(r.stdout)
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  with concurrent.futures.ThreadPoolExecutor(max_workers=3)as pool:
   results=list(pool.map(inspect,f.N))
  f.write(B/'PREFLIGHT_FUNCTIONAL_CURRENT.json',results)
  D=f.begin('ENV-01','R00')
  for r in results:
   assert r['physical_cores']>=8 and r['logical_threads']>=16 and r['MemTotal_bytes']>=64000000000
   assert '4345P'in json.dumps(r['CPU'])
  f.finish('ENV-01',dict(actual_hardware_all_three=results,shared_hardware_30_processes_not_30_machines=True))
  D=f.begin('ENV-02','R00')
  for r in results:assert r['nodeos_sha256']==f.T['nodeos_sha256']
  code={n['account']:f.rpc(n,'get_code_hash',dict(account_name='eosio'))['code_hash']for n in f.N};assert set(code.values())=={f.F['system_code_hash']}
  f.finish('ENV-02',dict(runtime_sha_all_three=f.T['nodeos_sha256'],source=f.T['source_commit'],system_hash=code,contract_abi_prior_entries='see qualified contract build report; KEY-01 live match',baseline_promoted=False))
  D=f.begin('ENV-03','R00')
  preset=json.loads((B/'input/chain-parameters.json').read_text());records={}
  for n,r in zip(f.N,results):
   cfg=r['configuration'];params=f.rpc(n,'get_consensus_parameters')['chain_config'];assert all(params[k]==v for k,v in preset.items())
   for name,expect in [('experimental-parallel-production','true'),('experimental-parallel-validation-threads','8'),('experimental-parallel-validation-batch-size','24'),('experimental-parallel-early-adoption-group','8'),('incoming-transaction-queue-size-mb','1024'),('validation-mode','full')]:assert cfg.get(name)==[expect],(name,cfg)
   assert '--force-all-checks'in r['node_argv']and'--disable-replay-opts'in r['node_argv']
   records[n['account']]=dict(chain=params,configuration=cfg,configuration_sha256=r['configuration_sha256'],argv=r['node_argv'],startup=r['startup'])
  f.finish('ENV-03',dict(complete_chain_preset_and_live_configuration=records,P2P_payload_bytes=67108864,P2P_limit_scope='compile-time per-message limit; identical hash-qualified node binary'))
  D=f.begin('ENV-05','R00')
  assert len({n['account']for n in f.N})==3
  providers=[]
  for n,r in zip(f.N,results):
   assert r['configuration']['producer-name']==[n['account']]
   assert r['safety'],'No finalizer safety file found'
   providers += r['configuration']['signature-provider']
   assert f.rpc(n,'get_info')['chain_id']==f.T['chain_id']
  assert len(providers)==len(set(providers))
  f.finish('ENV-05',dict(distinct_accounts_and_production_finalizer_identities=True,safety_files={r['account']:r['safety']for r in results},topology=f.T))
  D=f.begin('GSP-01','R02')
  for n,r in zip(f.N,results):
   seeds=r['configuration']['p2p-peer-address'];expected=f.N[1]['ip']+':9876'if n['index']in[0,2]else f.N[0]['ip']+':9876'
   assert seeds==[expected]
  f.finish('GSP-01',dict(only_declared_seed_edges=f.T['static_seed_edges'],actual_activation=str(B/'ACTIVATED.json'),head_and_LIB=f.infos(),all_three_parallel=True))
  md=f.PLAN.read_text().replace('| Node源码 | 0cfbc1a245ea7af027348f42e93d851abce1e851 |','| Node源码 | '+f.T['source_commit']+'（修复冷启动initial重试；旧版失败单列保留） |').replace('| Node SHA256 | fed399d5b57b1b897f4ccabe886ade152967b312cebd7d0524b819272616c129 |','| Node SHA256 | '+f.T['nodeos_sha256']+' |');f.PLAN.write_text(md)
except BaseException as e:f.failed(e);raise
