"""Remote TEST-only campaign helpers. No live production-chain paths."""
from pathlib import Path
import json
import subprocess
import time

R = Path('/opt/pureeos-devnet-20261105-r1')
V = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-3')
exec(compile((Path('/opt/pureeos-dev-account-genesis-20261105-r1/fresh_definitions.py')).read_text(), 'test45-definitions', 'exec'), globals())


def load_chain():
    f = TestChain.__new__(TestChain)
    f.root = R / 'founder'
    f.build = R / 'tools-runtime'
    f.contracts = V / 'contracts'
    f.cleos = f.build / 'programs/cleos/cleos'
    f.nodeos = f.build / 'programs/nodeos/nodeos'
    f.util = f.build / 'programs/helios-util/helios-util'
    f.resource_split = f.rotation_context = True
    f.logs, f.secrets, f.checks = [], [], []
    f.nonce = max([1000]+[int(p.stem)for p in(f.root/'immutable-packets').glob('*.json')if p.stem.isdigit()])
    f.bootstrapping = False
    f.report = {'status': 'running', 'checks': f.checks, 'confirmed_transactions': []}
    f.hooked = f.seven_day_activated = f.deferred_activated = True
    keys = json.loads((R / 'private/bootstrap-keys.json').read_text())
    f.accounts, f.bls, f.peer, f.production = keys['accounts'], keys['bls'], keys['peer'], keys['production']
    ports = json.loads((R / 'FOUNDER_PORTS.json').read_text())
    f.http_port, f.p2p_port = ports['rpc'], ports['p2p']
    f.url = 'http://127.0.0.1:' + str(f.http_port)
    f.node = ServiceHandle('pureeos-devnet-founder-20261105.service')
    f.wallet = ServiceHandle('pureeos-devnet-wallet-20261105.service')
    def collect(obj):
        if isinstance(obj, dict):
            for key, value in obj.items():
                if key == 'Private key':
                    f.secrets.append(value)
                else:
                    collect(value)
        elif isinstance(obj, list):
            for value in obj:
                collect(value)
    for p in (R / 'private').glob('*keys.json'):
        collect(json.loads(p.read_text()))
    password = (f.root / 'wallet/password.txt').read_text().strip()
    f.secrets.append(password)
    subprocess.run(['systemctl', 'start', f.wallet.unit], check=True)
    try:
        f.run(['wallet', 'unlock', '-n', 'localtest'], input_text=password + '\n')
    except RuntimeError as error:
        if 'already unlocked' not in str(error).lower():
            raise
    assert f.rpc('get_info')['chain_id']==json.loads((R/'BOOTSTRAP_RESULT.json').read_text())['chain_id']
    return f


def action(name, actor, data, contract='eosio'):
    return {'account': contract, 'name': name, 'authorization': [{'actor': actor, 'permission': 'active'}], 'data': data}


def transfer(sender, receiver, amount, memo):
    return action('transfer', sender, {'from': sender, 'to': receiver, 'quantity': amount, 'memo': memo}, 'eosio.token')


def permission(key):
    return {'threshold': 1, 'keys': [{'key': key, 'weight': 1}], 'accounts': [], 'waits': []}


def import_key(f, key):
    f.secrets.append(key['Private key'])
    try:
        f.run(['wallet', 'import', '-n', 'localtest'], input_text=key['Private key'] + '\n')
    except RuntimeError as error:
        if 'already in wallet' not in str(error).lower():
            raise


def ensure_bank(f):
    path = R / 'private/bank-keys.json'
    if path.exists():
        key = json.loads(path.read_text())
        f.secrets.append(key['Private key'])
        if (R / 'BANK_PREPARATION.json').exists():
            f.rpc('get_account', {'account_name': 'civicbankers'})
            return key
    else:
        key = f.key()
        path.write_text(json.dumps(key, indent=2) + '\n')
        path.chmod(0o600)
    import_key(f, key)
    # Sealed eosio permissions are code-only. Fund from the existing founder's
    # liquid allocation and let every new account sign its own resources.
    if not any(row['owner'] == 'gptgenesisbp' for row in f.table('powup.order', scope='0')):
        f.action('powerup', {'payer': 'gptgenesisbp', 'receiver': 'gptgenesisbp', 'days': 30,
            'net_frac': 200000000000000, 'cpu_frac': 200000000000000, 'max_payment': '100.0000 AIC'}, 'gptgenesisbp')
    name = 'civicbankers'
    auth = permission(key['Public key'])
    try:
        f.rpc('get_account', {'account_name': name})
        created = True
    except RuntimeError as error:
        if 'account' not in str(error).lower() and 'unknown key' not in str(error).lower():
            raise
        created = False
    if not created:
        f.self_transact('create-test-bank', [
        action('newaccount', 'gptgenesisbp', {'creator': 'gptgenesisbp', 'name': name, 'owner': auth, 'active': auth}),
        action('buyram', 'gptgenesisbp', {'payer': 'gptgenesisbp', 'receiver': name, 'quant': '100.0000 AIC'}),
        ])
    actions = [
        transfer('gptgenesisbp', name, '900000.0000 AIC', 'TEST existing founder allocation; no mint'),
        action('buyram', name, {'payer': name, 'receiver': name, 'quant': '1000.0000 AIC'}),
        action('delegatebw', name, {'from': name, 'receiver': name, 'stake_net_quantity': '1000.0000 AIC', 'stake_cpu_quantity': '10000.0000 AIC', 'transfer': False}),
        action('powerup', name, {'payer': name, 'receiver': name, 'days': 30, 'net_frac': 200000000000000, 'cpu_frac': 200000000000000, 'max_payment': '100.0000 AIC'}),
        action('voteproducer', name, {'voter': name, 'proxy': '', 'producers': ['gptgenesisbp']}),
    ]
    tx = f.self_transact('fund-test-bank', actions)
    (R / 'BANK_PREPARATION.json').write_text(json.dumps({'status': 'PASS', 'account': name, 'transaction_id': tx['transaction_id'], 'existing_issued_funds_only': True, 'self_owned_stake_and_PowerUp': True}, indent=2) + '\n')
    return key
