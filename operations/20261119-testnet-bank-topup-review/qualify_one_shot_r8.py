"""NEW disposable local contract fixture. Never connects to the public TEST host.

Imports only generic process/RPC test utilities. No old campaign controller,
public wallet, existing chain directory or remote key is used.
"""
from pathlib import Path
import datetime
import base64
import hashlib
import json
import os
import shutil
import shlex
import sys
import time
import traceback

os.umask(0o077)
D = Path(__file__).resolve().parent
K = Path('/home/guagua/.local/share/pureeos-key-native-ram-20261111')
sys.path.insert(0, str(K / 'sdk-tests'))
from pureeos_single_choice_local_test import LocalTest

BASE = K / 'runtime/contracts/eosio.token'
CANDIDATE = D / 'reviewed-artifact'
assert hashlib.sha256((BASE / 'eosio.token.wasm').read_bytes()).hexdigest() == 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
assert hashlib.sha256((K / 'runtime/libexec/nodeos').read_bytes()).hexdigest() == '946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
W = Path('/tmp/tmi-20261119-r8')
W.mkdir(mode=0o700, exist_ok=False)
for n in ['nodeos', 'cleos', 'keosd', 'helios-util']:
    p = W / 'programs' / n / n
    p.parent.mkdir(parents=True)
    p.write_text('#!/bin/sh\nexec ' + shlex.quote(str(K / 'runtime/bin' / n)) + ' \"$@\"\n')
    p.chmod(0o700)


class Fixture(LocalTest):
    local_signers = ('eosio',)

    def __init__(self):
        super().__init__(W, K / 'runtime/contracts')
        self.cleos = K / 'native-client/cleos'

    def run(self, *args, **kwargs):
        self.last_raw = super().run(*args, **kwargs)
        return self.last_raw

    def setup_minimal(self, *, issuer='eosio', precision=4):
        self.accounts = {n: self.key() for n in ['eosio', 'eosio.token', 'eosio.dev', 'civicbankers', 'alice', 'bob']}
        self.bls = {'eosio': self.key(True)}
        (self.root / 'config').mkdir()
        (self.root / 'wallet').mkdir()
        genesis = {
            'initial_timestamp': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000'),
            'initial_key': self.accounts['eosio']['Public key'],
            'initial_savanna': {'version': 2, 'finalizer_key': self.bls['eosio']['Public key'],
                                'proof_of_possession': self.bls['eosio']['Proof of Possession']},
        }
        (self.root / 'genesis.json').write_text(json.dumps(genesis))
        (self.root / 'config/config.ini').write_text('\n'.join([
            'plugin = eosio::producer_plugin', 'plugin = eosio::chain_api_plugin',
            'plugin = eosio::producer_api_plugin', 'producer-name = eosio',
            'enable-stale-production = true', 'chain-state-db-size-mb = 128',
            'chain-state-db-guard-size-mb = 4', 'resource-monitor-space-absolute-gb = 1',
            'http-server-address = 127.0.0.1:' + str(self.http_port),
            'p2p-listen-endpoint = 127.0.0.1:' + str(self.p2p_port),
            'max-body-size = 8388608', 'verbose-http-errors = true',
            'validation-mode = full', 'vote-threads = 1', 'chain-threads = 1', 'net-threads = 1',
            *['experimental-parallel-code-hash = ' + hashlib.sha256(p.read_bytes()).hexdigest() for p in [BASE / 'eosio.token.wasm', CANDIDATE / 'eosio.token.wasm', K / 'runtime/contracts/eosio.bios/eosio.bios.wasm']],
            'signature-provider = ' + self.accounts['eosio']['Public key'] + '=KEY:' + self.accounts['eosio']['Private key'],
            'signature-provider = ' + self.bls['eosio']['Public key'] + '=KEY:' + self.bls['eosio']['Private key'], '',
        ]))
        self.wallet = self.spawn([str(K / 'runtime/bin/keosd'), '--data-dir', str(self.root / 'wallet'),
                                  '--config-dir', str(self.root / 'wallet'), '--http-server-address', ''], 'wallet.log')
        deadline = time.monotonic() + 15
        while not (self.root / 'wallet/keosd.sock').exists():
            assert self.wallet.poll() is None and time.monotonic() < deadline
            time.sleep(.1)
        self.run(['wallet', 'create', '-n', 'fixture', '--file', str(self.root / 'wallet/password.txt')])
        for pair in self.accounts.values():
            self.run(['wallet', 'import', '-n', 'fixture'], input_text=pair['Private key'] + '\n')
        self.start_node('genesis', ['--genesis-json', str(self.root / 'genesis.json'),
                                    '--resource-monitor-space-absolute-gb', '1'])
        info = self.wait_lib(5)
        self.report['chain_id'] = info['chain_id']
        assert info['chain_id'] != '09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
        self.tx(['set', 'contract', 'eosio', str(K / 'runtime/contracts/eosio.bios')])
        for name in ['eosio.token', 'eosio.dev', 'civicbankers', 'alice', 'bob']:
            pair = self.accounts[name]
            self.tx(['create', 'account', 'eosio', name, pair['Public key'], pair['Public key']])
            self.action('setalimits', {'account': name, 'ram_bytes': -1, 'net_weight': -1, 'cpu_weight': -1}, 'eosio')
        self.tx(['set', 'contract', 'eosio.token', str(BASE)])
        suffix = '.' + '0' * precision + ' AIC'
        self.action('create', {'issuer': issuer, 'maximum_supply': '2100000000' + suffix}, 'eosio.token', contract='eosio.token')
        self.action('issue', {'to': issuer, 'quantity': '100000000' + suffix, 'memo': 'NEW isolated fixture only'}, issuer, contract='eosio.token')
        self.action('transfer', {'from': issuer, 'to': 'civicbankers', 'quantity': '2279' + suffix, 'memo': 'synthetic fixture bank'}, issuer, contract='eosio.token')

    def state(self):
        out = {}
        for scope, table in [('AIC', 'stat'),
                             *[(name, 'accounts') for name in self.accounts]]:
            r = self.rpc('get_table_rows', {'code': 'eosio.token', 'scope': scope, 'table': table,
                                           'json': False, 'show_payer': True, 'limit': 50})
            assert not r['more'], 'Partial fixture state'
            out[scope + '/' + table] = r['rows']
        receipts = self.rpc('get_table_by_scope', {'code': 'eosio.token', 'table': 'testmint', 'limit': 100})
        assert not receipts['more'], 'Partial receipt scope list'
        out['testmint-scope'] = receipts['rows']
        return out

    def topup(self, actors=('eosio.dev@active', 'civicbankers@active'), *, failure=None):
        cmd = ['push', 'action', 'eosio.token', 'testtopup', '{}']
        for actor in actors:
            cmd += ['-p', actor]
        return self.tx(cmd, failure=failure)

    def refused(self, failure, actors=('eosio.dev@active', 'civicbankers@active')):
        before = self.state()
        expected_actor = failure.removeprefix('missing authority of ') if failure.startswith('missing authority of ') else None
        raw = self.topup(actors, failure='missing_auth_exception' if expected_actor else failure)
        if expected_actor:
            obj = json.loads(self.last_raw)
            def contains_actor(value):
                if isinstance(value, dict):
                    return value.get('account') == expected_actor or any(contains_actor(v) for v in value.values())
                return isinstance(value, list) and any(contains_actor(v) for v in value)
            assert contains_actor(obj['processed']['except']), 'Wrong missing authorization actor'
        self.check(before == self.state(), 'Rejected ' + failure + ' leaves token rows and payers exact')

    def amount(self, owner):
        b = self.rpc('get_currency_balance', {'code': 'eosio.token', 'account': owner, 'symbol': 'AIC'})
        return int(b[0].split()[0].replace('.', '')) if b else 0

    def close(self):
        failures = []
        for p in [self.node, self.wallet]:
            if p is not None and p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=20)
                except Exception as error:
                    failures.append(str(error))
            if p is not None and p.poll() != 0:
                failures.append('Private child shutdown: ' + str(p.poll()))
        for log in self.logs:
            log.close()
        self.report['private_children_stopped'] = not failures
        assert not failures, failures


