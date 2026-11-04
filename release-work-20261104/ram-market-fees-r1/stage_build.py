"""Remote-only build. Own VM, new directories, no live service mutation."""
from pathlib import Path
import hashlib, importlib.util, json, subprocess, tarfile

D = Path(__file__).resolve().parent
W = D.parents[1]
spec = importlib.util.spec_from_file_location('cloud', W / 'release-work-20261102/cheap-vps-35bp-history-r1/cloud.py')
c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)
state = c.read()
assert state['instance']['main_ip'] == '66.245.194.132'
S = W / 'PureEOS-Key/system-contracts/eosio.key'
archive = D / 'contract-r1.tar.gz'
with tarfile.open(archive, 'w:gz') as t:
    for p in sorted(S.rglob('*')):
        if p.is_file() and not set(p.relative_to(S).parts) & {'node_modules', 'sdk-tests', 'artifacts', '__pycache__', 'state'}:
            t.add(p, arcname=str(p.relative_to(S)), recursive=False)
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
K = '/opt/pureeos-key-ram-market-20261104-r1'
subprocess.run(c.ssh() + ['mkdir', '-p', K], check=True)
opts = ['-i', state['ssh_key_path'], '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + state['known_hosts']]
subprocess.run(['scp', *opts, str(archive), 'root@66.245.194.132:' + K + '/contract-r1.tar.gz'], check=True)
script = r'''
from pathlib import Path
import hashlib,json,os,shutil,subprocess,tarfile
K=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');S=K/'source'
assert not S.exists(), 'Keep every attempt immutable'
assert hashlib.sha256((K/'contract-r1.tar.gz').read_bytes()).hexdigest()==DIGEST
S.mkdir()
with tarfile.open(K/'contract-r1.tar.gz') as t:t.extractall(S,filter='data')
shutil.copytree(R/'key-service/node_modules',S/'node_modules',symlinks=True)
(S/'sdk-tests').mkdir()
for name in ['pureeos_single_choice_local_test.py','native_savanna_genesis_test.py']:shutil.copy2(R/'harness'/name,S/'sdk-tests'/name)
body=r"""from pathlib import Path
import hashlib,json,os,subprocess,traceback
K=Path('/opt/pureeos-key-ram-market-20261104-r1');S=K/'source';A=K/'artifacts'
try:
 env={**os.environ,'CDT_ROOT':'/opt/pureeos-key-v1-20261103-r1/toolchain/pureeos-cdt-1.0.0','KEY_BUILD_FIXTURES':'1','KEY_UPDATE_ABI':'1'}
 subprocess.run(['bash',str(S/'scripts/build.sh'),str(A)],cwd=S,env=env,check=True)
 files={str(p.relative_to(K)):hashlib.sha256(p.read_bytes()).hexdigest() for p in A.rglob('*') if p.is_file()}
 (K/'BUILD_RESULT.json').write_text(json.dumps({'status':'PASS','files':files,'source_sha256':DIGEST},indent=2)+'\n')
 print('RAM BUILD PASS',flush=True)
except BaseException:
 (K/'BUILD_FAILURE.json').write_text(json.dumps({'error':traceback.format_exc()},indent=2)+'\n');raise
"""
(K/'build.py').write_text(body)
q=subprocess.run(['systemd-run','--no-block','--unit=pureeos-key-ram-build-r1-20261104','--property=Type=oneshot','--property=Nice=19','--property=CPUQuota=50%','--property=CPUWeight=1','--property=IOWeight=10','--property=MemoryHigh=512M','--property=MemoryMax=1536M','--property=OOMScoreAdjust=900','--property=RuntimeMaxSec=1800','--property=StandardOutput=append:'+str(K/'build.log'),'--property=StandardError=append:'+str(K/'build.log'),'python3','-B',str(K/'build.py')],capture_output=True,text=True)
assert q.returncode==0,q.stderr
print(json.dumps({'status':'BUILD_LAUNCHED','source_sha256':DIGEST,'root':str(K)}))
'''.replace('DIGEST', repr(digest))
q = subprocess.run(c.ssh() + ['python3', '-B', '-'], input=script, text=True, capture_output=True)
assert q.returncode == 0, q.stderr
(D / 'BUILD_LAUNCH.json').write_text(q.stdout)
print(q.stdout)
