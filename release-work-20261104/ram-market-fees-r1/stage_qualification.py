"""Stage final sources and sequential remote frontend/native qualification."""
from pathlib import Path
import hashlib, importlib.util, json, subprocess, tarfile
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);state=c.read()
assert state['instance']['main_ip']=='66.245.194.132'
K='/opt/pureeos-key-ram-market-20261104-r1'
opts=['-i',state['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+state['known_hosts']]
digests={}
for name,source in [('contract-final',W/'PureEOS-Key/system-contracts/eosio.key'),('frontend',W/'PureEOS-explorer-key-history-20261103')]:
    archive=D/(name+'.tar.gz')
    with tarfile.open(archive,'w:gz')as t:
        if name=='frontend':
            files=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=source).decode().split('\0')
            for f in files:
                if f and 'performance-results'not in Path(f).parts:t.add(source/f,arcname=f,recursive=False)
        else:
            for p in sorted(source.rglob('*')):
                if p.is_file()and not set(p.relative_to(source).parts)&{'node_modules','sdk-tests','artifacts','__pycache__','state'}:t.add(p,arcname=str(p.relative_to(source)),recursive=False)
    digests[name]=hashlib.sha256(archive.read_bytes()).hexdigest()
    subprocess.run(['scp',*opts,str(archive),'root@66.245.194.132:'+K+'/'+archive.name],check=True)
controller=r'''
from pathlib import Path
import hashlib,json,os,shutil,subprocess,time,traceback
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');S=K/'source-final';F=K/'frontend'
try:
 deadline=time.monotonic()+1800
 while not(K/'BUILD_RESULT.json').exists():
  assert not(K/'BUILD_FAILURE.json').exists(),'Remote compilation failed'
  assert time.monotonic()<deadline,'Build wait expired';time.sleep(5)
 assert json.loads((K/'BUILD_RESULT.json').read_text())['status']=='PASS'
 for folder in ['src','include']:
  for p in (K/'source'/folder).rglob('*'):
   if p.is_file():assert p.read_bytes()==(S/p.relative_to(K/'source')).read_bytes(),'Compiled source mismatch'
 assert (K/'source/tests/fixtures/system_mock.cpp').read_bytes()==(S/'tests/fixtures/system_mock.cpp').read_bytes()
 assert (K/'artifacts/eosio.key.abi').read_bytes()==(S/'abi/eosio.key.abi').read_bytes()
 env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],'PYTHONUNBUFFERED':'1','NODE_OPTIONS':'--max-old-space-size=512'}
 (F/'remote-vitest.config.ts').write_text("import config from './vite.config';export default {...config,test:{testTimeout:15000,setupFiles:['./remote-test-setup.ts']}};")
 (F/'remote-test-setup.ts').write_text("import {configure} from '@testing-library/dom';configure({asyncUtilTimeout:5000});")
 with (K/'frontend-test.log').open('w')as log:
  subprocess.run(['npm','test','--','--config','remote-vitest.config.ts','--maxWorkers=1'],cwd=F,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 buildenv={**env,'VITE_DATA_MODE':'live','VITE_RPC_URL':'https://aic-test.matrixai.one/api','VITE_HISTORY_URL':'https://aic-test.matrixai.one/history','VITE_TOKEN_SYMBOL':'AIC'}
 with (K/'frontend-build.log').open('w')as log:subprocess.run(['npm','run','build'],cwd=F,env=buildenv,stdout=log,stderr=subprocess.STDOUT,check=True)
 (K/'FRONTEND_RESULT.json').write_text(json.dumps({'status':'PASS','files':{str(p.relative_to(F/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()for p in(F/'dist').rglob('*')if p.is_file()}},indent=2)+'\n')
 # No hard CPU/memory cap below the native TEST startup floor. Functional test
 # nodes are sequential, private-network only, low priority and always stopped.
 env.pop('NODE_OPTIONS',None)
 for label,extra in [('native',[]),('fixture',['--fixture'])]:
  work=Path('/opt/kram-q')/label;assert not work.exists(),'Preserve failed attempts'
  command=['python3','-B',str(S/'tests/qualify.py'),'--package',str(R/'pureeos-test-1.0.0-test.1'),'--work',str(work),'--artifacts',str(K/'artifacts'),'--native-cleos',str(R/'native-client/cleos'),'--review-seconds','1800',*extra]
  with (K/('qualification-'+label+'.log')).open('w')as log:subprocess.run(command,cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
  result=json.loads((work/'RESULT.json').read_text());assert result['status']=='PASS'
  (K/('QUALIFICATION_'+label+'.json')).write_text(json.dumps({'status':'PASS','work':str(work),'checks':len(result['checks']),'scope':result.get('scope'),'report_sha256':hashlib.sha256((work/'RESULT.json').read_bytes()).hexdigest()},indent=2)+'\n')
 print('RAM MARKET ALL REMOTE QUALIFICATION PASS',flush=True)
except BaseException:
 (K/'QUALIFICATION_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
'''
setup=r'''
from pathlib import Path
import hashlib,json,shutil,subprocess,tarfile
K=Path('/opt/pureeos-key-ram-market-20261104-r1');S=K/'source-final';F=K/'frontend';Q=Path('/opt/pureeos-explorer-key-rate-20261104-r4-collapse/source')
for name,target in [('contract-final',S),('frontend',F)]:
 assert not target.exists(),'Preserve previous attempt'
 a=K/(name+'.tar.gz');assert hashlib.sha256(a.read_bytes()).hexdigest()==DIGESTS[name];target.mkdir()
 with tarfile.open(a)as t:t.extractall(target,filter='data')
shutil.copytree(K/'source/node_modules',S/'node_modules',symlinks=True)
shutil.copytree(K/'source/sdk-tests',S/'sdk-tests')
if not Q.exists():Q=Path('/opt/pureeos-explorer-key-rate-20261104-r3/source')
for name in ['package.json','package-lock.json']:assert(F/name).read_bytes()==(Q/name).read_bytes()
subprocess.run(['cp','-a','--reflink=auto',str(Q/'node_modules'),str(F/'node_modules')],check=True)
(K/'qualify-all.py').write_text(CONTROLLER)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-qualify-r1-20261104','--property=Type=oneshot','--property=PrivateNetwork=yes','--property=Nice=10','--property=CPUWeight=100','--property=IOWeight=10','--property=MemoryHigh=1536M','--property=MemoryMax=8G','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=7200','--property=StandardOutput=append:'+str(K/'qualification.log'),'--property=StandardError=append:'+str(K/'qualification.log'),'python3','-B',str(K/'qualify-all.py')],capture_output=True,text=True)
assert q.returncode==0,q.stderr
print(json.dumps({'status':'REMOTE_SEQUENTIAL_QUALIFICATION_LAUNCHED','unit':'pureeos-key-ram-qualify-r1-20261104','digests':DIGESTS,'live_changes':False}))
'''.replace('DIGESTS',repr(digests)).replace('CONTROLLER',repr(controller))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=setup,text=True,capture_output=True)
assert q.returncode==0,q.stderr
(D/'QUALIFICATION_LAUNCH.json').write_text(q.stdout);print(q.stdout)
