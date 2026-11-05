"""Read-only recovery observation; never restart processes or submit payments."""
from pathlib import Path
import datetime,json,subprocess,time
R=Path('/opt/pureeos-system-signup-fix-20261105-r2');start=time.time()
while True:
 w=json.loads((R/'WINDOW.json').read_text())
 assert w['status']=='RESTORED_VERIFIED'
 assert time.time()<min(start+600,w['window_end_unix']-60)
 result=subprocess.run(['/usr/bin/python3',str(R/'check_origin.py')],stdout=subprocess.DEVNULL)
 if result.returncode==0:
  o=json.loads((R/'POST_RESTORE_ORIGIN.json').read_text())
  a=json.loads(Path('/opt/pureeos-keynet-20261103-r2/activity/STATE.json').read_text())
  b=json.loads((R/'BEFORE_ACTIVITY.json').read_text())
  gap=o['head']-int(o['indexed_block'])
  if 'observed_index_progress' in o and gap<=64 and a['completed_batches']>b['completed_batches'] and a['business_transactions']>b['business_transactions']:
   value={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'scope':'Read-only observations of original ordinary/indexer controllers; no synthetic payment or restart',
    'before_batches':b['completed_batches'],'after_batches':a['completed_batches'],
    'before_transactions':b['business_transactions'],'after_transactions':a['business_transactions'],
    'ordinary_has_pending':a.get('pending') is not None,'history_index_progress':o['observed_index_progress'],
    'history_indexed_block':o['indexed_block'],'observed_head':o['head'],'observed_head_index_gap':gap}
   (R/'POST_RESTORE_PROGRESS.json').write_text(json.dumps(value,indent=2)+'\n');print(json.dumps(value),flush=True);break
 time.sleep(20)
