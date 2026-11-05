"""Start only isolated tests after a completed build inside the approved window."""
from pathlib import Path
import json,subprocess,time
R=Path('/opt/pureeos-system-signup-fix-20261105-r2')
while True:
 w=json.loads((R/'WINDOW.json').read_text())
 if w['status']!='PAUSED_FOR_BUILD_ONLY' or w['stop_build_unix']-time.time()<=120:raise SystemExit('Preserve restoration time')
 p=R/'build-4/RESULT.json'
 try:b=json.loads(p.read_text())
 except (FileNotFoundError,json.JSONDecodeError):time.sleep(3);continue
 if b['status']=='FAIL':raise SystemExit('Build failure retained; no test launched')
 if b['status']=='BUILT_NOT_TESTED_NOT_DEPLOYED':
  assert not (R/'qualification-1').exists()
  subprocess.run(['systemd-run','--no-block','--unit=pureeos-signup-fix-r2-test-1-20261105','--property=Environment=FIX_MODE=maintenance','--property=Environment=FIX_BUILD_ATTEMPT=4','--property=Environment=FIX_TEST_ATTEMPT=1','--property=CPUQuota=200%','--property=Nice=5','--property=MemoryHigh=1G','--property=MemoryMax=2G','--property=MemorySwapMax=0','--property=OOMScoreAdjust=900','--property=LimitCORE=0','--property=RuntimeMaxSec='+str(min(900,max(1,int(w['stop_build_unix']-time.time()-90)))),'--property=StandardOutput=append:'+str(R/'test-1.log'),'--property=StandardError=append:'+str(R/'test-1.log'),'/usr/bin/python3',str(R/'test.py')],check=True)
  print('Isolated test1 launched from complete build4',flush=True);break
 time.sleep(3)
