"""Remote full frontend verification while the authorized chain pause frees RAM."""
from pathlib import Path
import hashlib, json, os, subprocess
F=Path('/opt/pureeos-explorer-key-rate-20261104-r3')
K=Path('/opt/pureeos-key-rate-20261104-r1')
R=Path('/opt/pureeos-keynet-20261103-r2')
S=F/'source'
pause=json.loads((K/'CHAIN_PAUSE_COMPLETED.json').read_text())
assert pause['status']=='CHAIN_AND_HISTORY_PAUSED'
for u in pause['nodes']:
    assert subprocess.run(['systemctl','is-active','--quiet',u]).returncode!=0
env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],
     'NODE_OPTIONS':'--max-old-space-size=512'}
subprocess.run(['npm','test','--','--config','remote-vitest.config.ts','--maxWorkers=1'],cwd=S,env=env,check=True)
env.update(VITE_DATA_MODE='live',VITE_RPC_URL='https://aic-test.matrixai.one/api',
           VITE_HISTORY_URL='https://aic-test.matrixai.one/history',VITE_TOKEN_SYMBOL='AIC')
subprocess.run(['npm','run','build'],cwd=S,env=env,check=True)
result={'status':'PASS','source_patch':json.loads((F/'NATIVE_BOOLEAN_SOURCE_PATCH.json').read_text()),
        'files':{str(p.relative_to(S/'dist')):hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in (S/'dist').rglob('*') if p.is_file()},
        'verification':'Full npm test (including native0/1 boolean regression), TypeScript and production build PASS',
        'context':'User-authorized coordinated chain/history pause; no node/cap changes or TPS'}
(F/'BUILD_RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
print('FINAL FULL FRONTEND BUILD PASS',flush=True)
