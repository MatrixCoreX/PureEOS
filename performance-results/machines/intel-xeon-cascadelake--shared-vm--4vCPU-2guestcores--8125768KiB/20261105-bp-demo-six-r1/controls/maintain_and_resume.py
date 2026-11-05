"""Run the newly requested immediate maintenance, then resume only the waiting demo."""
from common import *
import traceback

try:
    assert not(R/'DEMO_PROGRESS.json').exists()and not(R/'STOP_RECEIPT.json').exists()
    subprocess.run(['/usr/bin/python3','-u',str(R/'maintain_now.py')],check=True,timeout=1200)
    assert read('MAINTENANCE_REQUEST.json')['status']=='PASS'
    unit='pureeos-bp-demo-six-campaign-r5-20261105.service'
    assert state(unit)['ActiveState']in['inactive','failed']
    subprocess.run(['systemd-run','--unit='+unit,'--property=CPUQuota=20%','--property=MemoryMax=256M','--property=Nice=15','--property=RuntimeMaxSec=8h','--property=StandardOutput=append:'+str(R/'campaign-r5.log'),'--property=StandardError=append:'+str(R/'campaign-r5.log'),'/usr/bin/python3','-u',str(R/'demonstrate.py')],check=True,capture_output=True)
    save('IMMEDIATE_MAINTENANCE_CONTROLLER_RESULT.json',{'status':'PASS','UTC':now(),'chain_id':CHAIN,'user_instruction':'第六个直接挂维护吧','maintenance_request_irreversible':True,'maintenance_effective':(R/'MAINTENANCE_EFFECTIVE.json').exists(),'resumed_campaign_unit':unit,'phase_scope':'Only the waiting controller was replaced. No original node was stopped, no successful enrollment or maintenance transaction was replayed. Fault-five gates continue without a sixth-node production prerequisite.'})
    print('IMMEDIATE_MAINTENANCE_AND_BACKGROUND_RESUME_PASS',flush=True)
except BaseException:
    save('IMMEDIATE_MAINTENANCE_CONTROLLER_FAILURE.json',{'status':'FAIL','UTC':now(),'error':traceback.format_exc()});raise
