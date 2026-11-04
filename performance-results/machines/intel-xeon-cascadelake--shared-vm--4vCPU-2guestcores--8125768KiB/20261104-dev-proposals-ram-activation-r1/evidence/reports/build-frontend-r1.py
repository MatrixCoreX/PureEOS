
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
 (K/'FRONTEND_RESULT.json').write_text(json.dumps({'status':'PASS','source_sha256':'086f9a00edfe35bb382586a2754b8126ba34316aaa43aea6ea6f0c6620600070','files':{str(p.relative_to(S/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()for p in(S/'dist').rglob('*')if p.is_file()}},indent=2)+'\n')
 print('DEV PROPOSAL FRONTEND PASS',flush=True)
except BaseException:
 (K/'FRONTEND_FAILURE_r1.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
