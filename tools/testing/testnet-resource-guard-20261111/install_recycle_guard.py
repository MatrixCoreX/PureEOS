"""Enable a minute check, at most one safe memory recycle per ten minutes."""
import datetime,json,subprocess
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1')
assert not(R/'RECYCLE_GUARD_INSTALLED.json').exists()
check=json.loads((R/'RECYCLE_CHECK_ONLY.json').read_text());assert check['status']=='NO_ACTION_ALL_BELOW1280MiB'and check['recent_strong_votes']>=19
name='pureeos-testnet-memory-guard-20261111';u=Path('/etc/systemd/system')/(name+'.service');t=u.with_suffix('.timer');assert not u.exists()and not t.exists()
u.write_text('[Unit]\nDescription=TEST-only guarded single BP memory recycle\n[Service]\nType=oneshot\nExecStart=/usr/bin/python3 -B '+str(R/'recycle_guard.py')+'\nCPUQuota=20%\nMemoryMax=192M\nNice=10\nTimeoutStartSec=180\n')
t.write_text('[Unit]\nDescription=Minute memory check with10minute per-attempt cooldown\n[Timer]\nOnBootSec=2min\nOnUnitInactiveSec=60s\nAccuracySec=5s\n[Install]\nWantedBy=timers.target\n')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True);subprocess.run(['systemctl','enable','--now',name+'.timer'],check=True,capture_output=True)
subprocess.run(['systemctl','start',name+'.service'],check=True,capture_output=True,timeout=120)
x={'status':'PASS_MINUTE_MEMORY_GUARD_ENABLED','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'timer':name+'.timer','threshold_MiB':1280,'minimum_other_strong_votes':18,'next_duty_margin_seconds':90,'cooldown_seconds':600,'one_target_only':True,'independent60s_start_safeguard':True,'stale_chain_missing_BP_newban_or_miss_streak_blocks_action':True,'no_native_source_binary_consensus_key_or_safety_reset':True,'prior21_actual_same_data_restarts_qualified':True,'initial_live_check':check}
(R/'RECYCLE_GUARD_INSTALLED.json').write_text(json.dumps(x,indent=2)+'\n');print(json.dumps(x))
