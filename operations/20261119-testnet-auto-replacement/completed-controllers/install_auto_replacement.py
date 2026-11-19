from pathlib import Path
import os,json,hashlib,fcntl,subprocess,sys,pwd
R=Path('/opt/pureeos-testnet-cpu-stability-20261119-r1');G=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');C=Path('/opt/pureeos-devnet-20261105-r1');os.umask(0o077);sys.path.insert(0,str(R));sys.path.insert(0,str(G));import stability_guard as g
assert not(R/'AUTO_INSTALL_STARTED.json').exists();roll=json.loads((R/'VOTE_ONE_ROLL_RESULT.json').read_text());assert roll['status']=='PASS_ALL23_ONE_VOTE_THREAD_SAME_DATA';assert len(roll['completed'])==23
lock=(G/'native-operation.lock').open('a')
for attempt in range(30):
 try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB);break
 except BlockingIOError:__import__('time').sleep(1)
else:raise RuntimeError('Existing native operation still active')
stage=json.loads((R/'AUTO_SOURCE_STAGE_RESULT.json').read_text());assert stage['status'].startswith('PASS_')
for n,h in stage['source_SHA256'].items():assert hashlib.sha256((R/n).read_bytes()).hexdigest()==h,n
pins={'contracts':g.load('REPLACEMENT_LIVE_BASELINE.json')['code_ABI_pins'],'tools':{str(C/'tools-runtime/programs'/p/p):hashlib.sha256((C/'tools-runtime/programs'/p/p).read_bytes()).hexdigest()for p in ['cleos','helios-util','keosd']}}
for n,pin in pins['contracts'].items():
 actual=g.rpc('get_raw_abi',{'account_name':n},port=8890);assert all(actual[k]==h for k,h in pin.items())
(R/'AUTO_TRUST_PINS.json').write_text(json.dumps(pins,indent=2)+'\n')
config={'enabled':True,'bank':'civicbankers','target_BPs':25,'fixed_code_caps':{'funding_AIC_per_node':'14000.0000','RAM_AIC_per_node':'1.0000','bank_reserve_AIC':'500.0000','per_hour':1,'per_day':2,'lifetime_nodes':10,'lifetime_max_bank_debit_AIC':'140010.0000'},'CPU_pressure_avg60_new_job_max':80,'no_unowned_votes_or_governed_treasury':True,'signed_packet_broadcasts':1,'interrupted_signing_or_ambiguous_delivery':'Original ID reconciliation only; no automatic re-sign/rebroadcast'}
(R/'AUTO_REPLACEMENT_CONFIG.json').write_text(json.dumps(config,indent=2)+'\n')
nodes=g.load('GUARD_NODE_MAP.json');template=Path(nodes['anthropiclab']['config_dir']);raw=(template/'config.ini').read_text();identity={'producer-name','signature-provider','peer-private-key','p2p-bp-gossip-endpoint','p2p-peer-address','p2p-server-address','http-server-address','p2p-listen-endpoint','agent-name','enable-stale-production','pause-on-startup','max-reversible-blocks'};public='\n'.join(line for line in raw.splitlines()if'='not in line or line.split('=',1)[0].strip()not in identity);assert 'vote-threads = 1'in public and 'validation-mode = full'in public;assert not __import__('re').search(r'PVT_(?:K1|R1|BLS)_[A-Za-z0-9_-]{25,}',public);assert not __import__('re').search(r'\b5[HJK][1-9A-HJ-NP-Za-km-z]{49}\b',public)
(R/'AUTO_RUNTIME_TEMPLATE.json').write_text(json.dumps({'config':public,'logging':(template/'logging.json').read_text(),'source_SHA256':g.sha(template/'config.ini'),'source':'Qualified current owned full-validation BP, identity and key directives removed'},indent=2)+'\n')
from replacement_chain import ChainAdapter
from replacement_policy import admission
from auto_replacement import run,spending
with ChainAdapter(R)as adapter:
 adapter.trust_checks(retire=False);context,retired=adapter.context();preview=admission(context,spending([]))
assert context['eligible']==23 and len(retired)==2
record={'utc':g.now(),'preview':preview,'context':context,'source_SHA256':stage['source_SHA256'],'tests':{'guardian':50,'replacement':42},'no_existing_signer_or_phone_access':True};(R/'AUTO_INSTALL_STARTED.json').write_text(json.dumps(record,indent=2)+'\n')
# Execute the real retirement/admission path now. Funding is still subject to
# the same code-enforced balance, CPU, daily and lifetime limits.
with ChainAdapter(R)as adapter:live=run(adapter)
(R/'AUTO_REPLACEMENT_LATEST.json').write_text(json.dumps(live,indent=2)+'\n')
unit='pureeos-auto-replacement-20261119';base=Path('/etc/systemd/system');service=base/(unit+'.service');timer=base/(unit+'.timer');assert not service.exists()and not timer.exists()
service.write_text('[Unit]\nDescription=Owned TEST permanent-ban retirement and bounded similar-name replacement\n[Service]\nType=oneshot\nUser=root\nGroup=root\nWorkingDirectory='+str(R)+'\nExecStart=/usr/bin/python3 -B '+str(R/'auto_replacement.py')+'\nUMask=0077\nNice=19\nCPUWeight=25\nCPUQuota=25%\nIOWeight=25\nIOSchedulingClass=idle\nMemoryHigh=192M\nMemoryMax=384M\nTimeoutStartSec=180\nKillMode=control-group\nLimitCORE=0\nNoNewPrivileges=true\n')
timer.write_text('[Unit]\nDescription=Check owned TEST BP replacement queue\n[Timer]\nOnBootSec=3min\nOnUnitInactiveSec=3min\nAccuracySec=5s\nPersistent=true\n[Install]\nWantedBy=timers.target\n')
subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True,timeout=15);subprocess.run(['systemctl','enable','--now',unit+'.timer'],check=True,capture_output=True,timeout=15);record.update(status='PASS_AUTOMATIC_RETIREMENT_AND_REPLACEMENT_TIMER_INSTALLED',utc=g.now(),first_production_path=live,timer=g.state(unit+'.timer'),retired_unit_config_receipts=[json.loads(p.read_text())for p in sorted((R/'retired-unit-configs').glob('*.json'))],private_job_count=len(list((R/'jobs').glob('*/STATE.json'))));(R/'AUTO_INSTALL_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');fcntl.flock(lock,fcntl.LOCK_UN);print(json.dumps(record))
