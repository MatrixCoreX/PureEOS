"""Open only a prepared replacement's own native database, checkpoint once."""
import json
import os
from pathlib import Path
import sys

R=Path('/opt/pureeos-testnet-new-ban-20261111-r1')
assert len(sys.argv)==2
name=sys.argv[1]
nodes={x['name']:x for x in json.loads((R/'REPLACEMENTS_PREPARED.json').read_text())['nodes']}
assert name in nodes
root=R/'nodes'/name;data=root/'data';config=root/'config'
assert data.is_dir() and config.is_dir()
binary='/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/bin/nodeos'
args=[binary,'--config-dir',str(config),'--data-dir',str(data)]
if not any((data/'state'/n).is_file() for n in ['shared_memory.bin','chain_head.dat']):
    args+=['--snapshot',str(R/'replacement-checkpoint.bin')]
os.execv(binary,args)
