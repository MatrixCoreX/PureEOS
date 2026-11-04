from pathlib import Path
import subprocess,os
K=Path('/opt/pureeos-dev-proposals-20261104-r1');node='/opt/pureeos-keynet-20261103-r2/tools/node-current/bin/node'
commands=[[node,str(K/'live-smoke.mjs')],[node,str(K/'history-audit.mjs'),'https://aic-test.matrixai.one/history','after'],['python3','-B',str(K/'live_audit.py'),'after'],['python3','-B',str(K/'launch_browser.py'),'public-after']]
for command in commands:subprocess.run(command,check=True)
