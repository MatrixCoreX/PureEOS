"""Fresh chain-bound read companions; old databases and statistics never reused."""
from pathlib import Path
import hashlib,json,subprocess
R=Path('/opt/pureeos-devnet-20261105-r1');O=Path('/opt/pureeos-keynet-20261103-r2')
chain=json.loads((R/'BOOTSTRAP_RESULT.json').read_text())['chain_id'];old=json.loads((R/'PLAN.json').read_text())['old_chain_id']
assert not(R/'COMPANIONS.json').exists()and not(R/'companions').exists()
S=R/'companions';S.mkdir()
for p in(O/'companions').glob('*.py'):
 text=p.read_text().replace(str(O),str(R)).replace(old,chain);(S/p.name).write_text(text)
units=[]
for name in ['observed-peak','rotation','votes']:
 old_unit='pureeos-keynet2-companion-'+name+'-20261103.service';unit='pureeos-devnet-companion-'+name+'-20261105.service'
 text=(Path('/etc/systemd/system')/old_unit).read_text().replace(str(O),str(R)).replace(old,chain).replace('pureeos-keynet2-','pureeos-devnet-').replace('-20261103.service','-20261105.service')
 text=text.replace('Nice=10','Nice=10\nCPUQuota=20%\nMemoryMax=256M');(Path('/etc/systemd/system')/unit).write_text(text);units.append(unit)
subprocess.run(['systemctl','daemon-reload'],check=True);subprocess.run(['systemctl','enable','--now',*units],check=True,capture_output=True)
(R/'COMPANIONS.json').write_text(json.dumps({'status':'STARTED','chain_id':chain,'units':units,'source_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest()for p in S.glob('*.py')},'old_databases_reused':False,'scope':'Read-only actual statistics/rotation/votes; no TPS workload'},indent=2)+'\n')
print('NEW_CHAIN_READ_COMPANIONS_STARTED',flush=True)
