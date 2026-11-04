"""Remote-only dev-proposal UI qualification, no chain or service mutation."""
from pathlib import Path
import hashlib,importlib.util,json,subprocess,tarfile
D=Path(__file__).resolve().parent;W=D.parents[1];S=W/'PureEOS-explorer-key-history-20261103'
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c);owner=c.read();assert owner['instance']['main_ip']=='66.245.194.132'
archive=D/'frontend-source-r1.tar.gz'
files=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=S).decode().split('\0')
with tarfile.open(archive,'w:gz')as t:
 for name in files:
  if name and not set(Path(name).parts)&{'performance-results','release-work-20261104'}:t.add(S/name,arcname=name,recursive=False)
digest=hashlib.sha256(archive.read_bytes()).hexdigest();K='/opt/pureeos-dev-proposals-20261104-r1'
subprocess.run(c.ssh()+['mkdir','-p',K],check=True)
o=['-i',owner['ssh_key_path'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+owner['known_hosts']]
subprocess.run(['scp',*o,str(archive),'root@66.245.194.132:'+K+'/'+archive.name],check=True)
worker=r'''
from pathlib import Path
import hashlib,json,os,subprocess,traceback
K=Path('/opt/pureeos-dev-proposals-20261104-r1');S=K/'frontend';R=Path('/opt/pureeos-keynet-20261103-r2')
try:
 env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],'NODE_OPTIONS':'--max-old-space-size=512'}
 (S/'remote-vitest.config.ts').write_text("import config from './vite.config';export default {...config,test:{testTimeout:15000,setupFiles:['./remote-test-setup.ts']}};")
 (S/'remote-test-setup.ts').write_text("import {configure} from '@testing-library/dom';configure({asyncUtilTimeout:5000});")
 with(K/'frontend-test-r1.log').open('w')as log:subprocess.run(['npm','test','--','--config','remote-vitest.config.ts','--maxWorkers=1'],cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 env.update(VITE_DATA_MODE='live',VITE_RPC_URL='https://aic-test.matrixai.one/api',VITE_HISTORY_URL='https://aic-test.matrixai.one/history',VITE_TOKEN_SYMBOL='AIC')
 with(K/'frontend-build-r1.log').open('w')as log:subprocess.run(['npm','run','build'],cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 (K/'FRONTEND_RESULT.json').write_text(json.dumps({'status':'PASS','source_sha256':DIGEST,'files':{str(p.relative_to(S/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()for p in(S/'dist').rglob('*')if p.is_file()}},indent=2)+'\n')
 print('DEV PROPOSAL FRONTEND PASS',flush=True)
except BaseException:
 (K/'FRONTEND_FAILURE_r1.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
'''.replace('DIGEST',repr(digest))
setup=r'''
from pathlib import Path
import hashlib,json,subprocess,tarfile
K=Path('/opt/pureeos-dev-proposals-20261104-r1');S=K/'frontend';assert not S.exists();S.mkdir()
assert hashlib.sha256((K/'frontend-source-r1.tar.gz').read_bytes()).hexdigest()==DIGEST
with tarfile.open(K/'frontend-source-r1.tar.gz')as t:t.extractall(S,filter='data')
Q=Path('/opt/pureeos-key-history-20261103-r1/r3/frontend')
for n in ['package.json','package-lock.json']:assert(S/n).read_bytes()==(Q/n).read_bytes()
(S/'node_modules').symlink_to(Q/'node_modules',target_is_directory=True)
(K/'build-frontend-r1.py').write_text(WORKER)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-dev-proposals-build-r1-20261104','--property=Type=oneshot','--property=Nice=10','--property=CPUWeight=100','--property=CPUQuota=50%','--property=MemoryHigh=768M','--property=MemoryMax=1G','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=3600','--property=StandardOutput=append:'+str(K/'build-controller-r1.log'),'--property=StandardError=append:'+str(K/'build-controller-r1.log'),'python3','-B',str(K/'build-frontend-r1.py')],capture_output=True,text=True)
assert not q.returncode,q.stderr
print(json.dumps({'status':'REMOTE_UI_BUILD_LAUNCHED','source_sha256':DIGEST,'root':str(K)}))
'''.replace('DIGEST',repr(digest)).replace('WORKER',repr(worker))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=setup,text=True,capture_output=True);assert not q.returncode,q.stderr
(D/'FRONTEND_LAUNCH.json').write_text(q.stdout);print(q.stdout)
