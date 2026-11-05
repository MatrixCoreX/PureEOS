"""Use the native checkpoint once, then reopen this new node's own database."""
from pathlib import Path
import os,sys
assert len(sys.argv)==5
name,config,data,snapshot=sys.argv[1:]
root=Path('/opt/pureeos-bp-demo-six-20261105-r1/nodes')/name
assert Path(config)==root/'config'and Path(data)==root/'data-checkpoint'
assert Path(snapshot)==Path('/opt/pureeos-bp-demo-six-20261105-r1/checkpoint/snapshot.bin')
binary='/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/bin/nodeos'
args=[binary,'--config-dir',config,'--data-dir',data]
state=Path(data)/'state'
if not any((state/n).is_file()for n in ['shared_memory.bin','chain_head.dat']):args+=['--snapshot',snapshot]
os.execv(binary,args)
