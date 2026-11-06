"""One-time preparation, native checkpoint, and two-node enrollment coordinator."""
from common import *
import traceback
try:
 assert not(R/'RUN_PROGRESS.json').exists(),'Reconcile existing canonical stages before any resume'
 for script,phase in [('prepare.py','PREPARING'),('create_snapshot.py','EXPORTING_NATIVE_CHECKPOINT'),('join.py','ENROLLING_TWO_NEW_NODES')]:
  save('RUN_PROGRESS.json',{'status':phase,'UTC':now(),'script':script})
  subprocess.run(['/usr/bin/python3','-u',str(R/script)],check=True,cwd=R)
 save('RUN_PROGRESS.json',{'status':'PASS','UTC':now(),'scope':'Owned two-node start and stop-two task completed; native duty remains governed by FIFO.'})
except BaseException:
 save('RUN_FAILURE.json',{'status':'NEEDS_RECONCILIATION','UTC':now(),'error':traceback.format_exc()});raise
