#!/usr/bin/env python3
"""Remote TEST reset phase runner: pinned plan, durable receipts, safe resume."""
import argparse,datetime,fcntl,hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def save(path,value):
    tmp=path.with_suffix('.tmp');tmp.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n');tmp.replace(path)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('command',choices=['prepare','status','run','resume'])
    p.add_argument('--plan',type=Path,required=True);p.add_argument('--run-dir',type=Path,required=True);a=p.parse_args()
    plan=json.loads(a.plan.read_text());plan_hash=digest(a.plan);root=a.run_dir.resolve()
    if not plan.get('test_only')or plan.get('TPS')is not False:raise ValueError('Explicit TEST-only, no-TPS profile required')
    if not root.is_absolute()or not str(root).startswith('/opt/pureeos-'):raise ValueError('Remote owned /opt/pureeos-* run directory required')
    if hashlib.sha256(Path('/etc/machine-id').read_bytes().strip()).hexdigest()!=plan['host_identity_sha256']:raise ValueError('Host identity does not match remote preflight')
    if plan['scope']=='public-testnet' and (not plan.get('reset_authorization')or not re.fullmatch('[0-9a-f]{64}',plan.get('old_chain_id',''))):raise ValueError('Public reset needs selected authorization and exact old ChainID')
    phases=plan['phases'];names=[s['name']for s in phases]
    if len(names)!=len(set(names))or not phases:raise ValueError('Unique ordered phase names required')
    for phase in phases:
        if any(x not in names[:names.index(phase['name'])]for x in phase.get('after',[])):raise ValueError('Phase dependency is not earlier in plan')
        script=Path(phase['script']).resolve()
        if not script.is_relative_to(root)or digest(script)!=phase['script_sha256']:raise ValueError('Prepared phase source hash mismatch')
    root.mkdir(mode=0o700,parents=True,exist_ok=True);(root/'receipts').mkdir(exist_ok=True);(root/'private-controller').mkdir(mode=0o700,exist_ok=True)
    state_path=root/'RESET_STATE.json'
    if a.command=='status':
        print(state_path.read_text()if state_path.exists()else json.dumps({'status':'NOT_PREPARED','plan_sha256':plan_hash}));return
    with (root/'reset.lock').open('w')as lock:
        fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        if state_path.exists():
            state=json.loads(state_path.read_text())
            if state['plan_sha256']!=plan_hash:raise ValueError('Existing run plan is immutable')
        else:
            state={'status':'PREPARED','UTC':now(),'plan_sha256':plan_hash,'scope':plan['scope'],'phases':{n:{'status':'PENDING'}for n in names}};save(state_path,state)
        if a.command=='prepare':print(json.dumps({'status':'PREPARED','plan_sha256':plan_hash}));return
        for phase in phases:
            item=state['phases'][phase['name']];receipt=root/'receipts'/(phase['name']+'.json')
            if any(state['phases'][d]['status']!='PASS'for d in phase.get('after',[])):raise ValueError('Dependency has not passed')
            if item['status']=='PASS':
                if digest(receipt)!=item['receipt_sha256']:raise ValueError('Completed receipt changed')
                continue
            uncertain=item['status']in['RUNNING','FAIL','NEEDS_RECONCILIATION']
            if uncertain and a.command!='resume':raise ValueError('Interrupted phase requires canonical reconciliation through resume')
            action='reconcile'if uncertain else'run'
            item.update(status='RUNNING',started_UTC=now(),attempt=item.get('attempt',0)+1,operation=action);state['status']='RUNNING';save(state_path,state)
            argv=[sys.executable,phase['script'],'--phase',phase['name'],'--operation',action,'--plan',str(a.plan.resolve()),'--receipt',str(receipt)]
            if any(re.search(r'PVT_|\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',s)for s in argv):raise ValueError('Signing secrets cannot appear in controller arguments')
            log=root/'private-controller'/(phase['name']+'-'+str(item['attempt'])+'.log')
            with log.open('w')as stream:
                try:result=subprocess.run(argv,stdout=stream,stderr=subprocess.STDOUT,timeout=phase['timeout_seconds'])
                except BaseException:
                    item['status']='NEEDS_RECONCILIATION';state['status']='STOPPED';save(state_path,state);raise
            if result.returncode or not receipt.exists():
                item['status']='NEEDS_RECONCILIATION';state['status']='STOPPED';save(state_path,state)
                raise RuntimeError('Phase failed; retained private diagnostics and canonical reconciliation required: '+phase['name'])
            evidence=json.loads(receipt.read_text())
            if evidence.get('status')!='PASS'or evidence.get('plan_sha256')!=plan_hash or evidence.get('phase')!=phase['name']:
                item['status']='NEEDS_RECONCILIATION';state['status']='STOPPED';save(state_path,state);raise ValueError('Phase receipt does not prove this exact plan')
            if not evidence.get('checks')or not all(c.get('pass')is True for c in evidence['checks']):raise ValueError('Phase receipt lacks passing checks')
            if phase.get('starts_new_chain'):
                chain=evidence.get('chain_id','')
                if not re.fullmatch('[0-9a-f]{64}',chain)or chain==plan.get('old_chain_id'):raise ValueError('Fresh native ChainID required')
                state['new_chain_id']=chain
            if phase.get('requires_new_chain')and evidence.get('chain_id')!=state.get('new_chain_id'):raise ValueError('Receipt belongs to a different chain')
            item.update(status='PASS',finished_UTC=now(),receipt_sha256=digest(receipt));save(state_path,state)
            print(json.dumps({'phase':phase['name'],'status':'PASS','UTC':now()}),flush=True)
        state.update(status='COMPLETE',finished_UTC=now());save(state_path,state);print(json.dumps({'status':'COMPLETE','run_id':plan['run_id']}))
if __name__=='__main__':main()
