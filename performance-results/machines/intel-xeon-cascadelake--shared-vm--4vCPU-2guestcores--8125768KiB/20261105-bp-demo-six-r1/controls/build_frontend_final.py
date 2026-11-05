"""Final caption-only refinement: focused checks and production TypeScript build."""
from pathlib import Path
import json,os,hashlib,subprocess,traceback,re
R=Path('/opt/pureeos-bp-demo-six-20261105-r1');F=R/'frontend-final'
try:
 assert json.loads((R/'FRONTEND_RESULT.json').read_text())['status']=='PASS'
 env={**os.environ,'PATH':'/opt/pureeos-devnet-20261105-r1/tools/node-current/bin:'+os.environ['PATH'],'NODE_OPTIONS':'--max-old-space-size=512'}
 (F/'remote-vitest.config.ts').write_text("import config from './vite.config';export default {...config,test:{testTimeout:15000,setupFiles:['./remote-test-setup.ts']}};")
 (F/'remote-test-setup.ts').write_text("import {configure} from '@testing-library/dom';configure({asyncUtilTimeout:5000});")
 with(R/'frontend-final-focused-tests.log').open('w')as log:subprocess.run(['npm','test','--','--config','remote-vitest.config.ts','src/lib/producers.test.ts','src/lib/i18n.test.ts','--maxWorkers=1'],cwd=F,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 env.update(VITE_DATA_MODE='live',VITE_RPC_URL='https://aic-test.matrixai.one/api',VITE_HISTORY_URL='https://aic-test.matrixai.one/history',VITE_TOKEN_SYMBOL='AIC')
 with(R/'frontend-final-build.log').open('w')as log:subprocess.run(['npm','run','build'],cwd=F,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 text=re.sub(r'\x1b\[[0-9;]*m','',(R/'frontend-final-focused-tests.log').read_text());checks=int(re.search(r'Tests\s+(\d+) passed',text).group(1))
 result={'status':'PASS','source_SHA256':hashlib.sha256((R/'frontend-final-source.tar.gz').read_bytes()).hexdigest(),'earlier_full_suite_checks':268,'final_focused_checks':checks,'scope':'Same capacity data/branches; final caption compacts DOM to keep existing120px cards. Existing producer-summary/i18n tests plus finalTypeScript/build; actual Chrome remains required.','files':{str(p.relative_to(F/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()for p in(F/'dist').rglob('*')if p.is_file()}}
 (R/'FRONTEND_FINAL_RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print('FINAL_CAPACITY_CAPTION_FOCUSED_TS_BUILD_PASS',flush=True)
except BaseException:
 (R/'FRONTEND_FINAL_FAILURE.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
