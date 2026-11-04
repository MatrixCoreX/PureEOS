"""Use the existing qualified remote Chromium cache; no software installation."""
from pathlib import Path
import importlib.util,json,subprocess
D=Path(__file__).resolve().parent;W=D.parents[1]
sp=importlib.util.spec_from_file_location('cloud',W/'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c=importlib.util.module_from_spec(sp);sp.loader.exec_module(c)
script=r'''
from pathlib import Path
import json,os,subprocess,traceback
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2')
try:
 for n in ['native','fixture']:assert json.loads((K/('QUALIFICATION_'+n+'_r3.json')).read_text())['status']=='PASS'
 assert json.loads((K/'INTEGRATION_RESULT.json').read_text())['SDK_comparisons']==480
 env={**os.environ,'PATH':str(R/'tools/node-current/bin')+':'+os.environ['PATH'],'NODE_OPTIONS':'--max-old-space-size=512','PLAYWRIGHT_BROWSERS_PATH':'/opt/pureeos-key-history-20261103-r1/tools/browsers','LD_LIBRARY_PATH':'/opt/pureeos-key-history-20261103-r1/tools/browser-libs/usr/lib/x86_64-linux-gnu','FONTCONFIG_FILE':'/opt/pureeos-key-history-20261103-r1/tools/fonts.conf'}
 assert Path(env['PLAYWRIGHT_BROWSERS_PATH']).is_dir()
 with (K/'browser-r5.log').open('w')as log:subprocess.run(['node',str(K/'browser-check.cjs')],cwd=K,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 r=json.loads((K/'browser/RESULT.json').read_text());assert r['status']=='PASS'and len(r['variants'])==8
 (K/'APPLICATION_REVIEW_r5.json').write_text(json.dumps({'status':'PASS','SDK_comparisons':480,'Chrome_variants':8,'public_activation':False,'browser_cache':env['PLAYWRIGHT_BROWSERS_PATH'],'scope':r['scope'],'first_launch_failure':'APPLICATION_REVIEW_FAILURE_r3.json'},indent=2)+'\n')
 print('BROWSER R5 PASS',flush=True)
except BaseException:
 (K/'APPLICATION_REVIEW_FAILURE_r5.json').write_text(json.dumps({'status':'FAIL','error':traceback.format_exc()},indent=2)+'\n');raise
'''
setup=r'''
from pathlib import Path
import json,subprocess
K=Path('/opt/pureeos-key-ram-market-20261104-r1');p=K/'browser-r5.py';assert not p.exists();p.write_text(SCRIPT)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-browser-r5-20261104','--property=Type=oneshot','--property=Nice=10','--property=CPUWeight=100','--property=CPUQuota=50%','--property=MemoryHigh=768M','--property=MemoryMax=1536M','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=1800','--property=StandardOutput=append:'+str(K/'browser-r5-controller.log'),'--property=StandardError=append:'+str(K/'browser-r5-controller.log'),'python3','-B',str(p)],capture_output=True,text=True)
assert q.returncode==0,q.stderr
print(json.dumps({'status':'BROWSER_R5_LAUNCHED','cached_browser':True,'new_installations':0}))
'''.replace('SCRIPT',repr(script))
q=subprocess.run(c.ssh()+['python3','-B','-'],input=setup,text=True,capture_output=True);assert not q.returncode,q.stderr
(D/'BROWSER_R5_LAUNCH.json').write_text(q.stdout);print(q.stdout)
