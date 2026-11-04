"""Preserve r1; fund the private RAM buyer before governance sealing in r2."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);s=c.read();assert s['instance']['main_ip']=='66.245.194.132'
K='/opt/pureeos-key-ram-market-20261104-r1';p=W/'PureEOS-Key/system-contracts/eosio.key/tests/chain_test.py'
opts=['-i',s['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+s['known_hosts']]
subprocess.run(['scp',*opts,str(p),'root@66.245.194.132:'+K+'/chain_test-r3.py'],check=True)
script=r'''
from pathlib import Path
import hashlib,json,os,subprocess,traceback
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');S=K/'source-r3'
try:
 assert json.loads((K/'FRONTEND_RESULT.json').read_text())['status']=='PASS'
 env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],'PYTHONUNBUFFERED':'1'}
 for label,extra in [('native',[]),('fixture',['--fixture'])]:
  work=Path('/opt/kram-q')/(label+'-r3');assert not work.exists()
  command=['python3','-B',str(S/'tests/qualify.py'),'--package',str(R/'pureeos-test-1.0.0-test.1'),'--work',str(work),'--artifacts',str(K/'artifacts'),'--native-cleos',str(R/'native-client/cleos'),'--review-seconds','1800',*extra]
  with (K/('qualification-'+label+'-r3.log')).open('w')as log:subprocess.run(command,cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  result=json.loads((work/'RESULT.json').read_text());assert result['status']=='PASS'
  (K/('QUALIFICATION_'+label+'_r3.json')).write_text(json.dumps({'status':'PASS','work':str(work),'checks':len(result['checks']),'report_sha256':hashlib.sha256((work/'RESULT.json').read_bytes()).hexdigest(),'chain_test_SHA256':result['chain_test_SHA256'],'scope':result.get('scope')},indent=2)+'\n')
 print('RAM QUALIFICATION R3 PASS',flush=True)
except BaseException:
 (K/'QUALIFICATION_FAILURE_r3.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
'''
setup=r'''
from pathlib import Path
import hashlib,json,shutil,subprocess
K=Path('/opt/pureeos-key-ram-market-20261104-r1');S=K/'source-r3'
assert (K/'QUALIFICATION_FAILURE_r2.json').exists()and not S.exists()
subprocess.run(['cp','-a','--reflink=auto',str(K/'source-final'),str(S)],check=True)
assert hashlib.sha256((K/'chain_test-r3.py').read_bytes()).hexdigest()==DIGEST
shutil.copy2(K/'chain_test-r3.py',S/'tests/chain_test.py')
(K/'qualify-r3.py').write_text(SCRIPT)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-qualify-r3-20261104','--property=Type=oneshot','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=100','--property=IOWeight=10','--property=MemoryHigh=1536M','--property=MemoryMax=8G','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=7200','--property=StandardOutput=append:'+str(K/'qualification-r3.log'),'--property=StandardError=append:'+str(K/'qualification-r3.log'),'python3','-B',str(K/'qualify-r3.py')],capture_output=True,text=True)
assert q.returncode==0,q.stderr
review=(K/'review.py').read_text().replace("('QUALIFICATION_'+n+'.json')","('QUALIFICATION_'+n+'_r3.json')").replace("'QUALIFICATION_FAILURE.json'","'QUALIFICATION_FAILURE_r3.json'").replace("'APPLICATION_REVIEW.json'","'APPLICATION_REVIEW_r3.json'").replace("'APPLICATION_REVIEW_FAILURE.json'","'APPLICATION_REVIEW_FAILURE_r3.json'")
(K/'review-r3.py').write_text(review)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-review-r3-20261104','--property=Type=oneshot','--property=Nice=10','--property=CPUWeight=100','--property=CPUQuota=50%','--property=MemoryHigh=768M','--property=MemoryMax=1536M','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=9000','--property=StandardOutput=append:'+str(K/'review-r3.log'),'--property=StandardError=append:'+str(K/'review-r3.log'),'python3','-B',str(K/'review-r3.py')],capture_output=True,text=True)
assert q.returncode==0,q.stderr
print(json.dumps({'status':'R3_PRIVATE_QUALIFICATION_AND_REVIEW_QUEUED','test_source_sha256':DIGEST,'r1_failure_preserved':True,'WASM_unchanged':True,'live_mutations':False}))
'''.replace('DIGEST',repr(hashlib.sha256(p.read_bytes()).hexdigest())).replace('SCRIPT',repr(script))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=setup,text=True,capture_output=True);assert not q.returncode,q.stderr
(D/'RETRY_LAUNCH_r3.json').write_text(q.stdout);print(q.stdout)
