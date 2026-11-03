"""Use the existing read-only companions with fresh chain-bound state."""
from pathlib import Path
import hashlib, json, subprocess

R = Path('/opt/pureeos-keynet-20261103-r1')
O = Path('/opt/pureeos-test45-20261102-r1')
old_id = json.loads((R/'PLAN.json').read_text())['old_chain_id']
chain_id = json.loads((R/'BOOTSTRAP_RESULT.json').read_text())['chain_id']
source = R/'companions'
source.mkdir(exist_ok=True)
for p in (O/'companions').glob('*.py'):
    text = p.read_text().replace(str(O),str(R)).replace(old_id,chain_id)
    text = text.replace('port=51003','port=8888').replace('127.0.0.1:51003','127.0.0.1:8888')
    (source/p.name).write_text(text)
units = []
for name in ['observed-peak','rotation','votes']:
    old_unit = 'pureeos-test45-companion-' + name + '-20261102.service'
    new_unit = 'pureeos-keynet-companion-' + name + '-20261103.service'
    text = (Path('/etc/systemd/system')/old_unit).read_text()
    text = text.replace(str(O),str(R)).replace(old_id,chain_id)
    text = text.replace('pureeos-test45-','pureeos-keynet-').replace('-20261102.service','-20261103.service')
    text = text.replace('127.0.0.1:51003','127.0.0.1:8888')
    (Path('/etc/systemd/system')/new_unit).write_text(text)
    units.append(new_unit)
subprocess.run(['systemctl','daemon-reload'],check=True)
subprocess.run(['systemctl','enable','--now',*units],check=True,capture_output=True)
(R/'COMPANIONS.json').write_text(json.dumps({'status':'STARTED','chain_id':chain_id,'units':units,
    'source_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest()for p in source.glob('*.py')},
    'old_statistics_or_rotation_databases_reused':False},indent=2)+'\n')
print('NEW_CHAIN_READONLY_COMPANIONS_STARTED',flush=True)
