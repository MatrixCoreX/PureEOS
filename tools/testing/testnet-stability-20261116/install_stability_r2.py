from pathlib import Path
import os,json,subprocess,hashlib,datetime,sys
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');os.umask(0o077);sys.path.insert(0,str(R));import stability_guard as g
assert not(R/'STABILITY_INSTALL_STARTED.json').exists();assert json.loads((R/'REPLACEMENT_ENROLL_RESULT.json').read_text())['status']=='PASS_6_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS';nodes=json.loads((R/'LIVE_NODES.json').read_text());plan=json.loads((R/'REPLACEMENTS_PREPARED.json').read_text())
for v in plan['nodes']:nodes[v['name']]={**v,'config_dir':str(R/'nodes'/v['name']/'config'),'data_dir':str(R/'nodes'/v['name']/'data')}
assert len(nodes)==25
for n,v in nodes.items():
 v['config_sha256']=g.sha(Path(v['config_dir'])/'config.ini');pid=g.state(v['unit'])['MainPID'];assert pid!='0';g.assert_native_binding(v,pid);v['safety_present_at_install']=(Path(v['data_dir'])/'finalizers/safety.dat').is_file();assert any((Path(v['data_dir'])/'state'/x).is_file()for x in ['shared_memory.bin','chain_head.dat'])
initial=json.loads((R/'REPLACEMENT_LIVE_BASELINE.json').read_text())['banned'];g.save('GUARD_NODE_MAP.json',nodes);g.save('GUARD_CONFIG.json',{'initial_banned':initial,'policy_tests':30,'native_chain_rules_unchanged':True,'scope':'25whitelisted BP identities; no financial or native source actions.'});g.save('STABILITY_INSTALL_STARTED.json',{'utc':g.now(),'node_map_sha256':g.sha(R/'GUARD_NODE_MAP.json')})
# New qualified guardian replaces completed historical monitor/recycle policy.
for timer in ['pureeos-testnet-memory-guard-20261111.timer','pureeos-testnet-health-priority-20261111.timer']:
 subprocess.run(['systemctl','disable','--now',timer],check=True,capture_output=True)
# Give the minimum-worker history pipeline a realistic soft working set.
# No restart, queue deletion, DB change or hard memory kill limit.
u='pureeos-devnet-purehistory-indexer-20261105.service';d=Path('/etc/systemd/system')/(u+'.d');d.mkdir(exist_ok=True);p=d/'98-stability-headroom-20261116.conf';assert not p.exists();p.write_text('[Service]\nMemoryHigh=640M\n')
subprocess.run(['systemctl','set-property','--runtime',u,'MemoryHigh=640M'],check=True,capture_output=True)
for v in nodes.values():
 d=Path('/etc/systemd/system')/(v['unit']+'.d');d.mkdir(exist_ok=True);p=d/'97-stability-log-rate-20261116.conf';assert not p.exists();p.write_text('[Service]\nLogRateLimitIntervalSec=30s\nLogRateLimitBurst=100\n')
base=Path('/etc/systemd/system');guard='pureeos-stability-memory-20261116';clean='pureeos-stability-logs-20261116'
for name,command,description,quota in [(guard,'/usr/bin/python3 -B '+str(R/'stability_guard.py'),'TEST guarded same-data memory recycling',''),(clean,'/usr/bin/python3 -B '+str(R/'log_cleanup.py'),'TEST bounded log retention','CPUQuota=5%\nIOSchedulingClass=idle\n')]:
 p=base/(name+'.service');assert not p.exists();p.write_text('[Unit]\nDescription='+description+'\n[Service]\nType=oneshot\nExecStart='+command+'\nNice=19\nCPUWeight=50\nIOWeight=25\nMemoryHigh=96M\nMemoryMax=192M\nTimeoutStartSec=infinity\n'+quota)
 period='30s'if name==guard else'15min';t=base/(name+'.timer');assert not t.exists();t.write_text('[Unit]\nDescription='+description+' periodic check\n[Timer]\nOnBootSec=2min\nOnUnitInactiveSec='+period+'\nAccuracySec=2s\nPersistent=true\n[Install]\nWantedBy=timers.target\n')
config=Path('/etc/pureeos-stability-logs-20261116.conf');assert not config.exists();config.write_text('/var/log/syslog {\n    su root syslog\n    daily\n    maxsize 64M\n    rotate 5\n    maxage 3\n    missingok\n    notifempty\n    compress\n    compressoptions -1\n    nodelaycompress\n    create 0640 syslog adm\n    sharedscripts\n    postrotate\n        /usr/lib/rsyslog/rsyslog-rotate\n    endscript\n}\n')
debug=subprocess.run(['/usr/sbin/logrotate','--debug','--state','/var/lib/logrotate/pureeos-syslog.status',str(config)],capture_output=True,text=True,timeout=20);assert debug.returncode==0,debug.stderr
subprocess.run(['systemctl','disable','--now','pureeos-syslog-rotation-20261111.timer'],check=True,capture_output=True)
oldstate=subprocess.check_output(['systemctl','show','pureeos-syslog-rotation-20261111.service','-p','ActiveState','--value'],text=True).strip();assert oldstate=='inactive','Do not race an existing log compression job'
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
# Exercise the exact live production decision path before scheduling writes.
p=subprocess.run(['/usr/bin/python3','-B',str(R/'stability_guard.py'),'--check-only'],capture_output=True,text=True,timeout=30);g.save('GUARD_LIVE_CHECK.json',{'rc':p.returncode,'out':p.stdout,'err':p.stderr});assert p.returncode==0,p.stderr;latest=json.loads((R/'GUARD_LATEST.json').read_text());assert latest['status']in ['NO_SAFE_OVERSIZED_TARGET','CHECK_ONLY_WOULD_RECYCLE_ONE','COOLDOWN'],latest
for name in [guard,clean]:subprocess.run(['systemctl','enable','--now',name+'.timer'],check=True,capture_output=True)
result={'status':'PASS_STABILITY_GUARD_AND_LOG_TIMERS_INSTALLED','utc':g.now(),'guardian':'768MiBRSS+Swap/512MiBemergency; atmost1BP/180sec; native21/15+19freshstrong+18other+90/110secdutywindow; independent30secstartwatchdog; positivecanonicalreconciliationaftertransienterrors; no dirtyrepair/reset','logs':'15mincheck;64MiBmaxsize/5gzrotations/3days; archivedjournalvacuum256MiB+3days; CPU5percentcleanupworker','history_indexer':'Existing1reader/1deserializer/1indexingqueue retained; MemoryHigh256→640MiB only, no processrestart or hardkill cap','old_timers_disabled':True,'live_check':latest,'node_map_sha256':g.sha(R/'GUARD_NODE_MAP.json'),'private_keys_and_safety_server_only':True};g.save('STABILITY_INSTALL_RESULT.json',result);print(json.dumps(result))
