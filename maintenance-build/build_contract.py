from pathlib import Path
import subprocess,json,hashlib,time
B=Path('/campaign');S=B/'source';O=B/'contract-build';E=B/'evidence'/('contract-'+str(time.time_ns()));E.mkdir()
SDK=Path('/work/sdk/aic-cdt-1.0.0');jobs=[]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(d):p=B/'evidence/CONTRACT_BUILD_STATUS.json';q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def run(label,args):
 with(E/(label+'.log')).open('x')as log:r=subprocess.run(list(map(str,args)),stdout=log,stderr=subprocess.STDOUT,timeout=1800)
 jobs.append(dict(label=label,returncode=r.returncode));(E/'JOBS.json').write_text(json.dumps(jobs,indent=2)+'\n');assert r.returncode==0,(label,r.returncode)
try:
 save(dict(status='building-owner-authorized-maintenance-system-extension',evidence=str(E)))
 flags=['PUREEOS_BP_MAINTENANCE','PUREEOS_BP_PEERING','PUREEOS_UPGRADE_GOVERNANCE','PUREEOS_SINGLE_CHOICE_VOTING','PUREEOS_RESOURCE_SPLIT','PUREEOS_SEQUENTIAL_ROTATION','SYSTEM_CONFIGURABLE_WASM_LIMITS','SYSTEM_BLOCKCHAIN_PARAMETERS','SYSTEM_ENABLE_CDT_VERSION_CHECK']
 run('configure',['/usr/bin/cmake','-S',S/'contracts','-B',O,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_TOOLCHAIN_FILE='+str(SDK/'lib/cmake/cdt/CDTWasmToolchain.cmake'),'-DCMAKE_PREFIX_PATH='+str(SDK),'-DCDT_ROOT='+str(SDK),*['-D'+f+'=ON'for f in flags]])
 run('build',['/usr/bin/cmake','--build',O,'--target','eosio.system','-j4'])
 old=Path('/opt/aic-bp-discovery-live-20261030/contract');prior=json.loads((old/'eosio.system.abi').read_text());abi=json.loads((O/'eosio.system/eosio.system.abi').read_text())
 comparison={}
 for section in ['types','structs','actions','tables','variants','action_results','ricardian_clauses','error_messages']:
  def index(items):
   out={}
   for item in items:
    key=next(str(item[k])for k in ['name','new_type_name','id','error_code']if k in item);assert key not in out;out[key]=item
   return out
  before=index(prior.get(section,[]));after=index(abi.get(section,[]))
  assert all(k in after and after[k]==v for k,v in before.items()),section
  comparison[section]=dict(prior_entries_preserved=True,added=sorted(set(after)-set(before)))
 assert comparison['actions']['added']==['setmaint'];assert comparison['tables']['added']==['bpmaint']
 structs={x['name']:x for x in abi['structs']};action=next(x for x in abi['actions']if x['name']=='setmaint')
 assert structs[action['type']]['fields']==[dict(name='producer',type='name'),dict(name='maintenance',type='bool')]
 head=subprocess.check_output(['git','-c','safe.directory=*','-C',str(S),'rev-parse','HEAD'],text=True).strip()
 assert not subprocess.check_output(['git','-c','safe.directory=*','-C',str(S),'status','--porcelain'],text=True)
 report=dict(status='maintenance-system-extension-built-existing-ABI-preserved-native-and-live-qualification-pending',source_commit=head,system_wasm_sha256=sha(O/'eosio.system/eosio.system.wasm'),system_abi_sha256=sha(O/'eosio.system/eosio.system.abi'),parent_system_wasm_sha256=sha(old/'eosio.system.wasm'),enabled_flags=flags,ABI_comparison=comparison,evidence=str(E),jobs=jobs,baseline_promoted=False,TPS='unmeasured')
 (B/'evidence/CONTRACT_BUILD_REPORT.json').write_text(json.dumps(report,indent=2)+'\n');save(report)
except BaseException as e:save(dict(status='maintenance-contract-build-stopped-on-preserved-error',error=str(e),evidence=str(E),jobs=jobs));raise