reports = []
current = None
try:
    f = current = Fixture()
    f.setup_minimal()
    before = f.state()
    f.tx(['set', 'contract', 'eosio.token', str(CANDIDATE), 'eosio.token.wasm', 'eosio.token.abi'])
    f.check(f.state() == before, 'Candidate upgrade preserves existing token rows and RAM payers')
    abi_record = f.rpc('get_raw_abi', {'account_name': 'eosio.token'})
    raw_abi = base64.b64decode(abi_record['abi'])
    assert hashlib.sha256(raw_abi).hexdigest() == abi_record['abi_hash']
    abi_file = CANDIDATE / 'eosio.token.raw.abi'
    if abi_file.exists():
        assert abi_file.read_bytes() == raw_abi
    else:
        abi_file.write_bytes(raw_abi)
    f.report['candidate_raw_ABI_hash'] = abi_record['abi_hash']
    f.refused('missing authority of eosio.dev', actors=('civicbankers@active',))
    f.refused('missing authority of civicbankers', actors=('eosio.dev@active',))
    f.refused('missing authority of eosio.dev', actors=('alice@active',))

    f.action('setmaxsupply', {'issuer': 'eosio', 'maximum_supply': '2100000001.0000 AIC'}, 'eosio', contract='eosio.token')
    f.refused('unexpected AIC cap')
    f.action('setmaxsupply', {'issuer': 'eosio', 'maximum_supply': '2100000000.0000 AIC'}, 'eosio', contract='eosio.token')
    f.action('issue', {'to': 'eosio', 'quantity': '0.0001 AIC', 'memo': 'fixture supply guard'}, 'eosio', contract='eosio.token')
    f.refused('unexpected TEST supply')
    f.action('retire', {'quantity': '0.0001 AIC', 'memo': 'fixture restore'}, 'eosio', contract='eosio.token')

    ram = f.rpc('get_account', {'account_name': 'civicbankers'})['ram_usage']
    f.action('setalimits', {'account': 'civicbankers', 'ram_bytes': ram, 'net_weight': -1, 'cpu_weight': -1}, 'eosio')
    f.refused('ram_usage_exceeded')
    f.action('setalimits', {'account': 'civicbankers', 'ram_bytes': -1, 'net_weight': -1, 'cpu_weight': -1}, 'eosio')

    before = f.state()
    mint = {'account': 'eosio.token', 'name': 'testtopup',
            'authorization': [{'actor': 'eosio.dev', 'permission': 'active'}, {'actor': 'civicbankers', 'permission': 'active'}], 'data': {}}
    impossible_transfer = {'account': 'eosio.token', 'name': 'transfer',
                           'authorization': [{'actor': 'civicbankers', 'permission': 'active'}],
                           'data': {'from': 'civicbankers', 'to': 'alice', 'quantity': '999999999.0000 AIC', 'memo': 'intentional rollback fixture'}}
    f.tx(['push', 'transaction', json.dumps({'actions': [mint, impossible_transfer]})], failure='overdrawn balance')
    f.check(f.state() == before, 'Later-action failure rolls back mint, receipt and all balances atomically')

    bank_before, issuer_before = f.amount('civicbankers'), f.amount('eosio')
    stats_before = f.rpc('get_currency_stats', {'code': 'eosio.token', 'symbol': 'AIC'})['AIC']
    mint_result = f.topup()
    stats_after = f.rpc('get_currency_stats', {'code': 'eosio.token', 'symbol': 'AIC'})['AIC']
    f.check(stats_after == {**stats_before, 'supply': '100150000.0000 AIC'}, 'Exactly 150000 AIC minted; cap and issuer unchanged')
    f.check(f.amount('civicbankers') == bank_before + 1500000000 and f.amount('eosio') == issuer_before,
            'Only bank receives 150000 AIC; no issuer/treasury balance debit')
    receipt = f.table('testmint', code='eosio.token', scope='eosio.token')
    f.check(len(receipt) == 1 and receipt[0]['id'] == 20261119 and receipt[0]['quantity'] == '150000.0000 AIC',
            'One permanent receipt records exact allocation')
    f.refused('TEST topup already executed')
    after_mint = f.state()
    f.tx(['set', 'contract', 'eosio.token', str(BASE)])
    f.check(f.state() == after_mint, 'Original code/ABI restoration preserves mint, receipt, rows and payers')
    f.check(f.rpc('get_code_hash', {'account_name': 'eosio.token'})['code_hash'] == 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c',
            'Exact original token WASM restored')
    f.check(f.rpc('get_raw_abi', {'account_name': 'eosio.token'})['abi_hash'] == '2d69b533e9ac8e557abfde5c034988dfc8a9d3e7bc7f8656fcf866cea2dda856',
            'Exact original raw token ABI restored')
    f.action('transfer', {'from': 'civicbankers', 'to': 'alice', 'quantity': '1.0000 AIC', 'memo': 'post-restore fixture'}, 'civicbankers', contract='eosio.token')
    f.check(f.amount('alice') == 10000, 'Normal bank transfer works after restoration')

    f.action('retire', {'quantity': '150000.0000 AIC', 'memo': 'test that supply restoration never resets receipt'}, 'eosio', contract='eosio.token')
    f.tx(['set', 'contract', 'eosio.token', str(CANDIDATE), 'eosio.token.wasm', 'eosio.token.abi'])
    f.refused('TEST topup already executed')
    f.check(f.rpc('get_currency_stats', {'code': 'eosio.token', 'symbol': 'AIC'})['AIC']['supply'] == '100000000.0000 AIC',
            'Even restoring old supply and redeploying candidate cannot mint twice')
    f.close()
    reports.append(f.report)
    current = None

    for label, issuer, precision, error in [
        ('wrong-issuer', 'bob', 4, 'unexpected AIC issuer'),
        ('wrong-precision', 'eosio', 3, 'unexpected AIC precision'),
    ]:
        f = current = Fixture()
        f.report['fixture'] = label
        f.setup_minimal(issuer=issuer, precision=precision)
        f.tx(['set', 'contract', 'eosio.token', str(CANDIDATE), 'eosio.token.wasm', 'eosio.token.abi'])
        f.refused(error)
        f.close()
        reports.append(f.report)
        current = None
    result = {'status': 'PASS_ISOLATED_WASM_ONLY', 'scope': 'Contract semantics on new private local chains; not live mint or governance execution acceptance',
              'nodeos_sha256': hashlib.sha256((K / 'runtime/libexec/nodeos').read_bytes()).hexdigest(),
              'fixture_script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'checks': sum(len(r['checks']) for r in reports), 'reports': reports,
              'public_chain_signatures': 0, 'public_chain_mutations': 0}
    (D / 'ISOLATED_QUALIFICATION_R8_RESULT.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'checks': result['checks'], 'public_mutations': 0}))
except BaseException:
    error = traceback.format_exc()
    if current is not None:
        error = current.redact(error)
    (D / 'ISOLATED_QUALIFICATION_R8_FAILURE.txt').write_text(error)
    raise
finally:
    if current is not None:
        current.close()
