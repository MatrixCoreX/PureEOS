from pathlib import Path
import hashlib,json,os,subprocess
F=Path('/opt/pureeos-explorer-key-rate-20261104-r4-collapse')
S=F/'source';R=Path('/opt/pureeos-keynet-20261103-r2')
env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH']}
subprocess.run(['npm','test','--','src/components/KeyFeeEstimate.test.tsx','--config','remote-vitest.config.ts','--maxWorkers=1'],cwd=S,env=env,check=True)
env.update(VITE_DATA_MODE='live',VITE_RPC_URL='https://aic-test.matrixai.one/api',VITE_HISTORY_URL='https://aic-test.matrixai.one/history',VITE_TOKEN_SYMBOL='AIC')
subprocess.run(['npm','run','build'],cwd=S,env=env,check=True)
result={'status':'PASS','scope':'Default-collapsed fee formula within existing footer disclosure',
        'baseline_full_tests':237,'affected_component_tests':'PASS','TypeScript_and_production_build':'PASS',
        'patch_files':{n:hashlib.sha256((S/n).read_bytes()).hexdigest() for n in ['src/components/KeyFeeEstimate.tsx','src/lib/translations.ts','src/styles.css']},
        'files':{str(p.relative_to(S/'dist')):hashlib.sha256(p.read_bytes()).hexdigest() for p in (S/'dist').rglob('*') if p.is_file()}}
(F/'BUILD_RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
node=str(R/'tools/node-current/bin/node')
subprocess.run([node,str(F/'integration.mjs')],env=env,check=True)
env.update(PLAYWRIGHT_BROWSERS_PATH='/opt/pureeos-key-history-20261103-r1/tools/browsers',
           LD_LIBRARY_PATH='/opt/pureeos-key-history-20261103-r1/tools/browser-libs/usr/lib/x86_64-linux-gnu',
           FONTCONFIG_FILE='/opt/pureeos-key-history-20261103-r1/tools/fonts.conf')
subprocess.run([node,str(F/'browser-check.cjs'),'preview'],env=env,check=True)
