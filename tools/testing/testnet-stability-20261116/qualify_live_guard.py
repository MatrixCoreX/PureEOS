from pathlib import Path
import json,sys,time,fcntl
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');sys.path.insert(0,str(R));import stability_guard as g
assert not(R/'GUARD_MANUAL_QUALIFICATION_STARTED.json').exists();assert g.load('STABILITY_INSTALL_RESULT.json')['status'].startswith('PASS_');nodes=g.load('GUARD_NODE_MAP.json');lock=(R/'native-operation.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
end=time.monotonic()+150;name=None
while time.monotonic()<end:
 ctx=g.status_context(nodes);s,i,pids,ro,mem=ctx
 if s['head_age']<5 and s['lib_age']<20 and s['policy_count']==21 and s['threshold']==15 and len(s['recent'])>=20 and s['all_running']:
  eligible=[n for n,v in s['nodes'].items()if n!='gptgenesisbp'and not v['banned']and v['safety_exists']and v['strikes']==0 and v['distance']>=110 and len(set(s['recent'])-{n})>=19]
  if eligible:name=max(eligible,key=lambda n:(s['nodes'][n]['standby'],s['nodes'][n]['distance']));break
 time.sleep(2)
assert name,'No qualified safe canary; no native stop performed'
g.save('GUARD_MANUAL_QUALIFICATION_STARTED.json',{'utc':g.now(),'target':name,'scope':'One explicitly manualcanary using exact productionperform, allactualbinding/chain/quorum/duty/safetyguards. Normalautomaticmemorythresholds unchanged, no artificialpressure.'});result=g.perform(name,nodes,ctx);g.save('GUARD_MANUAL_QUALIFICATION_RESULT.json',result);assert result['status']=='PASS_ONE_GUARDED_RECYCLE',result;print(json.dumps({'status':result['status'],'target':name,'proof':result['proof'],'before_bytes':result['before_bytes']}))
