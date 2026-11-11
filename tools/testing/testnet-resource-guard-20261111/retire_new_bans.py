"""New Nov11 authorization: stop four irrevocably banned identities; prepare exact data plan."""
import datetime, hashlib, json, os, stat, subprocess, time, urllib.request
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1')
OLD=Path('/opt/pureeos-testnet-resilience-20261110-r1')
TARGETS=['alibabacloud','deepmindlabs','openaiclouds','sydneyunswai']
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
os.umask(0o077)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(n,x):
    p=R/n;tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(x,ensure_ascii=False,indent=2)+'\n');os.replace(tmp,p)
def rpc(n,d=None):
    for a in range(4):
        try:
            q=urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+n,json.dumps(d or {}).encode(),{'Content-Type':'application/json'})
            with urllib.request.urlopen(q,timeout=20)as f:return json.load(f)
        except Exception:
            if a==3:raise
            time.sleep(1)
def state(u):
    return dict(l.split('=',1)for l in subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,SubState,UnitFileState,NRestarts,Result'],text=True).splitlines()if'='in l)
def memory():return {l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines()if l.startswith(('MemAvailable:','SwapTotal:','SwapFree:'))}
def inventory(root):
    out=[]
    for base,ds,fs in os.walk(root,followlinks=False):
        for n in [*ds,*fs]:
            p=Path(base)/n;t=p.lstat()
            assert not p.is_symlink() and not os.path.ismount(p),str(p)
            out.append({'path':str(p),'size':t.st_size,'allocated':t.st_blocks*512,'dev':t.st_dev,'ino':t.st_ino,'mtime_ns':t.st_mtime_ns,'mode':t.st_mode,'nlink':t.st_nlink})
    return out
R.mkdir(mode=0o700,exist_ok=True)
assert not(R/'STOP_STARTED.json').exists(),'Do not repeat successful stop/delete phases'
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text())
i=rpc('get_info');assert i['chain_id']==CHAIN
fin=rpc('get_finalizer_info');sched=rpc('get_producer_schedule')
members={v['description']for v in fin['active_finalizer_policy']['finalizers']}
for k in ['active','pending','proposed']:
    if sched.get(k):members.update(v['producer_name']for v in sched[k]['producers'])
assert not set(TARGETS)&members,'Never retire an active/pending producer or finalizer'
proof={}
for n in TARGETS:
    v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'rotcands','json':True,'lower_bound':n,'limit':1,'time_limit_ms':1000})['rows'][0]
    assert v['owner']==n and v['banned']and 0<int(v['ban_final_block'])<=i['last_irreversible_block_num']
    proof[n]=v
allnodes={**nodes,**{x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']}}
protected={x['unit']:state(x['unit'])['MainPID']for n,x in allnodes.items()if n not in TARGETS}
protected['pureeos-devnet-history-20261105.service']=state('pureeos-devnet-history-20261105.service')['MainPID']
before={'utc':now(),'info':i,'proof':proof,'finalizers':fin,'schedule':sched,'memory':memory(),'protected_PIDs':protected,'targets':{n:state(nodes[n]['unit'])for n in TARGETS}}
save('STOP_STARTED.json',before)
for n in TARGETS:
    u=nodes[n]['unit']
    subprocess.run(['systemctl','disable',u],check=True,capture_output=True)
    subprocess.run(['systemctl','stop',u],check=True,capture_output=True,timeout=180)
    s=state(u);assert s['MainPID']=='0' and s['ActiveState'] in ['inactive','failed'] and s['UnitFileState']=='disabled'
    save('STOP_PROGRESS.json',{'utc':now(),'last':n,'state':s})
assert all(state(u)['MainPID']==p for u,p in protected.items())
plans=[]
for n in TARGETS:
    root=Path(nodes[n]['data_dir']);roots=[root]
    archive=root.with_name(root.name+'.pre-resilience-20261110')
    if archive.exists():roots.append(archive)
    for data in roots:
        assert data.resolve(strict=True)==data and data.is_dir()and not os.path.ismount(data)
        entries=inventory(data)
        safeties=[x for x in entries if '/finalizers/'in x['path'] and stat.S_ISREG(x['mode'])]
        for x in safeties:
            assert x['size']<1024**2
            x['sha256']=hashlib.sha256(Path(x['path']).read_bytes()).hexdigest()
        removable=[x for x in entries if '/finalizers/'not in x['path']and Path(x['path']).name!='finalizers']
        assert all(not Path(x['path']).name.endswith(('.key','.pem','.wallet','.keystore','.p12','.pfx'))for x in removable)
        plans.append({'owner':n,'unit':nodes[n]['unit'],'root':str(data),'entries':entries,'retained_safety':safeties,'remove_allocated_bytes':sum(x['allocated']for x in removable if stat.S_ISREG(x['mode']))})
    log=root.parent/'nodeos.log'
    if log.exists():
        size=log.stat().st_size
        with log.open('rb')as f:f.seek(max(0,size-256*1024));tail=f.read()
        (R/(n+'-log-tail.txt')).write_bytes(tail)
        plans.append({'owner':n,'unit':nodes[n]['unit'],'log':str(log),'bytes':size,'allocated':log.stat().st_blocks*512,'action':'gzip-and-byte-verify-preserve-evidence'if size else'empty-log-preserve'})
result={'status':'STOPPED_AND_EXACT_CLEANUP_PLANNED','utc':now(),'stopped':{n:state(nodes[n]['unit'])for n in TARGETS},'memory_after_stop':memory(),'protected_PIDs_unchanged':True,'plans':plans,'data_allocated_bytes':sum(p.get('remove_allocated_bytes',0)for p in plans),'info_after':rpc('get_info')}
save('CLEANUP_PLAN.json',result)
print(json.dumps(result,ensure_ascii=False))
