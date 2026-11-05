"""Wait for this window's restoration, then run read-only audits only."""
from pathlib import Path
import datetime,json,subprocess,time
R=Path('/opt/pureeos-system-signup-fix-20261105-r2')
while True:
 w=json.loads((R/'WINDOW.json').read_text())
 assert time.time()<w['window_end_unix']-60,'Restore-audit deadline; inspect guardian'
 if w['status']=='RESTORED_VERIFIED':break
 time.sleep(5)
subprocess.run(['/usr/bin/python3',str(R/'verify_restore.py')],check=True)
deadline=min(time.time()+720,w['window_end_unix']-60)
while True:
 result=subprocess.run(['/usr/bin/python3',str(R/'check_origin.py')])
 if result.returncode==0:break
 assert time.time()<deadline,'Read-only origin recovery audit needs inspection'
 time.sleep(25)
print('Original-program, native-state and TLS-origin audits passed',flush=True)
