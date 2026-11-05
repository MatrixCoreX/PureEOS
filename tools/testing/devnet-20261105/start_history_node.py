"""Create one independent, nonproducing SHIP node on the new cheap VM."""
from pathlib import Path
import json
import os
import pwd
import re
import shlex
import subprocess

R = Path('/opt/pureeos-devnet-20261105-r1')
V = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-3')
H = R / 'history'
account = pwd.getpwnam('pureeostest')
(H / 'config').mkdir(parents=True)
(H / 'data').mkdir()
(H / 'state-history').mkdir()
text = (V / 'config/test-low-cost/config.ini').read_text()
text = text.replace('127.0.0.1:8888', '127.0.0.1:8890').replace('127.0.0.1:9876', '127.0.0.1:9877')
for option, value in {'vote-threads': '0', 'read-mode': 'head',
                      'validation-mode': 'full', 'api-accept-transactions': 'false',
                      'http-max-response-time-ms': '500',
                      'abi-serializer-max-time-ms': '500'}.items():
    pattern = r'^' + option + r'\s*=.*$'
    if re.search(pattern, text, re.M):
        text = re.sub(pattern, option + ' = ' + value, text, flags=re.M)
    else:
        text += '\n' + option + ' = ' + value + '\n'
port = json.loads((R / 'FOUNDER_PORTS.json').read_text())['p2p']
text += '\np2p-peer-address = 127.0.0.1:' + str(port)
text += '\np2p-accept-transactions = false\nplugin = eosio::state_history_plugin\ntrace-history = true\nchain-state-history = true\nstate-history-endpoint = 127.0.0.1:8080\nstate-history-dir = ' + str(H / 'state-history') + '\nresource-monitor-space-absolute-gb = 8\n'
(H / 'config/config.ini').write_text(text)
(H / 'config/config.ini').chmod(0o600)
for p in [H, *H.rglob('*')]:
    os.chown(p, account.pw_uid, account.pw_gid)
H.chmod(0o750)
args = [str(V / 'bin/nodeos'), '--config-dir', str(H / 'config'), '--data-dir', str(H / 'data'), '--genesis-json', str(R / 'founder/genesis.json')]
unit = '[Unit]\nDescription=PureEOS TEST nonproducing SHIP history source\nAfter=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory=' + str(H) + '\nExecStart=' + shlex.join(args) + '\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:' + str(H / 'nodeos.log') + '\nStandardError=append:' + str(H / 'nodeos.log') + '\n[Install]\nWantedBy=multi-user.target\n'
name = 'pureeos-devnet-history-20261105.service'
Path('/etc/systemd/system', name).write_text(unit)
subprocess.run(['systemctl', 'daemon-reload'], check=True)
subprocess.run(['systemctl', 'enable', '--now', name], check=True, capture_output=True)
(R / 'HISTORY_NODE_CREATED.json').write_text(json.dumps({'status': 'STARTED', 'unit': name, 'rpc': 8890, 'SHIP': 8080, 'produces': False, 'keys_present': False, 'parallel': True}, indent=2) + '\n')
print('NONPRODUCING_SHIP_HISTORY_STARTED', flush=True)
