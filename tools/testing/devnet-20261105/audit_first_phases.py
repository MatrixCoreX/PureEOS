from pathlib import Path
import datetime,hashlib,json,subprocess,time,urllib.request
from campaign_helpers import R,V,load_chain
f=load_chain();plan=json.loads((R/'PLAN.json').read_text());names={e['name']for e in plan['BPs']}
assert len(f.table('producersv2'))==24
assert len(f.table('bpbonds'))==24 and all(int(x['balance'])==100000000 for x in f.table('bpbonds'))
assert f.governance()['sealed']and f.governance()['developer']=='eosio.dev'
deadline=time.monotonic()+240
while True:
 fin=f.rpc('get_finalizer_info');members={x['description']for x in fin['active_finalizer_policy']['finalizers']}
 if len(members)>=2 and members<=names and not fin.get('pending_finalizer_policy'):break
 assert time.monotonic()<deadline,'Wait for genuine multi-BP finalizer handover';time.sleep(1)
start=f.rpc('get_info');f.wait_lib(start['head_block_num']+10,timeout=120)
proof=[]
for e in plan['BPs']:
 q=urllib.request.Request('http://127.0.0.1:'+str(e['rpc_port'])+'/v1/chain/get_info',b'{}',{'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=15)as r:info=json.load(r)
 assert info['chain_id']==start['chain_id']and info['last_irreversible_block_num']>=start['last_irreversible_block_num']-20
 unit='pureeos-devnet-founder-20261105.service'if e['index']==0 else'pureeos-devnet-bp-'+e['name']+'-20261105.service'
 pid=int(subprocess.check_output(['systemctl','show','-p','MainPID','--value',unit],text=True));assert pid>0
 cmd=Path('/proc',str(pid),'cmdline').read_bytes().split(b'\0');elf=V/'libexec/nodeos'
 assert str(elf).encode()in cmd and str(elf)in Path('/proc',str(pid),'maps').read_text()
 assert hashlib.sha256(elf.read_bytes()).hexdigest()==plan['nodeos_SHA256']
 proof.append({'name':e['name'],'unit':unit,'PID':pid,'info':info})
# Only after real other native producers/finalizers are online, remove sole-founder bootstrap flag.
cfg=R/'founder/config/config.ini';text=cfg.read_text();assert text.count('enable-stale-production = true')==1
(R/'archive/founder-config-before-stale-removal.ini').write_bytes(cfg.read_bytes())
cfg.write_text(text.replace('enable-stale-production = true','enable-stale-production = false'));cfg.chmod(0o600)
subprocess.run(['systemctl','restart','pureeos-devnet-founder-20261105.service'],check=True)
deadline=time.monotonic()+60
while True:
 try:after=f.rpc('get_info');break
 except Exception:assert time.monotonic()<deadline;time.sleep(.3)
f.wait_lib(after['head_block_num']+10,timeout=120)
result={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':start['chain_id'],'BPs':proof,'finalizers':f.rpc('get_finalizer_info'),'info':f.rpc('get_info'),'all_self_bonded':True,'bootstrap_stale_flag_restored_false':True,'scope':'Functional enrollment and real finality, no TPS/capacity qualification'}
(R/'FIRST_PHASES_AUDIT.json').write_text(json.dumps(result,indent=2)+'\n');print('ACTUAL24_BP_FUNCTIONAL_FINALITY_PASS',flush=True)
