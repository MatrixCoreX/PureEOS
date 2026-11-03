"""Runs on the cheap TEST VM; join only the explicit selected stage."""
from pathlib import Path
import hashlib
import json
import os
import pwd
import shlex
import subprocess
import sys
import time
import traceback
import urllib.request

from campaign_helpers import R, V, load_chain, ensure_bank, import_key, action, permission, transfer

os.umask(0o077)
target = int(sys.argv[1])
assert target == 25
f = None
report = {'status': 'JOINING', 'target_BPs': target, 'joined': [], 'TPS': 'explicitly excluded'}
output = R / ('JOIN_STAGE' + str(target) + '.json')


def save():
    output.write_text(json.dumps(report, indent=2) + '\n')


def rpc(port, method, data=None):
    req = urllib.request.Request('http://127.0.0.1:' + str(port) + '/v1/chain/' + method,
        json.dumps(data or {}).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=8) as response:
        return json.load(response)


def start_bp(entry, keys):
    root = R / 'bps' / entry['name']
    account = pwd.getpwnam('pureeostest')
    unit = 'pureeos-keynet-bp-' + entry['name'] + '-20261103.service'
    if root.exists():
        assert subprocess.run(['systemctl', 'is-active', '--quiet', unit]).returncode == 0, 'Reconcile existing BP service before retry'
        return root, unit
    (root / 'config').mkdir(parents=True)
    (root / 'data').mkdir()
    text = (V / 'config/test-low-cost/config.ini').read_text()
    text = text.replace('127.0.0.1:8888', '127.0.0.1:' + str(entry['rpc_port']))
    text = text.replace('127.0.0.1:9876', '127.0.0.1:' + str(entry['p2p_port']))
    text += '\nplugin = eosio::net_api_plugin\nproducer-name = ' + entry['name']
    text += '\np2p-peer-address = 127.0.0.1:' + str(f.p2p_port)
    text += '\np2p-bp-gossip-endpoint = ' + entry['name'] + ',127.0.0.1:' + str(entry['p2p_port']) + ',127.0.0.1'
    text += '\npeer-private-key = ' + json.dumps([keys['peer']['Public key'], keys['peer']['Private key']])
    for role in ['production', 'bls']:
        text += '\nsignature-provider = ' + keys[role]['Public key'] + '=KEY:' + keys[role]['Private key']
    text += '\nresource-monitor-space-absolute-gb = 8\n'
    config = root / 'config/config.ini'
    config.write_text(text)
    config.chmod(0o600)
    for p in [root, *root.rglob('*')]:
        os.chown(p, account.pw_uid, account.pw_gid)
    root.chmod(0o750)
    args = [str(V / 'bin/nodeos'), '--config-dir', str(root / 'config'), '--data-dir', str(root / 'data'), '--genesis-json', str(R / 'founder/genesis.json')]
    content = '[Unit]\nDescription=PureEOS TEST BP ' + entry['name'] + '\nAfter=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory=' + str(root) + '\nExecStart=' + shlex.join(args) + '\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:' + str(root / 'nodeos.log') + '\nStandardError=append:' + str(root / 'nodeos.log') + '\n[Install]\nWantedBy=multi-user.target\n'
    Path('/etc/systemd/system', unit).write_text(content)
    subprocess.run(['systemctl', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', 'enable', '--now', unit], check=True, capture_output=True)
    return root, unit


try:
    f = load_chain()
    ensure_bank(f)
    (R / 'bps').mkdir(exist_ok=True)
    (R / 'bps').chmod(0o755)
    plan = json.loads((R / 'PLAN.json').read_text())['BPs']
    keys = json.loads((R / 'private/bp-keys.json').read_text())
    registered = {x['owner'] for x in f.table('producersv2')}
    for entry in plan[1:target]:
        name = entry['name']
        if name in registered:
            continue
        key = keys[name]
        import_key(f, key['account'])
        auth = permission(key['account']['Public key'])
        try:
            rpc(f.http_port, 'get_account', {'account_name': name})
            already_created = True
        except Exception:
            already_created = False
        funding = None
        if not already_created:
            f.self_transact('create-test-BP-' + name, [
                action('newaccount', 'civicbankers', {'creator': 'civicbankers', 'name': name, 'owner': auth, 'active': auth}),
                action('buyram', 'civicbankers', {'payer': 'civicbankers', 'receiver': name, 'quant': '100.0000 AIC'}),
            ])
        already_bonded = any(row['owner'] == name and int(row['balance']) == 100000000 for row in f.table('bpbonds'))
        if not already_bonded:
            # Every stake and PowerUp action is authorized by and owned by its receiver.
            actions = [
                transfer('civicbankers', name, '16000.0000 AIC', 'TEST own bond, RAM and resources'),
                action('buyram', name, {'payer': name, 'receiver': name, 'quant': '100.0000 AIC'}),
                action('delegatebw', name, {'from': name, 'receiver': name, 'stake_net_quantity': '10.0000 AIC', 'stake_cpu_quantity': '100.' + str(entry['index']).zfill(4) + ' AIC', 'transfer': False}),
                action('powerup', name, {'payer': name, 'receiver': name, 'days': 30, 'net_frac': 5000000000000, 'cpu_frac': 5000000000000, 'max_payment': '100.0000 AIC'}),
                action('bondbp', name, {'producer': name}),
            ]
            funding = f.self_transact('fund-own-resources-' + name, actions)['transaction_id']
        root, unit = start_bp(entry, key)
        goal = f.rpc('get_info')['last_irreversible_block_num']
        deadline = time.monotonic() + 180
        while True:
            try:
                info = rpc(entry['rpc_port'], 'get_info')
                if info['last_irreversible_block_num'] >= goal:
                    break
            except Exception:
                pass
            assert time.monotonic() < deadline, 'New BP sync timeout: ' + name
            time.sleep(.25)
        actions = [
            transfer('civicbankers', name, '0.0001 AIC', 'TEST sponsored registration; BP signs own actions'),
            action('regproducer', name, {'producer': name, 'producer_key': key['production']['Public key'], 'url': 'https://aic-test.matrixai.one', 'location': 0}),
            action('regfinkey', name, {'finalizer_name': name, 'finalizer_key': key['bls']['Public key'], 'proof_of_possession': key['bls']['Proof of Possession']}),
            action('regpeerkey', name, {'proposer_finalizer_name': name, 'key': key['peer']['Public key']}),
            action('voteproducer', name, {'voter': name, 'proxy': '', 'producers': [name]}),
        ]
        registration = f.self_transact('register-funded-BP-' + name, actions)['transaction_id']
        registered.add(name)
        report['joined'].append({'name': name, 'index': entry['index'], 'unit': unit, 'funding_transaction': funding, 'registration_transaction': registration})
        report['registered_BPs'] = len(registered)
        save()
        print(json.dumps({'registered_BPs': len(registered), 'name': name}), flush=True)
    producers = f.table('producersv2')
    assert len(producers) == target
    bonds = f.table('bpbonds')
    assert all(any(row['owner'] == e['name'] and int(row['balance']) == 100000000 for row in bonds) for e in plan[:target])
    cfg = f.table('bpbondcfg')[0]
    assert int(cfg['locked']) == target * 100000000
    report.update(status='REGISTERED_AND_SYNCED', registered_BPs=len(producers), bond_config=cfg,
        chain_id=f.rpc('get_info')['chain_id'], notes='This is enrollment, not the later sustained stability or full native-duty qualification.')
    save()
    print(json.dumps({'status': report['status'], 'BPs': target}), flush=True)
except BaseException:
    error = traceback.format_exc()
    if f:
        error = f.redact(error)
    report.update(status='FAIL', error=error)
    save()
    print(error, flush=True)
    raise
