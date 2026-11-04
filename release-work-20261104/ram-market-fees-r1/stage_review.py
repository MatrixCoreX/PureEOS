"""Queue read-only application review after both private suites finish."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);s=c.read();assert s['instance']['main_ip']=='66.245.194.132'
K='/opt/pureeos-key-ram-market-20261104-r1'
opts=['-i',s['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+s['known_hosts']]
for n in ['integration.mjs','browser-check.cjs']:subprocess.run(['scp',*opts,str(D/n),'root@66.245.194.132:'+K+'/'+n],check=True)
script=r'''
from pathlib import Path
import json,os,subprocess,time,traceback
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2')
try:
 deadline=time.monotonic()+7200
 while not all((K/('QUALIFICATION_'+n+'.json')).exists()for n in ['native','fixture']):
  assert not(K/'QUALIFICATION_FAILURE.json').exists(),'Qualification failed; retain its evidence and stop review'
  assert time.monotonic()<deadline,'Qualification wait expired';time.sleep(10)
 for n in ['native','fixture']:assert json.loads((K/('QUALIFICATION_'+n+'.json')).read_text())['status']=='PASS'
 env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],'NODE_OPTIONS':'--max-old-space-size=512'}
 for script in ['integration.mjs','browser-check.cjs']:
  with (K/(script+'.log')).open('w')as log:subprocess.run(['node',str(K/script)],cwd=K,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 assert json.loads((K/'INTEGRATION_RESULT.json').read_text())['SDK_comparisons']==480
 assert json.loads((K/'browser/RESULT.json').read_text())['status']=='PASS'
 (K/'APPLICATION_REVIEW.json').write_text(json.dumps({'status':'PASS','SDK_comparisons':480,'Chrome_variants':8,'public_activation':False,'scope':'Read-only original RPC; candidate code-hash substitution is explicitly a browser fixture'},indent=2)+'\n')
 print('APPLICATION REVIEW PASS',flush=True)
except BaseException:
 (K/'APPLICATION_REVIEW_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
'''
setup=r'''
from pathlib import Path
import json,subprocess
K=Path('/opt/pureeos-key-ram-market-20261104-r1');p=K/'review.py';assert not p.exists();p.write_text(SCRIPT)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-review-r1-20261104','--property=Type=oneshot','--property=Nice=10','--property=CPUWeight=100','--property=CPUQuota=50%','--property=IOWeight=10','--property=MemoryHigh=768M','--property=MemoryMax=1536M','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=9000','--property=StandardOutput=append:'+str(K/'review.log'),'--property=StandardError=append:'+str(K/'review.log'),'python3','-B',str(p)],capture_output=True,text=True)
assert q.returncode==0,q.stderr
print(json.dumps({'status':'REVIEW_QUEUED_AFTER_PRIVATE_TESTS','live_mutations':False}))
'''.replace('SCRIPT',repr(script))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=setup,text=True,capture_output=True);assert not q.returncode,q.stderr
(D/'REVIEW_LAUNCH.json').write_text(q.stdout);print(q.stdout)
