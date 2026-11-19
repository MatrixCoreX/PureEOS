"""New authorized TEST one-shot upgrade controller. No old controller is run.

One immutable signed packet and ONE broadcast per phase. Reconciliation is
read-only. No native BP, history, guardian, relay or phone service is changed.
All keys and signed packets remain in this server-only case's private folder.
"""
from pathlib import Path
import base64
import contextlib
import datetime as dt
import fcntl
import hashlib
import http.client
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent
LIVE = Path('/opt/pureeos-devnet-20261105-r1')
NATIVE = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5')
GUARD = Path('/opt/pureeos-testnet-stall-recovery-20261116-r1')
CHAIN = '09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
NATIVE_SHA = '946a52fb7331baca06779995fe30e59ab7e55c534b448f8376847a00e9fa2397'
OLD = {'code_hash': 'f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c',
       'abi_hash': '2d69b533e9ac8e557abfde5c034988dfc8a9d3e7bc7f8656fcf866cea2dda856'}
NEW = {'code_hash': 'a21148871198e1aeae781e690b202638981430dad27b3504dca4a6f2892de498',
       'abi_hash': '09fedf3e92878e098c31b45a932848a251afe174b390430145df79dfb9207e16'}
REVIEW = '3a2c71113f441b49005a055286d974af89a37a7414b9f1db264ce2d0c98bdd20'
CLEOS = LIVE / 'tools-runtime/programs/cleos/cleos'
KEOSD = LIVE / 'tools-runtime/programs/keosd/keosd'

def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()

def stamp(s):
    value = dt.datetime.fromisoformat(s.replace('Z', '+00:00'))
    return value.replace(tzinfo=dt.timezone.utc) if value.tzinfo is None else value

def sha(b):
    return hashlib.sha256(b).hexdigest()

def load(name):
    return json.loads((ROOT / name).read_text())

def save(name, value):
    p = ROOT / name
    p.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temp = p.with_name(p.name + '.tmp')
    with temp.open('wb') as f:
        f.write((json.dumps(value, ensure_ascii=False, indent=2) + '\n').encode())
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, p)
    fd = os.open(p.parent, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

def rpc(method, data=None, port=8890):
    assert method.startswith('get_'), 'Only explicit broadcast() can write to chain'
    body = json.dumps(data or {}).encode()
    for attempt in range(3):
        req = urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/{method}', data=body,
                                     headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=5) as f:
                return json.load(f)
        except urllib.error.HTTPError as error:
            error.read()
            error.close()
            if error.code not in [500, 502, 503, 504] or attempt == 2:
                raise RuntimeError('Bounded read HTTP failure: ' + method) from None
        except (OSError, urllib.error.URLError):
            if attempt == 2:
                raise RuntimeError('Bounded read transport failure: ' + method) from None
        time.sleep(.2 * (attempt + 1))

def token_pin():
    a = rpc('get_raw_abi', {'account_name': 'eosio.token'})
    return {k: a[k] for k in ['code_hash', 'abi_hash']}

def stats():
    s = rpc('get_currency_stats', {'code': 'eosio.token', 'symbol': 'AIC'})['AIC']
    assert s['max_supply'] == '2100000000.0000 AIC' and s['issuer'] == 'eosio'
    return s

def bank():
    b = rpc('get_currency_balance', {'code': 'eosio.token', 'account': 'civicbankers', 'symbol': 'AIC'})
    assert len(b) == 1 and b[0].endswith(' AIC')
    return int(b[0].split()[0].replace('.', ''))

def fields(old, new):
    return {'target': 'eosio.token', 'old_code_hash': old['code_hash'], 'old_abi_hash': old['abi_hash'],
            'new_code_hash': new['code_hash'], 'new_abi_hash': new['abi_hash'], 'review_document_hash': REVIEW}

def proposal_actions(old, new):
    auth = [{'actor': 'eosio.dev', 'permission': 'upgrade'}]
    return [{'account': 'eosio', 'name': 'refreshgov', 'authorization': auth, 'data': {}},
            {'account': 'eosio', 'name': 'devpropose', 'authorization': auth, 'data': fields(old, new)}]

