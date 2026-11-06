"""Once-only new owned public QA after reviewed irreversible activation."""
from pathlib import Path
import datetime,json,os,subprocess,time,traceback
import ops_common as o
R=o.R
try:
 assert not(R/'POST_QA_WATCHER_STARTED.json').exists();o.save(R/'POST_QA_WATCHER_STARTED.json',{'status':'WAITING_REVIEWED_UPGRADE_AND_APPS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()})
 deadline=time.monotonic()+14400
 while not(R/'GOVERNED_UPGRADE_AND_APPS.json').exists():
  assert time.monotonic()<deadline and not(R/'UPGRADE_WAIT_FAILURE.json').exists(),'Upgrade incomplete; no payment';time.sleep(20)
 assert json.loads((R/'GOVERNED_UPGRADE_AND_APPS.json').read_text())['status']=='PASS'
 assert not(R/'PUBLIC_QA_PROGRESS.json').exists(),'Reconcile existing QA; never replay'
 assert o.sha((R/'controllers/public_qa.py').read_bytes())==json.loads((R/'OPERATIONAL_QA_ADDENDUM.json').read_text())['new_SHA256']
 o.guard();o.require_codes(o.NEW);o.authority_guard()
 with(R/'public-qa.log').open('w')as log:subprocess.run(['python3','-B',str(R/'controllers/public_qa.py')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=1800)
 assert json.loads((R/'PUBLIC_QA_RESULT.json').read_text())['status']=='PASS'
 with(R/'final-audit.log').open('w')as log:subprocess.run(['python3','-B',str(R/'controllers/final_audit.py')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
 o.save(R/'POST_QA_AND_AUDIT.json',{'status':'PASS','QA_result_SHA256':o.sha((R/'PUBLIC_QA_RESULT.json').read_bytes()),'final_audit_SHA256':o.sha((R/'FINAL_AUDIT.json').read_bytes()),'screenshots_and_Git_finalization':'Still separate; no complete claim yet'})
except BaseException:
 o.save(R/'POST_QA_FAILURE.json',{'status':'FAIL','error':traceback.format_exc(),'never_repeat_signed_packet_or_previous_cases':True});raise
