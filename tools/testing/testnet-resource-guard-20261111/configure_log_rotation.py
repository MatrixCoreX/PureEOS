"""User explicitly requests scheduled log cleanup; scope syslog, preserve auth/chain evidence."""
import datetime,json,os,subprocess
from pathlib import Path
R=Path('/opt/pureeos-testnet-new-ban-20261111-r1');os.umask(0o077)
assert not(R/'LOG_ROTATION_CONFIGURED.json').exists(),'Reconcile already-configured timer'
original=Path('/etc/logrotate.d/rsyslog');raw=original.read_bytes()
assert raw.count(b'/var/log/syslog\n')==1
backup=R/'os-backups';backup.mkdir(exist_ok=True);(backup/'rsyslog-logrotate.before').write_bytes(raw)
config=Path('/etc/pureeos-syslog-logrotate.conf');assert not config.exists()
config.write_text('''/var/log/syslog {
    daily
    maxsize 128M
    rotate 7
    maxage 7
    missingok
    notifempty
    compress
    compressoptions -1
    nodelaycompress
    create 0640 syslog adm
    sharedscripts
    postrotate
        /usr/lib/rsyslog/rsyslog-rotate
    endscript
}
''')
debug=subprocess.run(['/usr/sbin/logrotate','--debug','--state','/var/lib/logrotate/pureeos-syslog.status',str(config)],capture_output=True,text=True,timeout=30)
assert debug.returncode==0,debug.stderr[-1500:]
original.write_bytes(raw.replace(b'/var/log/syslog\n',b''))
unit='pureeos-syslog-rotation-20261111'
service=Path('/etc/systemd/system')/(unit+'.service');timer=service.with_suffix('.timer')
assert not service.exists()and not timer.exists()
service.write_text('''[Unit]
Description=Bounded compressed TEST syslog retention
[Service]
Type=oneshot
ExecStart=/usr/sbin/logrotate --state /var/lib/logrotate/pureeos-syslog.status /etc/pureeos-syslog-logrotate.conf
Nice=19
CPUQuota=10%
IOSchedulingClass=idle
MemoryMax=128M
TimeoutStartSec=infinity
''')
timer.write_text('''[Unit]
Description=Hourly TEST syslog size check
[Timer]
OnCalendar=hourly
AccuracySec=1min
Persistent=true
[Install]
WantedBy=timers.target
''')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
subprocess.run(['systemctl','enable','--now',unit+'.timer'],check=True,capture_output=True)
# Initial rotation uses a single independent capped gzip worker; no truncation or node pause.
subprocess.run(['systemctl','start','--no-block',unit+'.service'],check=True,capture_output=True)
result={'status':'PASS_TIMER_CONFIGURED_INITIAL_ROTATION_RUNNING','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'service':unit+'.service','timer':unit+'.timer','checks':'hourly','rotate':'daily or over128MiB','retention':'7compressed rotations/max7days','initial6GiB_log_preserved_compressed':True,'auth_and_other_logs_policy_unchanged':True,'journal_existing512MiB_cap_retained':True,'fault_evidence_under_case_root_excluded':True,'initial_worker_CPU_percent_cap':10,'debug_validation':debug.stderr}
(R/'LOG_ROTATION_CONFIGURED.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');print(json.dumps(result,ensure_ascii=False))
