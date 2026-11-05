"""Stage isolated public source overlays and a bounded build on retained host."""
from pathlib import Path
import base64,json,shlex,subprocess,sys
W=Path(__file__).resolve().parents[2];D=Path(__file__).resolve().parent
sys.path.insert(0,str(W/'release-work-20261105/system-signup-r1'));from remote import host
repo=W/'PureEOS-System-Accounts-20261105'
names=['libraries/chain/include/eosio/chain/system_account_proposal.hpp','libraries/chain/system_account_proposal.cpp','libraries/chain/controller.cpp','unittests/pureeos_system_account_tests.inc','unittests/pureeos_governance_tests.cpp','unittests/pureeos_signup_real_tests.cpp','tools/governance/system_account_bundle.py']
payload={'files':{n:base64.b64encode((repo/n).read_bytes()).decode() for n in names},
 'base':{n:base64.b64encode(subprocess.check_output(['git','-C',str(repo),'show','d64479756e8b5ea62c4281435bc32c67d443ace4:'+n])).decode() for n in names if not n.startswith('tools/')},
 'build':base64.b64encode((D/'build.py').read_bytes()).decode()}
code=r'''
from pathlib import Path
import base64,json,sys,hashlib,subprocess,datetime,urllib.request
r=Path('/opt/pureeos-system-signup-fix-20261105-r2');old=Path('/opt/pureeos-system-signup-20261105-r1/native-r1');d=json.load(sys.stdin)
assert not r.exists(),'Keep each staging attempt immutable'
assert int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))>=1572864,'Insufficient live headroom; do not pause nodes'
for n,value in d['base'].items():assert (old/'source'/n).read_bytes()==base64.b64decode(value),'Unexpected base source '+n
r.mkdir(mode=0o700)
subprocess.run(['cp','-a','--reflink=auto',str(old/'source'),str(r/'source')],check=True)
subprocess.run(['cp','-al',str(old/'cache'),str(r/'cache')],check=True)
subprocess.run(['cp','-a','--reflink=auto',str(old/'real-contracts'),str(r/'real-contracts')],check=True)
hashes={}
for name,value in d['files'].items():
 p=r/'source'/name;data=base64.b64decode(value);p.write_bytes(data);hashes[name]=hashlib.sha256(data).hexdigest()
(r/'SOURCE_HASHES.json').write_text(json.dumps(hashes,indent=2)+'\n');(r/'build.py').write_bytes(base64.b64decode(d['build']))
def rpc(method):
 q=urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=b'{}',headers={'Content-Type':'application/json'})
 return json.load(urllib.request.urlopen(q,timeout=10))
prior=json.loads(Path('/opt/pureeos-system-signup-20261105-r1/POST_RESTORE_VERIFICATION.json').read_text());pids={}
for unit,oldpid in prior['node_pids'].items():
 pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True));assert pid==oldpid,unit;pids[unit]=pid
info=rpc('get_info');assert info['chain_id']==prior['chain_id'];age=(datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)-datetime.datetime.fromisoformat(info['head_block_time'])).total_seconds()
baseline={'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'node_pids':pids,'head_age_seconds':age,'head_LIB_gap':info['head_block_num']-info['last_irreversible_block_num'],'info':info,'upgrade':rpc('get_upgrade_state'),'caps':rpc('get_consensus_parameters'),'finalizers':rpc('get_finalizer_info')}
(r/'LIVE_BASELINE.json').write_text(json.dumps(baseline,indent=2)+'\n')
subprocess.run(['systemd-run','--no-block','--unit=pureeos-signup-fix-r2-build-1-20261105','--property=CPUQuota=50%','--property=Nice=10','--property=MemoryHigh=1G','--property=MemoryMax=1280M','--property=MemorySwapMax=0','--property=OOMScoreAdjust=900','--property=OOMPolicy=continue','--property=LimitCORE=0','--property=RuntimeMaxSec=2450','--property=StandardOutput=append:'+str(r/'build-1.log'),'--property=StandardError=append:'+str(r/'build-1.log'),'/usr/bin/python3',str(r/'build.py')],check=True)
print(json.dumps({'status':'BOUNDED_REMOTE_FIX_BUILD_STARTED','source_files':hashes,'node_pids_preserved':True,'head_age_seconds':age,'head_LIB_gap':baseline['head_LIB_gap'],'no_chain_pause_or_install':True}))
'''
r=subprocess.run(host.ssh()+['python3','-c',shlex.quote(code)],input=json.dumps(payload),text=True,capture_output=True)
print(r.stdout);print(r.stderr[-2000:]);raise SystemExit(r.returncode)
