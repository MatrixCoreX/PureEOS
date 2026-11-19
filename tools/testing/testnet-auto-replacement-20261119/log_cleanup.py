"""Bounded logs only. Never traverses node databases, keys, snapshots or case evidence."""
from pathlib import Path
import subprocess,json,datetime,os,fcntl
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077);lock=(R/'log-cleanup.lock').open('a');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
commands=[['/usr/sbin/logrotate','--state','/var/lib/logrotate/pureeos-syslog.status','/etc/pureeos-stability-logs-20261116.conf'],['/usr/bin/journalctl','--vacuum-time=3d','--vacuum-size=256M']];before=os.statvfs('/');results=[]
for a in commands:
 p=subprocess.run(a,capture_output=True,text=True,timeout=1800);results.append({'program':a[0],'rc':p.returncode,'out':p.stdout[-1500:],'err':p.stderr[-2500:]});assert p.returncode==0,results
out={'status':'PASS_BOUNDED_LOG_CLEANUP','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'results':results,'syslog_maxsize_MiB':64,'syslog_compressed_rotations':5,'max_age_days':3,'checks_minutes':15,'journal_archived_budget_MiB':256,'existing_journald_512MiB_global_cap_unchanged':True,'filesystem_available_delta_bytes':(os.statvfs('/').f_bavail-before.f_bavail)*before.f_frsize,'scope':'Only syslog rotation and archived journal vacuum. Currentnodeblocks/keys/configs/history/uniqueevidence untouched.'};p=R/'LOG_CLEANUP_LATEST.json';t=p.with_suffix('.tmp');t.write_text(json.dumps(out,indent=2)+'\n');os.replace(t,p);print(json.dumps(out))
