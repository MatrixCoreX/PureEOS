from pathlib import Path
import json,urllib.request,datetime,time,hashlib,os
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,d):(R/n).write_text(json.dumps(d,indent=2)+'\n')
def api(port,path,p=None):
 q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/'+path,json.dumps(p or{}).encode(),{'Content-Type':'application/json'});raw=urllib.request.urlopen(q,timeout=8).read();return json.loads(raw)if raw.strip()else None
assert not(R/'FOUNDER_RESUME_STARTED.json').exists();assert(R/'BANNED_DELETE_RESULT.json').exists();p=json.loads((R/'BANNED_STOP_PLAN.json').read_text());bans=set(p['banned']);nodes={n:v for n,v in p['all_known_nodes'].items()if n not in bans};assert len(nodes)==19
native=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/libexec/nodeos');assert hashlib.sha256(native.read_bytes()).hexdigest()=='946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
infos={n:api(v['rpc_port'],'chain/get_info')for n,v in nodes.items()};assert len({v['head_block_id']for v in infos.values()})==1 and len({v['last_irreversible_block_id']for v in infos.values()})==1
f=api(8888,'chain/get_finalizer_info');assert f['active_finalizer_policy']['threshold']==15 and len(f['active_finalizer_policy']['finalizers'])==21;assert len({v['description']for v in f['active_finalizer_policy']['finalizers']}-bans)>=15
before=infos['gptgenesisbp'];record={'utc':now(),'before':before,'all19_head_and_LIB_identical':True,'paused_before':api(8888,'producer/paused'),'scope':'One normal producer/resume call on alreadyrunning founder. No restart/config/timeout/consensus/DB/safety changes.'};save('FOUNDER_RESUME_STARTED.json',record)
record['resume_result']=api(8888,'producer/resume');samples=[];start=time.monotonic()
while time.monotonic()-start<70:
 i=api(8888,'chain/get_info');samples.append({'utc':now(),'head':i['head_block_num'],'head_time':i['head_block_time'],'LIB':i['last_irreversible_block_num'],'LIB_time':i['last_irreversible_block_time']})
 if i['last_irreversible_block_num']>before['head_block_num']and i['head_block_time'][:10]=='2026-11-16':break
 time.sleep(3)
record.update(status='PASS_FRESH_HEAD_AND_LIB'if samples[-1]['LIB']>before['head_block_num']else'NEEDS_RECONCILIATION',utc_finished=now(),samples=samples,finalizers=api(8888,'chain/get_finalizer_info'));save('FOUNDER_RESUME_RESULT.json',record);print(json.dumps(record))
