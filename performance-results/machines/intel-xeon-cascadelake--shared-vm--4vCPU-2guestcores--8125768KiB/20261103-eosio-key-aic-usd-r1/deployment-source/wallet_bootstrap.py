"""Fresh-chain bootstrap only: deploy approved AIC/USD wallet before sealing."""
from pathlib import Path
import hashlib, json, os, urllib.error


def deploy_key_wallet(self):
    R = self.root.parent
    artifacts = R / 'key-contract'
    expected = json.loads((R / 'KEY_ARTIFACTS.json').read_text())
    for name, digest in expected['files'].items():
        assert hashlib.sha256((artifacts / name).read_bytes()).hexdigest() == digest
    assert self.rpc('get_code_hash', {'account_name': 'eosio.token'})['code_hash'] == expected['token_code_hash']
    supported = self.rpc('get_supported_protocol_features', {'exclude_disabled': True}, namespace='producer')
    byid = {f['feature_digest']: f for f in supported}
    activated = {f['feature_digest'] for f in self.rpc('get_activated_protocol_features', {'limit': 1000})['activated_protocol_features']}

    def activate(digest):
        if digest in activated:
            return
        for dep in byid[digest]['dependencies']:
            activate(dep)
        self.action('activate', {'feature_digest': digest}, 'eosio')
        activated.add(digest)

    required = {'GET_CODE_HASH', 'CRYPTO_PRIMITIVES', 'RAM_RESTRICTIONS'}
    found = set()
    for feature in supported:
        labels = {s['value'] for s in feature['specification']}
        if labels & required:
            activate(feature['feature_digest'])
            found |= labels & required
    assert found == required
    native_key = self.accounts['eosio']['Public key']
    def account(name):
        try:return self.rpc('get_account',{'account_name':name})
        except urllib.error.HTTPError as error:
            detail=error.read().decode()
            if 'unknown key' in detail:return None
            raise RuntimeError(detail)
    def balance(name,contract='eosio.token',symbol='AIC'):
        values=self.rpc('get_currency_balance',{'code':contract,'account':name,'symbol':symbol})
        return int(values[0].split()[0].replace('.',''))if values else 0
    def root(operation,actor,data,contract='eosio'):
        return {'account':contract,'name':operation,'authorization':[{'actor':actor,'permission':'active'}],'data':data}
    def create_funded(name, public_key):
        if account(name):return
        auth={'threshold':1,'keys':[{'key':public_key,'weight':1}],'accounts':[],'waits':[]}
        return self.self_transact('create-and-fund-'+name,[
            root('newaccount','eosio',{'creator':'eosio','name':name,'owner':auth,'active':auth}),
            root('buyram','eosio',{'payer':'eosio','receiver':name,'quant':'100.0000 AIC'})])
    create_funded('eosio.key',native_key)
    if not account('eosio.key')['self_delegated_bandwidth']:
        assert balance('eosio.key')==0,'Reconcile unexpected pre-init wallet funds'
        self.self_transact('fund-and-own-wallet-resources',[
            root('transfer','eosio',{'from':'eosio','to':'eosio.key','quantity':'110.0000 AIC','memo':'TEST own wallet bootstrap stake'},'eosio.token'),
            root('delegatebw','eosio.key',{'from':'eosio.key','receiver':'eosio.key','stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':'100.0000 AIC','transfer':False})])
    assert balance('eosio.key')==0,'No unattributed liquid AIC before wallet init'
    keyfile=R/'private/usd-keys.json'
    key=json.loads(keyfile.read_text())if keyfile.exists()else self.key()
    keyfile.write_text(json.dumps(key)+'\n')
    self.accounts['keyusdtoken1'] = key
    self.secrets.append(key['Private key'])
    try:self.run(['wallet', 'import', '-n', 'localtest'], input_text=key['Private key'] + '\n')
    except RuntimeError as error:
        if 'already in wallet'not in str(error).lower():raise
    create_funded('keyusdtoken1',key['Public key'])
    if not account('keyusdtoken1')['self_delegated_bandwidth']:
        if balance('keyusdtoken1')==0:
            self.tx(['transfer','eosio','keyusdtoken1','10000.0000 AIC','TEST standard USD issuer resources'])
        self.self_stake('keyusdtoken1','10.0000 AIC','100.0000 AIC')
    if self.rpc('get_code_hash',{'account_name':'keyusdtoken1'})['code_hash']!=expected['token_code_hash']:
        self.tx(['set', 'contract', 'keyusdtoken1', str(self.contracts / 'eosio.token')])
    stats=self.rpc('get_currency_stats',{'code':'keyusdtoken1','symbol':'USD'})
    if not stats:
        self.action('create', {'issuer': 'keyusdtoken1', 'maximum_supply': '1000000.0000 USD'}, 'keyusdtoken1', contract='keyusdtoken1')
        stats=self.rpc('get_currency_stats',{'code':'keyusdtoken1','symbol':'USD'})
    if stats['USD']['supply']=='0.0000 USD':
        self.action('issue', {'to': 'keyusdtoken1', 'quantity': '100000.0000 USD', 'memo': 'TEST USD; no fiat backing claim'}, 'keyusdtoken1', contract='keyusdtoken1')
    else:assert stats['USD']['supply']=='100000.0000 USD'
    for name in ['gptgenesisbp', 'civicuserone']:
        amount=balance(name,'keyusdtoken1','USD')
        if not amount:self.action('transfer', {'from': 'keyusdtoken1', 'to': name, 'quantity': '1000.0000 USD', 'memo': 'TEST initial USD allocation'}, 'keyusdtoken1', contract='keyusdtoken1')
        else:assert amount==10000000
    codeauth = {'threshold': 1, 'keys': [], 'accounts': [{'permission': {'actor': 'eosio.key', 'permission': 'eosio.code'}, 'weight': 1}], 'waits': []}
    self.tx(['set', 'account', 'permission', 'eosio.key', 'active', json.dumps(codeauth), 'owner'])
    self.tx(['set', 'contract', 'eosio.key', str(artifacts), 'eosio.key.wasm', 'eosio.key.abi', '-p', 'eosio.key@owner'])
    cfg = {'chain_id': self.rpc('get_info')['chain_id'], 'deployment': hashlib.sha256(os.urandom(32)).hexdigest(),
           'fee_contract': 'eosio.token', 'fee_symbol': '4,AIC',
           'system_hash': self.rpc('get_code_hash', {'account_name': 'eosio'})['code_hash'],
           'policy': {'minimum_fee': 100, 'unit_fee': 10, 'new_row_fee': 500, 'maximum_multiplier_bps': 100000,
                      'epoch_seconds': 1, 'max_wallets': 1000, 'max_balances': 4000, 'max_assets': 32, 'max_native_payers': 1000}}
    self.action('init', cfg, 'eosio.key@owner', contract='eosio.key')
    self.action('updateauth', {'account': 'eosio.key', 'permission': 'owner', 'parent': '', 'auth': codeauth}, 'eosio.key@owner')
    assert self.rpc('get_code_hash', {'account_name': 'eosio.key'})['code_hash'] == expected['files']['eosio.key.wasm']
    # Persist the newly generated issuer key too; later helpers import only local
    # private files and never depend on the independent qualification VM's keys.
    p = R / 'private/bootstrap-keys.json'
    saved = json.loads(p.read_text())
    saved['accounts'] = self.accounts
    p.write_text(json.dumps(saved, indent=2) + '\n')
    (R / 'WALLET_BOOTSTRAP.json').write_text(json.dumps({'status': 'PRESEALED', 'configuration': cfg,
        'whitelist': json.loads((artifacts / 'token_allowlist.json').read_text()), 'artifacts': expected,
        'note': 'Initial AIC/USD bootstrap profile, not an early governance proposal execution'}, indent=2) + '\n')