def verify_proposal(s, old, new, identifier=None, mature=False):
    assert s['sealed'] and s['developer'] == 'eosio.dev' and s['pending']
    assert all(s[k] == v for k, v in fields(old, new).items()), 'Proposal payload differs'
    assert s['review_seconds'] == 1800
    assert (stamp(s['execute_after']) - stamp(s['proposed_at'])).total_seconds() == 1800
    if identifier is not None:
        assert s['proposal_id'] == identifier, 'Proposal identity differs'
    assert s['effective_voter_count'] > 0 and not s['execution_blocked_no_electorate']
    assert s['effective_veto_count'] < s['required_vetoes'], 'Native veto prevents execution'
    if mature:
        i = rpc('get_info')
        block = rpc('get_block', {'block_num_or_id': i['last_irreversible_block_num']})
        assert stamp(block['timestamp']) >= stamp(s['execute_after']), 'Full review is not irreversible yet'

def artifacts():
    assert load('AUTHORIZATION.json')['status'] == 'AUTHORIZED'
    assert sha((ROOT / 'ONE_SHOT_REVIEW.zh-CN.md').read_bytes()) == REVIEW
    assert load('ISOLATED_QUALIFICATION_R8_RESULT.json')['status'] == 'PASS_ISOLATED_WASM_ONLY'
    for directory, pin in [('candidate', NEW), ('original', OLD)]:
        assert sha((ROOT / directory / 'eosio.token.wasm').read_bytes()) == pin['code_hash']
        assert sha((ROOT / directory / 'eosio.token.raw.abi').read_bytes()) == pin['abi_hash']

def guard(expected):
    artifacts()
    baseline = load('AUTHORIZED_PREFLIGHT.json')
    assert sha((NATIVE / 'libexec/nodeos').read_bytes()) == NATIVE_SHA
    for path, digest in baseline['tools'].items():
        assert sha(Path(path).read_bytes()) == digest, 'Signing tool changed'
    for account, pin in baseline['pins'].items():
        wanted = expected if account == 'eosio.token' else pin
        a = rpc('get_raw_abi', {'account_name': account})
        assert all(a[k] == v for k, v in wanted.items()), 'Unexpected contract pin: ' + account
    for name, old in baseline['accounts'].items():
        current = rpc('get_account', {'account_name': name})
        assert current['permissions'] == old['permissions'] and current['privileged'] == old['privileged'], 'Authority changed: ' + name
    infos = [rpc('get_info', port=p) for p in [8888, 8890]]
    for i in infos:
        assert i['chain_id'] == CHAIN
        assert (dt.datetime.now(dt.timezone.utc) - stamp(i['head_block_time'])).total_seconds() < 6
        assert (dt.datetime.now(dt.timezone.utc) - stamp(i['last_irreversible_block_time'])).total_seconds() < 20
    f = rpc('get_finalizer_info', port=8902)
    recent = [v for v in f['last_tracked_votes'] if v['is_vote_strong'] and
              (dt.datetime.now(dt.timezone.utc) - stamp(v['voted_for_block_timestamp'])).total_seconds() < 12]
    assert len(f['active_finalizer_policy']['finalizers']) == 21 and f['active_finalizer_policy']['threshold'] == 15
    assert len({v['description'] for v in recent}) >= 19
    mem = {s.split(':')[0]: int(s.split()[1]) * 1024 for s in Path('/proc/meminfo').read_text().splitlines()
           if s.startswith(('MemAvailable:', 'SwapFree:'))}
    assert mem['MemAvailable'] >= 1024**3 and mem['SwapFree'] >= 8 * 1024**3
    assert os.statvfs('/').f_bavail * os.statvfs('/').f_frsize >= 25 * 1024**3
    inflight = json.loads((GUARD / 'GUARD_INFLIGHT.json').read_text())
    assert inflight['status'] in ['PASS_ONE_GUARDED_RECYCLE', 'PASS_RECONCILED_SAME_OPERATION',
                                 'ABORTED_BEFORE_NATIVE_STOP_RETRY_AFTER_COOLDOWN', 'CLOSED_CANONICAL_BAN_RETIRED_NO_RESTART']
    s = stats()
    return {'utc': now(), 'info': infos[0], 'reader_info': infos[1], 'memory': mem,
            'recent_strong': len(recent), 'token': expected, 'stats': s, 'bank_units': bank()}

