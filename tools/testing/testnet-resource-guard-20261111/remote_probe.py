"""Read-only public-chain and OS inventory; deliberately no signer or service changes."""
import datetime, json, os, subprocess, sys, time, urllib.request, urllib.error
from pathlib import Path

CHAIN = '09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
OLD = Path('/opt/pureeos-testnet-resilience-20261110-r1')
NEW_BANS = ['alibabacloud', 'deepmindlabs', 'openaiclouds', 'sydneyunswai']
def rpc(name, data=None, port=8890):
    assert name.startswith('get_')
    q=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/'+name,json.dumps(data or {}).encode(),{'Content-Type':'application/json'})
    try:
        with urllib.request.urlopen(q,timeout=30) as f:return json.load(f)
    except urllib.error.HTTPError as e:
        raise RuntimeError(name+' '+str(data)+' '+e.read().decode(errors='replace')[:2200]) from e
def table(name):
    rows=[];lower='';deadline=time.monotonic()+75
    while time.monotonic()<deadline:
        v=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':name,'json':True,'limit':5,'time_limit_ms':1000,'lower_bound':lower})
        if not v.get('more'):return rows+v['rows']
        nxt=str(v.get('next_key',''))
        if not nxt or nxt==lower:time.sleep(.15);continue
        rows+=v['rows'];lower=nxt
    raise TimeoutError('Incomplete table '+name)
def run(args,timeout=20):
    q=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
    return {'returncode':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
def state(unit):
    return dict(l.split('=',1) for l in run(['systemctl','show',unit,'-p','MainPID,ActiveState,SubState,UnitFileState,NRestarts,Result,ExecMainStartTimestamp,ExecMainExitTimestamp,ControlGroup,CPUWeight,IOWeight,MemoryHigh,MemoryMax,TasksCurrent,FragmentPath'])['stdout'].splitlines() if '=' in l)
def mem():return {l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith(('MemAvailable:','SwapTotal:','SwapFree:'))}
def layout(root):
    if not root.exists():return None
    return [{'path':str(p),'is_dir':p.is_dir(),'is_symlink':p.is_symlink(),'size':p.lstat().st_size,'allocated':p.lstat().st_blocks*512} for p in root.iterdir()]

d={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'read_only':True,'info':rpc('get_info')}
assert d['info']['chain_id']==CHAIN
for n in ['rotcands','bpmaint','bpbonds','bpbondcfg','producersv2','votersv2']:
    print('Reading '+n,file=sys.stderr,flush=True);d[n]=table(n)
d['finalizers']=rpc('get_finalizer_info');d['schedule']=rpc('get_producer_schedule')
nodes=json.loads((OLD/'RESTORE_NODE_PATHS.json').read_text())
nodes.update({x['name']:x for x in json.loads((OLD/'REPLACEMENTS_PREPARED.json').read_text())['nodes']})
nodes['history']={'unit':'pureeos-devnet-history-20261105.service','rpc_port':8890,'data_dir':'/opt/pureeos-devnet-20261105-r1/history/data'}
d['nodes']={}
for n,x in nodes.items():
    s=state(x['unit']);pid=s.get('MainPID','0');v={'unit':x['unit'],'state':s,'rpc_port':x['rpc_port']}
    if pid!='0':
        v['memory']={l.split(':')[0]:int(l.split()[1])*1024 for l in (Path('/proc')/pid/'status').read_text().splitlines() if l.startswith(('VmRSS:','VmSwap:','RssAnon:','RssFile:'))}
    if n in NEW_BANS:
        v['path_spec']=x;root=Path(x['data_dir']);v['data_layout']=layout(root);v['node_parent_layout']=layout(root.parent)
        v['service_exit_journal']=run(['journalctl','-u',x['unit'],'--since','2026-11-10 02:13:00 UTC','-n','50','--no-pager','-o','short-iso'])
    d['nodes'][n]=v
d['memory']=mem();s=os.statvfs('/');d['disk_available_bytes']=s.f_bavail*s.f_frsize
d['pressure']={n:(Path('/proc/pressure')/n).read_text()for n in ['cpu','memory','io']}
d['vmstat']=run(['vmstat','1','3'])
d['health_latest']=json.loads((OLD/'HEALTH_LATEST.json').read_text())
d['kernel_oom']=run(['journalctl','-k','--since','2026-11-10 02:13:00 UTC','--no-pager','-o','short-iso','--grep','Out of memory|Killed process|oom-kill|invoked oom'])
d['log_inventory']=run(['sh','-c','ls -lh /var/log/syslog* /var/log/kern.log* 2>/dev/null'])
d['sysctl']=run(['sysctl','vm.swappiness','vm.page-cluster','vm.dirty_background_ratio','vm.dirty_ratio'])
d['end_info']=rpc('get_info')
print(json.dumps(d,ensure_ascii=False))
