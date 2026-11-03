#!/usr/bin/env python3
"""Check the SDK runtime/tests before starting a disposable contract chain."""
import os,subprocess,sys
from pathlib import Path
source=Path(__file__).resolve().parents[1]
version=subprocess.check_output(['node','--version'],text=True).strip()
parts=tuple(int(x)for x in version.lstrip('v').split('.'))
if parts<(22,12,0):raise SystemExit('SDK/relay tests require Node.js >=22.12.0; use an independent runtime PATH')
print('QUALIFICATION NODE '+version,flush=True)
if '--artifacts'in sys.argv:
    os.environ['KEY_ABI']=str(Path(sys.argv[sys.argv.index('--artifacts')+1])/'eosio.key.abi')
result=subprocess.run(['node','--test',*[str(p)for p in sorted((source/'tests').glob('*.test.mjs'))]],cwd=source)
if result.returncode:raise SystemExit(result.returncode)
from chain_test import main
raise SystemExit(main())
