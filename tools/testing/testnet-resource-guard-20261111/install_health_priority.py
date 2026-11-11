"""Replace only the old resource-sampling timer; no business or native services paused."""
from pathlib import Path
import json,subprocess,datetime
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1')
assert not(R/'HEALTH_PRIORITY_INSTALLED.json').exists()
assert(R/'health_priority_watch.py').is_file()and(R/'REPLACEMENTS_PREPARED.json').is_file()
unit='pureeos-testnet-health-priority-20261111'
service=Path('/etc/systemd/system')/(unit+'.service');timer=service.with_suffix('.timer')
assert not service.exists()and not timer.exists()
service.write_text('[Unit]\nDescription=TEST chain headroom and native duty CPU priority\n[Service]\nType=oneshot\nExecStart=/usr/bin/python3 -B '+str(R/'health_priority_watch.py')+'\nCPUQuota=30%\nMemoryMax=128M\nNice=10\nTimeoutStartSec=90\n')
timer.write_text('[Unit]\nDescription=Bounded10second TEST chain headroom monitor\n[Timer]\nOnBootSec=30s\nOnUnitInactiveSec=10s\nAccuracySec=1s\n[Install]\nWantedBy=timers.target\n')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
subprocess.run(['systemctl','enable','--now',unit+'.timer'],check=True,capture_output=True)
subprocess.run(['systemctl','start',unit+'.service'],check=True,capture_output=True,timeout=90)
old='pureeos-testnet-health-20261110.timer'
# Exact timer name is discovered from the existing health-watch ExecStart, not guessed.
q=subprocess.check_output(['systemctl','list-unit-files','--type=service','--no-legend','--no-pager'],text=True)
retired=[]
for line in q.splitlines():
 name=line.split()[0]
 if'resilience'in name or'health'in name:
  if name==unit+'.service':continue
  cmd=subprocess.check_output(['systemctl','show',name,'-p','ExecStart','--value'],text=True)
  if'/opt/pureeos-testnet-resilience-20261110-r1/health_watch.py'in cmd:
   timername=name.removesuffix('.service')+'.timer'
   subprocess.run(['systemctl','disable','--now',timername],check=True,capture_output=True);retired.append(timername)
x={'status':'PASS_NEW_HEALTH_AND_DUTY_PRIORITY_TIMER','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'new_timer':unit+'.timer','old_resource_timers_retired':retired,'no_BP_or_history_or_business_service_stopped':True,'no_automatic_restarts_or_financial_actions':True}
(R/'HEALTH_PRIORITY_INSTALLED.json').write_text(json.dumps(x,indent=2)+'\n');print(json.dumps(x))
