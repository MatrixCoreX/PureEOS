from pathlib import Path
import os,json,subprocess,hashlib,time,traceback,fcntl,sys,concurrent.futures
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
B=Path('/opt/aic-bp-discovery-live-20261030');C=Path('/opt/aic-bp-maintenance-30nodes-20261030');P=Path('/opt/aic-bp-maintenance-30peer-20261030');J=Path('/opt/aic-bp-gossip-roster-resync-20261030')
sys.path.insert(0,str(B/'input'));import functional_common as f
def read(p):return json.loads(p.read_text())
def save(d):d['UTC']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());f.write(C/'ACTIVATION_STATUS.json',d)
opts=['-i',str(B/'peer-access-key'),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(B/'peer_known_hosts')]
def operate(n,code):
 if n['primary']:r=subprocess.run(['python3','-B','-c',code],capture_output=True,text=True,timeout=180)
 else:r=subprocess.run(['ssh',*opts,'root@'+n['ip'],'python3 -B -'],input=code,capture_output=True,text=True,timeout=180)
 assert r.returncode==0,r.stderr[-1500:]
 return json.loads(r.stdout)if r.stdout.strip()else None
def start_node(n,initial=True,suffix=''):
 root=C if n['primary']else P;D=root/'live'/n['account'];unit=n['unit'] if not suffix else n['unit']+'-'+suffix
 args=['systemd-run','--unit='+unit,'--property=Type=exec','--property=User='+str(n['uid']),'--property=Group='+str(n['gid']),'--property=MemoryMax=5G','--property=MemorySwapMax=0','--property=CPUAffinity=0-15','--property=RuntimeMaxSec=86400','--property=RemainAfterExit=yes','--property=StandardOutput=append:'+str(D/'node.log'),'--property=StandardError=append:'+str(D/'node.log')]
 if n['primary']:
  args +=['--property=RootDirectory=/opt/aic-tps/rootfs','--property=BindReadOnlyPaths=/opt:/opt','--property=BindPaths='+str(C)+':/campaign',str(C/'runtime/nodeos'),'--config-dir','/campaign/live/'+n['account']+'/config','--data-dir','/campaign/live/'+n['account']+'/data']
  snapshot='/campaign/fixture/bootstrap.snapshot'
 else:
  args +=['--property=NoNewPrivileges=yes',str(P/'runtime/ld-linux-x86-64.so.2'),'--library-path',str(P/'runtime/lib'),str(P/'runtime/nodeos'),'--config-dir',str(D/'config'),'--data-dir',str(D/'data')];snapshot=str(P/'fixture/bootstrap.snapshot')
 if initial:args+=['--snapshot',snapshot]
 args+=['--chain-threads','6','--force-all-checks','--disable-replay-opts']
 code="import subprocess,os,json\nr=subprocess.run("+repr(args)+",capture_output=True,text=True);assert r.returncode==0,r.stderr\npid=int(subprocess.check_output(['systemctl','show',"+repr(unit)+",'-p','MainPID','--value'],text=True));assert pid>0;print(json.dumps(dict(pid=pid,unit="+repr(unit)+",initial_snapshot="+str(initial)+")))"
 result=operate(n,code);n['unit']=unit;return result
def observations(nodes):
 result={}
 for n in nodes:
  try:
   info=f.rpc(n,'get_info');gossip=f.rpc(n,'bp_gossip_peers',ns='net');conns=f.rpc(n,'connections',ns='net')
   result[n['account']]=dict(info=info,gossip=gossip,connections=conns)
  except BaseException as e:result[n['account']]=dict(error=str(e)[:400])
 return result
def wait_stage(nodes,target,initial_head):
 limit=time.monotonic()+300;history=[]
 while True:
  states=observations(nodes);good=all('info'in x for x in states.values())
  if good:
   good=all(x['info']['chain_id']==f.T['chain_id']and len({p['producer_name']for p in x['gossip']})>=target for x in states.values())
   heads=[x['info']['head_block_num']for x in states.values()]
   good=good and max(heads)-min(heads)<=4
   if target>=21:good=good and min(x['info']['last_irreversible_block_num']for x in states.values())>initial_head+2
   else:assert max(x['info']['last_irreversible_block_num']for x in states.values())<=initial_head,'Sub-quorum group finalized newly produced blocks'
  history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),nodes=states));f.write(C/('stage-'+str(target)+'-observations.json'),history)
  if good:return states
  assert time.monotonic()<limit,'Stage '+str(target)+' discovery/finality did not reach the predeclared 300s gate'
  time.sleep(3)
def profile_host(machine,nodes):
 root=C if machine['primary']else P
 code="from pathlib import Path\nimport os,json,subprocess,hashlib,time\nroot=Path("+repr(str(root))+");out=[]\nfor n in "+repr(nodes)+":\n"
 code+=" D=root/'live'/n['account'];pid=int(subprocess.check_output(['systemctl','show',n['unit'],'-p','MainPID','--value'],text=True));assert pid>0\n status=(Path('/proc')/str(pid)/'status').read_text();m={line.split(':',1)[0]:line.split(':',1)[1].strip()for line in status.splitlines()if ':'in line}\n stat=(Path('/proc')/str(pid)/'stat').read_text().split(') ',1)[1].split();safety=[]\n for p in (D/'data').rglob('*'):\n  if p.is_file()and'safety'in p.name.lower():safety.append(dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size))\n cfg=(D/'config/config.ini').read_text();assert hashlib.sha256(cfg.encode()).hexdigest()==n['configuration_sha256']\n out.append(dict(account=n['account'],pid=pid,uid=os.stat(Path('/proc')/str(pid)).st_uid,RSS_kB=int(m.get('VmRSS','0 kB').split()[0]),threads=int(m['Threads']),FDs=len(list((Path('/proc')/str(pid)/'fd').iterdir())),CPU_ticks=int(stat[11])+int(stat[12]),clock_ticks_per_second=os.sysconf('SC_CLK_TCK'),monotonic_ns=time.monotonic_ns(),safety=safety,configuration_sha256=hashlib.sha256(cfg.encode()).hexdigest(),startup=[line for line in (D/'node.log').read_text(errors='replace').splitlines()if('parallel'in line.lower()or'minimum'in line.lower()or'physical'in line.lower())and'PVT_'not in line][:12]))\nprint(json.dumps(out))"
 return operate(machine,code)
topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.F=read(C/'fixture/PUBLIC_FIXTURE.json');f.N=nodes

E=C
C0=Path('/opt/aic-bp-discovery-30nodes-20261030')
