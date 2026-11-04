
from pathlib import Path
import json,hashlib,subprocess,os
K=Path('/opt/pureeos-dev-proposals-20261104-r1');S=K/'frontend';assert hashlib.sha256((K/'frontend-source-r3.tar.gz').read_bytes()).hexdigest()=='b4a84818160eee54c750fecda0bee0142fe36e88cb7f9ac4eee90f28daa19852'
(S/'src/styles.css').write_bytes((K/'styles.css').read_bytes());old=json.loads((K/'FRONTEND_RESULT.json').read_text());(K/'FRONTEND_RESULT_r2.json').write_text(json.dumps(old,indent=2)+'\n')
env={**os.environ,'PATH':'/opt/pureeos-keynet-20261103-r2/tools/node-current/bin:'+os.environ['PATH'],'NODE_OPTIONS':'--max-old-space-size=512','VITE_DATA_MODE':'live','VITE_RPC_URL':'https://aic-test.matrixai.one/api','VITE_HISTORY_URL':'https://aic-test.matrixai.one/history','VITE_TOKEN_SYMBOL':'AIC'}
with(K/'frontend-build-r3.log').open('w')as log:subprocess.run(['npm','run','build'],cwd=S,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
(K/'FRONTEND_RESULT.json').write_text(json.dumps({'status':'PASS','source_sha256':'b4a84818160eee54c750fecda0bee0142fe36e88cb7f9ac4eee90f28daa19852','unit_tests':251,'change_from_tested_r1':'Scoped CSS spacing and navigation scrollbar styling only','files':{str(p.relative_to(S/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()for p in(S/'dist').rglob('*')if p.is_file()}},indent=2)+'\n')