@contextlib.contextmanager
def native_lock():
    with (GUARD / 'native-operation.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield

class Signer:
    def __init__(self, label, need_bank):
        self.label, self.need_bank = label, need_bank
        self.private = ROOT / 'private' / label
        self.child = None

    def command(self, args):
        p = subprocess.run([str(CLEOS), '--no-auto-keosd', '--url', 'http://127.0.0.1:8888',
                            '--wallet-url', 'unix://' + str(self.socket), *args], capture_output=True, text=True, timeout=45)
        if p.returncode:
            (self.private / 'client-error.log').write_text(p.stdout + p.stderr)
            raise RuntimeError('Native client refused; private diagnostic retained, no automatic retry')
        return p.stdout

    def wallet(self, method, data):
        path = self.socket
        class UnixHTTP(http.client.HTTPConnection):
            def connect(self):
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(10)
                self.sock.connect(str(path))
        c = UnixHTTP('localhost')
        body = '' if method in ['get_public_keys', 'list_wallets'] else json.dumps(data)
        c.request('POST', '/v1/wallet/' + method, body, {'Content-Type': 'application/json'})
        r = c.getresponse()
        b = r.read()
        c.close()
        if not 200 <= r.status < 300:
            (self.private / 'wallet-error.log').write_bytes(b)
            raise RuntimeError('Own Unix-only signer refused; private diagnostic retained')
        return json.loads(b) if b else None

    def __enter__(self):
        self.private.mkdir(mode=0o700, parents=True, exist_ok=False)
        self.socket = self.private / 'k.sock'
        assert len(str(self.socket).encode()) < 104
        self.log = (self.private / 'keosd.log').open('ab')
        self.child = subprocess.Popen([str(KEOSD), '--http-server-address', '', '--unix-socket-path', str(self.socket),
                                       '--wallet-dir', str(self.private / 'wallet')], stdout=self.log, stderr=self.log)
        try:
            deadline = time.monotonic() + 20
            while not self.socket.exists():
                assert self.child.poll() is None and time.monotonic() < deadline, 'Own signer did not become ready'
                time.sleep(.1)
            self.command(['wallet', 'create', '-n', 'testbank', '--file', str(self.private / 'password.txt')])
            public = []
            for account in ['civicgovauth'] + (['civicbankers'] if self.need_bank else []):
                auth = next(p['required_auth'] for p in rpc('get_account', {'account_name': account})['permissions'] if p['perm_name'] == 'active')
                assert auth['threshold'] == 1 and len(auth['keys']) == 1 and not auth['accounts'] and not auth['waits']
                wanted = auth['keys'][0]['key']
                matches = []
                def walk(value):
                    if isinstance(value, dict):
                        if value.get('Public key') == wanted and isinstance(value.get('Private key'), str):
                            matches.append(value)
                        for child in value.values():
                            walk(child)
                    elif isinstance(value, list):
                        for child in value:
                            walk(child)
                for p in [LIVE / 'private/bootstrap-keys.json', LIVE / 'private/bank-keys.json']:
                    if p.exists():
                        walk(json.loads(p.read_text()))
                assert matches, 'Authorized TEST role not found on server'
                self.wallet('import_key', ['testbank', matches[0]['Private key']])
                public.append(wanted)
                matches.clear()
            actual = [self.command(['convert', 'public_key', value]).strip() for value in self.wallet('get_public_keys', [])]
            assert set(actual) == set(public)
            save('SIGNER_' + self.label + '.json', {'status': 'READY', 'utc': now(), 'own_PID': self.child.pid,
                                                   'accounts': ['civicgovauth'] + (['civicbankers'] if self.need_bank else []),
                                                   'unix_only_no_TCP': True})
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.child is not None:
            if self.child.poll() is None:
                self.child.terminate()
                self.child.wait(timeout=20)
            assert self.child.returncode == 0, 'Own auxiliary signer shutdown requires inspection'
            self.log.close()
            save('SIGNER_' + self.label + '_CLOSED.json', {'status': 'PASS', 'utc': now(), 'own_PID': self.child.pid})

    def __exit__(self, *error):
        self.close()

def receipt(label, timeout=180):
    p = load('transactions/' + label + '.json')
    assert p['status'] in ['BROADCAST_ONCE', 'RECONCILING_ORIGINAL_ID', 'IRREVERSIBLE']
    deadline = time.monotonic() + timeout
    cursor = p.get('cursor', p['start_LIB'] + 1)
    previous = rpc('get_block', {'block_num_or_id': cursor - 1})['id']
    while time.monotonic() < deadline:
        i = rpc('get_info')
        while cursor <= i['last_irreversible_block_num']:
            b = rpc('get_block', {'block_num_or_id': cursor})
            assert b['previous'] == previous, 'Canonical link mismatch'
            matches = [t for t in b['transactions'] if (t['trx'] if isinstance(t['trx'], str) else t['trx']['id']) == p['transaction_id']]
            if matches:
                assert len(matches) == 1 and matches[0]['status'] == 'executed'
                other = rpc('get_block', {'block_num_or_id': cursor}, port=8888)
                assert other['id'] == b['id']
                r = {'transaction_id': p['transaction_id'], 'block_num': cursor, 'block_id': b['id'],
                     'timestamp': b['timestamp'], 'observed_LIB': i['last_irreversible_block_num'], 'two_source_canonical': True}
                p.update(status='IRREVERSIBLE', canonical_receipt=r, utc=now())
                save('transactions/' + label + '.json', p)
                return r
            previous = b['id']
            cursor += 1
        p.update(status='RECONCILING_ORIGINAL_ID', cursor=cursor, utc=now())
        save('transactions/' + label + '.json', p)
        time.sleep(1)
    raise RuntimeError('Original packet result still unknown; read-only reconciliation only, never re-sign/rebroadcast')

def send(label, actions, expected, need_bank=False):
    assert not (ROOT / ('transactions/' + label + '.json')).exists(), 'Reconcile original packet, never replay phase'
    assert not (ROOT / 'private' / label).exists(), 'Existing partial signer must be inspected, not reused'
    with native_lock():
        baseline = guard(expected)
        save('PHASE_' + label + '_STARTED.json', {'utc': now(), 'baseline': baseline, 'action_names': [a['name'] for a in actions]})
        with Signer(label, need_bank) as s:
            save('private/' + label + '/intent.json', {'actions': actions})
            signed = json.loads(s.command(['push', 'transaction', str(s.private / 'intent.json'), '-j', '-x', '300',
                                           '--dont-broadcast', '--compression', 'zlib']))
            assert signed['signatures']
            save('private/' + label + '/signed.json', signed)
            packed = json.loads(s.command(['convert', 'pack_transaction', str(s.private / 'signed.json')]))
            assert packed['compression'] in [0, 'none']
            save('private/' + label + '/packed.json', packed)
            identifier = sha(bytes.fromhex(packed['packed_trx']))
            i = rpc('get_info')
            p = {'status': 'BROADCAST_ONCE', 'utc': now(), 'transaction_id': identifier,
                 'start_LIB': i['last_irreversible_block_num'], 'expiration': signed['expiration'],
                 'signed_file_sha256': sha((s.private / 'signed.json').read_bytes()), 'action_names': [a['name'] for a in actions]}
            # Persist BEFORE the only network submission. Even a network/CPU
            # error never causes an automatic second broadcast or signature.
            save('transactions/' + label + '.json', p)
            req = urllib.request.Request('http://127.0.0.1:8888/v1/chain/push_transaction', data=json.dumps(packed).encode(),
                                         headers={'Content-Type': 'application/json'})
            try:
                with urllib.request.urlopen(req, timeout=30) as response:
                    raw = response.read()
                (s.private / 'response.json').write_bytes(raw)
                value = json.loads(raw)
                assert value['transaction_id'] == identifier
                p['reported_accepted'] = value.get('processed', {}).get('receipt', {}).get('status') == 'executed'
            except urllib.error.HTTPError as error:
                (s.private / 'response-error.json').write_bytes(error.read())
                error.close()
                p['reported_error'] = 'HTTP_' + str(error.code)
            except (OSError, urllib.error.URLError, ValueError):
                p['reported_error'] = 'AMBIGUOUS_TRANSPORT_OR_RESPONSE'
            save('transactions/' + label + '.json', p)
    return receipt(label)

def execute_action(identifier, directory):
    return {'account': 'eosio', 'name': 'devexec', 'authorization': [{'actor': 'eosio.dev', 'permission': 'upgrade'}],
            'data': {'proposal_id': identifier, 'code': (ROOT / directory / 'eosio.token.wasm').read_bytes().hex(),
                     'abi': (ROOT / directory / 'eosio.token.raw.abi').read_bytes().hex()}}

def propose():
    assert not (ROOT / 'PROPOSAL.json').exists()
    g = rpc('get_upgrade_state')
    assert not g['pending'] and g['sealed'] and g['review_seconds'] == 1800
    assert stats()['supply'] == '100000000.0000 AIC'
    before_bank = bank()
    r = send('propose-r2', proposal_actions(OLD, NEW), OLD)
    g = rpc('get_upgrade_state')
    verify_proposal(g, OLD, NEW)
    assert token_pin() == OLD
    save('PROPOSAL.json', {'status': 'PROPOSED_IRREVERSIBLE', 'utc': now(), 'governance': g,
                           'canonical_receipt': r, 'bank_before_units': before_bank})
    return {'status': 'PROPOSED_IRREVERSIBLE', 'proposal_id': g['proposal_id'], 'execute_after': g['execute_after'], 'receipt': r}

def execute_mint_restore_proposal():
    assert not (ROOT / 'MINT_AND_RESTORE_PROPOSAL.json').exists()
    p = load('PROPOSAL.json')
    g = rpc('get_upgrade_state')
    verify_proposal(g, OLD, NEW, p['governance']['proposal_id'], mature=True)
    assert stats()['supply'] == '100000000.0000 AIC'
    before_bank = bank()
    a = rpc('get_account', {'account_name': 'civicbankers'})
    assert int(a['ram_quota']) < 0 or int(a['ram_quota']) - int(a['ram_usage']) >= 1024
    mint = {'account': 'eosio.token', 'name': 'testtopup', 'authorization': [
        {'actor': 'eosio.dev', 'permission': 'active'}, {'actor': 'civicbankers', 'permission': 'active'}], 'data': ''}
    # ONE atomic transaction: exact candidate installation, fixed allocation,
    # and the next fully reviewed proposal for exact original restoration.
    actions = [execute_action(g['proposal_id'], 'candidate'), mint, *proposal_actions(NEW, OLD)]
    r = send('execute-mint-restore-proposal', actions, OLD, need_bank=True)
    after = rpc('get_upgrade_state')
    verify_proposal(after, NEW, OLD, g['proposal_id'] + 1)
    assert token_pin() == NEW
    assert stats()['supply'] == '100150000.0000 AIC' and bank() == before_bank + 1500000000
    q = rpc('get_table_rows', {'code': 'eosio.token', 'scope': 'eosio.token', 'table': 'testmint', 'json': True, 'limit': 10})
    assert not q['more'] and len(q['rows']) == 1
    row = q['rows'][0]
    assert row == {'id': 20261119, 'recipient': 'civicbankers', 'quantity': '150000.0000 AIC',
                   'supply_before': '100000000.0000 AIC', 'supply_after': '100150000.0000 AIC'}
    save('MINT_AND_RESTORE_PROPOSAL.json', {'status': 'MINTED_IRREVERSIBLE_RESTORE_UNDER_FULL_REVIEW', 'utc': now(),
         'canonical_receipt': r, 'candidate_proposal': g, 'restore_governance': after,
         'bank_before_units': before_bank, 'bank_after_units': bank(), 'stats': stats(), 'mint_receipt': row})
    return {'status': 'MINTED_IRREVERSIBLE_RESTORE_UNDER_FULL_REVIEW', 'restore_proposal_id': after['proposal_id'],
            'restore_execute_after': after['execute_after'], 'receipt': r}

def restore():
    assert not (ROOT / 'RESTORATION.json').exists()
    p = load('MINT_AND_RESTORE_PROPOSAL.json')
    g = rpc('get_upgrade_state')
    verify_proposal(g, NEW, OLD, p['restore_governance']['proposal_id'], mature=True)
    r = send('restore', [execute_action(g['proposal_id'], 'original')], NEW)
    after = rpc('get_upgrade_state')
    assert not after['pending'] and token_pin() == OLD
    assert stats()['supply'] == '100150000.0000 AIC'
    q = rpc('get_table_by_scope', {'code': 'eosio.token', 'table': 'testmint', 'limit': 100})
    assert not q['more'] and len(q['rows']) == 1 and q['rows'][0]['count'] == 1
    assert q['rows'][0]['payer'] == 'civicbankers'
    health = guard(OLD)
    save('RESTORATION.json', {'status': 'PASS_MINTED_AND_EXACT_ORIGINAL_CONTRACT_RESTORED', 'utc': now(),
         'canonical_receipt': r, 'before_governance': g, 'after_governance': after, 'original_token_pins': OLD,
         'stats': stats(), 'bank_units': bank(), 'permanent_receipt_scope': q['rows'], 'health': health})
    return {'status': 'PASS_MINTED_AND_EXACT_ORIGINAL_CONTRACT_RESTORED', 'stats': stats(), 'bank_units': bank(), 'receipt': r}

def observe():
    i = rpc('get_info')
    assert i['chain_id'] == CHAIN
    g = rpc('get_upgrade_state')
    block = rpc('get_block', {'block_num_or_id': i['last_irreversible_block_num']})
    remaining = max(0, (stamp(g['execute_after']) - stamp(block['timestamp'])).total_seconds()) if g['pending'] else 0
    result = {'status': 'READ_ONLY_OBSERVATION', 'utc': now(), 'head': i['head_block_num'],
              'LIB': i['last_irreversible_block_num'], 'pending': g['pending'], 'proposal_id': g['proposal_id'],
              'execute_after': g['execute_after'], 'remaining_irreversible_seconds': remaining,
              'effective_voters': g['effective_voter_count'], 'vetoes': g['effective_veto_count'],
              'veto_threshold': g['required_vetoes'], 'token': token_pin(), 'stats': stats(), 'bank_units': bank()}
    save('LATEST.json', result)
    return result

if __name__ == '__main__':
    os.umask(0o077)
    operation = sys.argv[1]
    try:
        if operation == 'propose':
            result = propose()
        elif operation == 'execute-mint':
            result = execute_mint_restore_proposal()
        elif operation == 'restore':
            result = restore()
        elif operation == 'observe':
            result = observe()
        elif operation == 'reconcile':
            result = receipt(sys.argv[2])
        else:
            raise ValueError('Unknown operation')
        print(json.dumps(result, ensure_ascii=False), flush=True)
    except BaseException as error:
        save('FAILURE_' + operation + '_' + dt.datetime.now(dt.timezone.utc).strftime('%H%M%S') + '.json',
             {'utc': now(), 'operation': operation, 'type': type(error).__name__, 'message': str(error),
              'no_automatic_resign_or_rebroadcast': True})
        print(json.dumps({'status': 'HOLD_NO_AUTOMATIC_FINANCIAL_RETRY', 'operation': operation,
                          'type': type(error).__name__, 'message': str(error)}), flush=True)
        raise SystemExit(1)
