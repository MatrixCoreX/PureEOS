"""Launch remote cached-Chromium checks at low priority; no chain mutation."""
from pathlib import Path
import os,subprocess,sys,json
K=Path('/opt/pureeos-dev-proposals-20261104-r1');phase=sys.argv[1]
assert phase in ['preview','public-before','public-review','public-after','navigation-preview','navigation-public']
directory=phase if phase.startswith('navigation-')else'browser-'+phase
assert not(K/directory/'RESULT.json').exists()
env={'PATH':'/opt/pureeos-keynet-20261103-r2/tools/node-current/bin:'+os.environ['PATH'],
 'PLAYWRIGHT_BROWSERS_PATH':'/opt/pureeos-key-history-20261103-r1/tools/browsers',
 'LD_LIBRARY_PATH':'/opt/pureeos-key-history-20261103-r1/tools/browser-libs/usr/lib/x86_64-linux-gnu',
 'FONTCONFIG_FILE':'/opt/pureeos-key-history-20261103-r1/tools/fonts.conf','NODE_OPTIONS':'--max-old-space-size=512'}
revision=json.loads((K/'FRONTEND_RESULT.json').read_text())['source_sha256'][:8]
args=['systemd-run','--no-block','--unit=pureeos-dev-proposals-'+phase+'-'+revision+'-20261104','--property=Type=oneshot','--property=Nice=10',
 '--property=CPUWeight=100','--property=CPUQuota=50%','--property=MemoryHigh=768M','--property=MemoryMax=1G','--property=RuntimeMaxSec=1200',
 '--property=StandardOutput=append:'+str(K/(phase+'-final.log')),'--property=StandardError=append:'+str(K/(phase+'-final.log'))]
for k,v in env.items():args+=['--setenv='+k+'='+v]
script='navigation-check.cjs'if phase.startswith('navigation-')else'browser-check.cjs'
argument=phase.removeprefix('navigation-')if phase.startswith('navigation-')else phase
subprocess.run(args+['node',str(K/script),argument],check=True)
