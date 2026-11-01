from pathlib import Path
import subprocess,json,time,datetime,traceback
G=Path('/opt/pureeos-governance-seven-day-20261101-r1');R=G/'immediate-history-profile-r2';D=G/'immediate-replay-r5';unit='pureeos-immediate-history-profile-r2-20261101.service'
def st():return {k:subprocess.check_output(['systemctl','show',unit,'--property='+k,'--value'],text=True).strip()for k in ['MainPID','ActiveState','Result']}
probe="import urllib.request,json;print(json.dumps(json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:15090/v1/chain/get_info',data=b'{}',headers={'Content-Type':'application/json'}),timeout=2))))"
try:
 cmd=[str(G/'runtime-package/bin/nodeos'),'--data-dir',str(D/'node-data'),'--config-dir',str(D/'node-config'),'--disable-replay-opts','--force-all-checks','--integrity-hash-on-start','--resource-monitor-space-absolute-gb','8']
 q=subprocess.run(['systemd-run','--no-block','--unit='+unit.removesuffix('.service'),'--property=Type=simple','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=30','--property=RuntimeMaxSec=180','--property=StandardOutput=append:'+str(R/'nodeos.log'),'--property=StandardError=append:'+str(R/'nodeos.log')]+cmd,capture_output=True,text=True);assert q.returncode==0,q.stderr
 deadline=time.monotonic()+45
 while True:
  pid=int(st()['MainPID'])
  if pid:
   q=subprocess.run(['nsenter','--target',str(pid),'--net','/usr/bin/python3','-c',probe],capture_output=True,text=True,timeout=5)
   if q.returncode==0:break
  assert time.monotonic()<deadline,'RPC readiness timeout'
  time.sleep(.3)
 script=(R/'integration.py').read_text()
 q=subprocess.run(['nsenter','--target',str(pid),'--net','/usr/bin/python3','-B','-c',script],capture_output=True,text=True,timeout=90)
 (R/'integration.stderr.log').write_text(q.stderr);assert q.returncode==0,q.stderr[-1600:]
 out=json.loads(q.stdout);assert out['status']=='PASS';(R/'RESULT.json').write_text(json.dumps(out,indent=2)+'\n')
except Exception as e:
 (R/'FAILURE.json').write_text(json.dumps({'status':'FAIL','error':str(e),'traceback':traceback.format_exc()},indent=2));raise
finally:
 subprocess.run(['systemctl','stop',unit],capture_output=True,text=True,timeout=120)
 (R/'CLEANUP.json').write_text(json.dumps(st(),indent=2)+'\n')
