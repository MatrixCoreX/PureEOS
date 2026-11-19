from pathlib import Path
import sys,os,json,hashlib,datetime,time,fcntl,pwd
G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');sys.path.insert(0,str(G));import stability_guard as g
assert not(R/'VOTE_ONE_CANARY_STARTED.json').exists();lock=(G/'native-operation.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);nodes=g.load('GUARD_NODE_MAP.json')
end=time.monotonic()+180
while True:
 s,i,pids,rows,m=g.status_context(nodes)
 assert s['head_age']<5 and s['lib_age']<20 and len(s['recent'])>=19 and s['policy_count']==21 and s['threshold']==15 and s['all_running']and s['disk_free']>25*1024**3 and s['mem_available']>512*1024**2
 candidates=[n for n,v in s['nodes'].items()if n!='gptgenesisbp'and not v['banned']and v['strikes']==0 and v['safety_exists']and v['distance']>=110 and len(set(s['recent'])-{n})>=19]
 if candidates:break
 assert time.monotonic()<end,'No safe canary window';time.sleep(2)
name=max(candidates,key=lambda n:(s['nodes'][n]['standby'],s['nodes'][n]['distance']));v=nodes[name];cfg=Path(v['config_dir'])/'config.ini';raw=cfg.read_bytes();text=raw.decode();lines=text.splitlines();hits=[l for l in lines if l.strip().startswith('vote-threads')and '='in l];assert len(hits)==1 and hits[0].split('=',1)[1].strip()=='2';updated=text.replace(hits[0],hits[0].split('=')[0]+'= 1');assert updated!=text
record={'utc':g.now(),'target':name,'old_config_SHA256':g.sha(cfg),'new_config_SHA256':hashlib.sha256(updated.encode()).hexdigest(),'profile_change':'vote-threads2->1 only, full validation/native quorum/safety all retained','oldPID':pids[name]}
private=R/'private'/'config-backups';private.mkdir(parents=True,mode=0o700);(private/(name+'.before-vote-one.ini')).write_bytes(raw);(R/'VOTE_ONE_CANARY_STARTED.json').write_text(json.dumps(record,indent=2)+'\n');st=cfg.stat();tmp=cfg.with_name('config.cpu-one.tmp');assert not tmp.exists();tmp.write_text(updated);os.chown(tmp,st.st_uid,st.st_gid);tmp.chmod(st.st_mode&0o777);os.replace(tmp,cfg);v['config_sha256']=g.sha(cfg);g.save('GUARD_NODE_MAP.json',nodes)
# Re-read duty/quorum immediately before the normal same-data operation.
s,i,pids,rows,m=g.status_context(nodes);assert s['nodes'][name]['distance']>=100 and len(set(s['recent'])-{name})>=19
result=g.perform(name,nodes,(s,i,pids,rows,m));record['recycle']=result;assert result['status']=='PASS_ONE_GUARDED_RECYCLE',result
samples=[]
for n in range(12):
 s,i,pids,ro,m=g.status_context(nodes);samples.append({'utc':g.now(),'head':i['head_block_num'],'LIB':i['last_irreversible_block_num'],'head_age':s['head_age'],'LIB_age':s['lib_age'],'recent':len(s['recent'])});assert s['head_age']<5 and s['lib_age']<20 and len(s['recent'])>=19
 if n<11:time.sleep(5)
def ticks():
 out={}
 for p in(Path('/proc')/pids[name]/'task').iterdir():
  try:
   st=(p/'stat').read_text();tail=st[st.rfind(')')+2:].split();out[p.name]=(st[st.find('(')+1:st.rfind(')')],int(tail[11])+int(tail[12]))
  except OSError:pass
 return out
x=ticks();t=time.monotonic();time.sleep(4);elapsed=time.monotonic()-t;y=ticks();hz=os.sysconf('SC_CLK_TCK');cpu=[{'thread':v[0],'CPU':round((v[1]-x[k][1])/hz/elapsed*100,3)}for k,v in y.items()if k in x];record.update(status='PASS_ONE_VOTE_THREAD_CANARY_60SEC',utc=g.now(),samples=samples,threads_CPU=cpu,newPID=pids[name],other_PIDs_unchanged=all(g.state(nodes[n]['unit'])['MainPID']==pid for n,pid in result['other_PIDs'].items()));assert record['other_PIDs_unchanged'];(R/'VOTE_ONE_CANARY_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record))
