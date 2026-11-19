"""Server-only adapter for the bounded, owned TEST replacement state machine."""
from pathlib import Path
import datetime
import hashlib
import json
import os
import pwd
import re
import shlex
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from replacement_policy import validate_actions, FUND_UNITS, RAM_UNITS, RESERVE_UNITS
from replacement_store import save, load, advance_transaction

G = Path('/opt/pureeos-testnet-stall-recovery-20261116-r1')
C = Path('/opt/pureeos-devnet-20261105-r1')
V = Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5')
sys.path.insert(0, str(G))
import stability_guard as guard

CLEOS = C / 'tools-runtime/programs/cleos/cleos'
UTIL = C / 'tools-runtime/programs/helios-util/helios-util'
KEOSD = C / 'tools-runtime/programs/keosd/keosd'


def units(asset):
    if not re.fullmatch(r'\d+\.\d{4} AIC', asset):
        raise ValueError('Unexpected token precision or symbol')
    return int(asset.split()[0].replace('.', ''))


class ChainAdapter:
    def __init__(self, root):
        self.root = Path(root)
        self.signer = None
        self.signer_log = None
        self.socket_path = None
        self.secrets = []

    def __enter__(self):
        return self

    def __exit__(self, *error):
        if self.signer is not None:
            if self.signer.poll() is None:
                self.signer.terminate()
                try:
                    self.signer.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    # Only this new auxiliary signer child, never a native BP.
                    self.signer.kill()
                    self.signer.wait(timeout=5)
            if self.signer_log is not None:
                self.signer_log.close()
            save(self.root / 'AUTO_SIGNER_CLOSED.json', {'status': 'PASS', 'utc': guard.now(),
                                                       'owned_auxiliary_PID': self.signer.pid})
            self.signer = None
        self.secrets.clear()

    def table(self, name, code='eosio', scope='eosio'):
        if code != 'eosio' or scope != 'eosio' or name not in ['bpbondcfg', 'producersv2', 'bpmaint', 'bpbonds']:
            raise ValueError('Table outside reviewed replacement schema')
        values, cursor, previous = [], 0, -1
        deadline = time.monotonic() + 20
        for _ in range(128):
            if time.monotonic() >= deadline:
                raise RuntimeError('Bounded table-read deadline')
            q = {'code': code, 'scope': scope, 'table': name, 'json': True,
                 'limit': 20, 'time_limit_ms': 1000, 'key_type': 'i64', 'index_position': 'primary'}
            if cursor:
                q['lower_bound'] = str(cursor)
            page = guard.rpc('get_table_rows', q, port=8890)
            if name == 'bpbondcfg':
                if page['more'] or len(page['rows']) != 1:
                    raise RuntimeError('Incomplete pinned bond singleton read')
                return page['rows']
            for row in page['rows']:
                primary = guard.owner_primary_key(row['owner'])
                if primary < cursor or primary <= previous:
                    raise RuntimeError('Duplicate or backwards primary row')
                previous = primary
            values.extend(page['rows'])
            if len(values) > 4096:
                raise RuntimeError('Table exceeds reviewed row bound')
            if not page['more']:
                return values
            following = str(page.get('next_key') or '')
            if not page['rows'] or not following or int(following) < previous:
                raise RuntimeError('Incomplete table pagination')
            cursor = previous + 1
        raise RuntimeError('Table exceeds reviewed page bound')

    def trust_checks(self, retire=True):
        if hashlib.sha256((V / 'libexec/nodeos').read_bytes()).hexdigest() != guard.PIN:
            raise RuntimeError('Native binary changed')
        expected = load(self.root / 'AUTO_TRUST_PINS.json')
        for account, pin in expected['contracts'].items():
            actual = guard.rpc('get_raw_abi', {'account_name': account}, port=8890)
            if any(actual[k] != v for k, v in pin.items()):
                raise RuntimeError('Contract pin changed: ' + account)
        for path, sha in expected['tools'].items():
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != sha:
                raise RuntimeError('Signing tool changed')
        if guard.rpc('get_info')['chain_id'] != guard.CHAIN:
            raise RuntimeError('Unexpected chain')
        if retire:
            self.retire_pending_bans()
        inflight = guard.load('GUARD_INFLIGHT.json')
        if inflight['status'] not in ['PASS_ONE_GUARDED_RECYCLE', 'PASS_RECONCILED_SAME_OPERATION',
                                      'ABORTED_BEFORE_NATIVE_STOP_RETRY_AFTER_COOLDOWN',
                                      'CLOSED_CANONICAL_BAN_RETIRED_NO_RESTART']:
            raise RuntimeError('Reconcile an unfinished memory operation first')
        bond_config = self.table('bpbondcfg')[0]
        # The required amount is fixed by the pinned SYSTEM code, not a field
        # in this version of bpbondcfg. Never invent a missing table field.
        if bond_config['version'] != 1 or bond_config['enforced'] != 1:
            raise RuntimeError('Native bond enforcement differs from reviewed code')

    def retire_pending_bans(self):
        from retire_banned_runtime import retire_confirmed
        nodes = guard.load('GUARD_NODE_MAP.json')
        rows = guard.rows()
        # A newly registered node can be banned before its roster commit. It
        # remains an owned job, so stop/clean it too, then hold that job.
        for path in (self.root / 'jobs').glob('*/NODE.json'):
            candidate = load(path)
            name = candidate['name']
            if name in rows and rows[name]['banned']:
                nodes[name] = candidate
        # Same production retirement code and lock as the memory guardian. This
        # step does not depend on having funds for the replacement.
        retire_confirmed(guard, nodes)
        from retired_unit_cleanup import cleanup
        cleanup(guard, nodes, self.root)
        inflight = guard.load('GUARD_INFLIGHT.json')
        if inflight.get('status') not in ['STARTED', 'NEEDS_POSITIVE_RECONCILIATION_NO_SECOND_STOP']:
            return
        name = inflight['target']
        receipt = load(G / 'auto-retired' / (name + '.json'))
        row = guard.rows()[name]
        if not row['banned'] or receipt is None:
            return
        if receipt['status'] not in ['PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED',
                                     'PASS_RECONCILED_COMPLETED_RETIREMENT']:
            return
        # A permanently banned target must never be restarted merely to finish
        # the earlier memory-recycle proof. Preserve its failure and close only
        # against the canonical retirement receipt.
        subprocess.run(['systemctl', 'stop', inflight['watchdog'] + '.timer'],
                       capture_output=True, timeout=10, check=False)
        inflight.update(status='CLOSED_CANONICAL_BAN_RETIRED_NO_RESTART', utc=guard.now(),
                        canonical_retirement=receipt)
        guard.save(inflight['event'] + '/CLOSED_PERMANENT_BAN.json', inflight)
        guard.save('GUARD_INFLIGHT.json', inflight)
        guard.save('GUARD_LAST.json', inflight)

    def context(self):
        nodes = guard.load('GUARD_NODE_MAP.json')
        s, info, pids, rows, mem = guard.status_context(nodes)
        maintenance = {r['owner'] for r in self.table('bpmaint') if r['state'] == 2}
        bank = guard.rpc('get_currency_balance', {'code': 'eosio.token', 'account': 'civicbankers', 'symbol': 'AIC'}, port=8890)
        eligible = {r['owner'] for r in self.table('producersv2') if r['is_active'] and not rows[r['owner']]['banned']} - maintenance
        pressure = (Path('/proc/pressure/cpu').read_text().splitlines()[0]).split()
        cpu_pressure60 = float(next(x.split('=')[1] for x in pressure if x.startswith('avg60=')))
        retired = []
        for n, v in nodes.items():
            path = G / 'auto-retired' / (n + '.json')
            if rows[n]['banned'] and path.exists():
                receipt = load(path)
                if receipt['status'] in ['PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED', 'PASS_RECONCILED_COMPLETED_RETIREMENT']:
                    retired.append({'owner': n, 'ban_block_id': receipt['ban_block_id'], 'ban_block_num': rows[n]['ban_final_block']})
        return {'eligible': len(eligible), 'policy_count': s['policy_count'], 'threshold': s['threshold'],
                'recent': len(s['recent']), 'head_age': s['head_age'], 'LIB_age': s['lib_age'],
                'all_running': s['all_running'], 'RAM': mem['MemAvailable'], 'swap_free': mem['SwapFree'],
                'disk': s['disk_free'], 'bank_units': units(bank[0]) if bank else 0,
                'cpu_pressure60': cpu_pressure60}, retired

    def verify_original_ban(self, job):
        rows = guard.rows()
        n = job['replaces']
        row = rows[n]
        if not row['banned'] or row['zero_windows'] != 3 or row['ban_final_block'] != job['ban_block_num']:
            raise RuntimeError('Original irreversible ban binding changed')
        for port in [8888, 8890]:
            info = guard.rpc('get_info', port=port)
            if info['chain_id'] != guard.CHAIN or info['last_irreversible_block_num'] < row['ban_final_block']:
                raise RuntimeError('Ban is not irreversible on both sources')
            if guard.rpc('get_block', {'block_num_or_id': row['ban_final_block']}, port=port)['id'] != job['ban_block_id']:
                raise RuntimeError('Canonical ban block differs')
        nodes = guard.load('GUARD_NODE_MAP.json')
        if n in nodes:
            v = nodes[n]
            if guard.state(v['unit'])['MainPID'] != '0' or (Path(v['config_dir']) / 'config.ini').exists():
                raise RuntimeError('Old identity was not fully retired')

    def account_absent(self, name):
        try:
            guard.rpc('get_account', {'account_name': name}, port=8890)
            return False
        except urllib.error.HTTPError as error:
            # The shared read helper consumed retryable errors. Use a direct bounded
            # read to distinguish a genuinely absent account from an outage.
            pass
        request = urllib.request.Request('http://127.0.0.1:8890/v1/chain/get_account',
                                         data=json.dumps({'account_name': name}).encode(),
                                         headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(request, timeout=8):
                return False
        except urllib.error.HTTPError as error:
            response = json.load(error)
            if response.get('error', {}).get('name') != 'account_query_exception':
                raise RuntimeError('Account absence cannot be established')
            return True

    def prepare(self, job):
        path = self.root / 'private' / job['id'] / 'keys.json'
        keys = load(path, {})
        for role in ['account', 'production', 'peer', 'bls']:
            if role in keys:
                continue
            command = [str(UTIL), 'bls', 'create', 'key', '--to-console'] if role == 'bls' else [str(CLEOS), 'create', 'key', '--to-console']
            result = subprocess.run(command, check=True, text=True, capture_output=True, timeout=20)
            value = {}
            for line in result.stdout.splitlines():
                if ': ' in line:
                    k, v = line.split(': ', 1)
                    if k in ['Private key', 'Public key', 'Proof of Possession']:
                        value[k] = v.strip()
            expected = {'Private key', 'Public key', 'Proof of Possession'} if role == 'bls' else {'Private key', 'Public key'}
            if set(value) != expected:
                raise RuntimeError('New key generator output incomplete')
            keys[role] = value
            save(path, keys)
        if len({v['Public key'] for v in keys.values()}) != 4:
            raise RuntimeError('New key roles must be independent')
        record = self.root / 'jobs' / job['id'] / 'NODE.json'
        if not record.exists():
            ordinal = job['ordinal']
            sockets = []
            try:
                for port in [8980 + ordinal, 9980 + ordinal]:
                    bound = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    sockets.append(bound)
                    bound.bind(('127.0.0.1', port))
            finally:
                for bound in sockets:
                    bound.close()
            save(record, {'name': job['name'], 'unit': 'pureeos-auto-bp-' + job['name'] + '-20261119.service',
                          'rpc_port': 8980 + ordinal, 'p2p_port': 9980 + ordinal,
                          'config_dir': str(self.root / 'nodes' / job['name'] / 'config'),
                          'data_dir': str(self.root / 'nodes' / job['name'] / 'data')})

    def health_before_step(self, job):
        self.trust_checks()
        self.verify_original_ban(job)
        row = guard.rows().get(job['name'])
        if row is not None and row['banned']:
            raise RuntimeError('Replacement itself is banned: retired, preserve job, no further funding')
        context, retired = self.context()
        if context['head_age'] > 5 or context['LIB_age'] > 20 or context['recent'] < 19 or context['policy_count'] != 21 or context['threshold'] != 15:
            raise RuntimeError('Fresh native quorum required before each step')
        if context['RAM'] < 512 * 1024 ** 2 or context['swap_free'] < 8 * 1024 ** 3 or context['disk'] < 25 * 1024 ** 3:
            raise RuntimeError('Preserve host resource headroom')
        if context['cpu_pressure60'] > 90:
            raise RuntimeError('WAIT_CPU_HEADROOM_BEFORE_ANY_NEW_STEP')
        if context['eligible'] >= 25 and row is None:
            raise RuntimeError('Target pool filled by another actor; no extra BP registration')
        if job['stage'] != 'PREPARED':
            keys = load(self.root / 'private' / job['id'] / 'keys.json')
            expected = {'threshold': 1, 'keys': [{'key': keys['account']['Public key'], 'weight': 1}], 'accounts': [], 'waits': []}
            account = guard.rpc('get_account', {'account_name': job['name']}, port=8890)
            for permission in ['owner', 'active']:
                actual = next(x['required_auth'] for x in account['permissions'] if x['perm_name'] == permission)
                if actual != expected:
                    raise RuntimeError('Fresh replacement authority changed before funding/registration')
        if job['stage'] in ['PREPARED', 'ACCOUNT_CREATED']:
            need = FUND_UNITS + (RAM_UNITS if job['stage'] == 'PREPARED' else 0) + RESERVE_UNITS
            if context['bank_units'] < need:
                raise RuntimeError('WAIT_BANK_FUNDS_BEFORE_FINANCIAL_STEP')

    def command(self, args, input_text=None):
        q = subprocess.run([str(CLEOS), '--no-auto-keosd', '--url', 'http://127.0.0.1:8888',
                            '--wallet-url', 'unix://' + str(self.socket_path), *args],
                           input=input_text, capture_output=True, text=True, timeout=90)
        if q.returncode:
            # Even an error can contain an unbroadcast signed packet. Keep raw
            # CLI diagnostics in server-private storage, never public status.
            identifier = getattr(self, 'job', {}).get('id', 'diagnostic')
            save(self.root / 'private' / identifier / 'last-cli-error.json',
                 {'returncode': q.returncode, 'operation': args[:2],
                  'stdout': q.stdout, 'stderr': q.stderr})
            if 'already in wallet' in (q.stdout + q.stderr).lower():
                raise RuntimeError('Owned TEST key already in wallet')
            raise RuntimeError('Owned TEST CLI command failed; details retained server-private')
        return q.stdout

    def open_signer(self, job):
        if self.signer is None:
            private = self.root / 'private' / job['id']
            wallet_dir = private / 'wallet'
            wallet_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
            self.socket_path = private / 'k.sock'
            if len(str(self.socket_path).encode()) >= 104:
                raise RuntimeError('Private signer socket path too long')
            if self.socket_path.exists():
                connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    connection.settimeout(1)
                    connection.connect(str(self.socket_path))
                except ConnectionRefusedError:
                    if not self.socket_path.is_socket():
                        raise RuntimeError('Unexpected private socket path')
                    self.socket_path.unlink()
                else:
                    raise RuntimeError('Existing signer at this job socket; do not borrow it')
                finally:
                    connection.close()
            self.signer_log = (private / 'signer.log').open('ab')
            self.signer = subprocess.Popen([str(KEOSD), '--http-server-address', '',
                                            '--unix-socket-path', str(self.socket_path),
                                            '--wallet-dir', str(wallet_dir)],
                                           stdout=self.signer_log, stderr=self.signer_log)
            deadline = time.monotonic() + 20
            while not self.socket_path.exists():
                if self.signer.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Own isolated signer not ready')
                time.sleep(.2)
            password_file = private / 'wallet-password.txt'
            wallet_file = wallet_dir / 'autotest.wallet'
            if not wallet_file.exists():
                if password_file.exists():
                    raise RuntimeError('Reconcile partially created job wallet')
                self.command(['wallet', 'create', '-n', 'autotest', '--file', str(password_file)])
                password_file.chmod(0o600)
            else:
                password = password_file.read_text().strip()
                self.secrets.append(password)
                self.command(['wallet', 'open', '-n', 'autotest'])
                self.command(['wallet', 'unlock', '-n', 'autotest'], password + '\n')
        account = guard.rpc('get_account', {'account_name': 'civicbankers'})
        auth = next(x['required_auth'] for x in account['permissions'] if x['perm_name'] == 'active')
        if auth['threshold'] != 1 or len(auth['keys']) != 1 or auth['accounts'] or auth['waits']:
            raise RuntimeError('Owned bank requires its original single TEST key')
        public = auth['keys'][0]['key']
        found = []
        def walk(value):
            if isinstance(value, dict):
                if value.get('Public key') == public and isinstance(value.get('Private key'), str):
                    found.append(value)
                for child in value.values():
                    walk(child)
            elif isinstance(value, list):
                for child in value:
                    walk(child)
        for path in [C / 'private/bank-keys.json', C / 'private/bootstrap-keys.json']:
            walk(json.loads(path.read_text()))
        if not found:
            raise RuntimeError('Known TEST bank key unavailable')
        keys = load(self.root / 'private' / job['id'] / 'keys.json')
        for value in [found[0], keys['account']]:
            secret = value['Private key']
            self.secrets.append(secret)
            # Key material is passed only over stdin; never argv or logs.
            try:
                self.command(['wallet', 'import', '-n', 'autotest'], secret + '\n')
            except RuntimeError as error:
                if 'already in wallet' not in str(error).lower():
                    raise

    def transaction(self, job, stage):
        keys = load(self.root / 'private' / job['id'] / 'keys.json')
        actions = validate_actions(stage, None, job['name'], keys['account']['Public key'],
                                   keys['production']['Public key'], keys['bls'], keys['peer']['Public key'], job['cpu_units'])
        self.job = job
        self.stage = stage
        path = self.root / 'private' / job['id'] / 'transactions' / stage / 'STATE.json'
        # Existing packets only need read-only canonical checks, no wallet unlock.
        if not path.exists():
            self.open_signer(job)
        return advance_transaction(path, {'actions': actions}, self)

    def sign(self, intent, directory):
        save(directory / 'intent.json', intent)
        signed = json.loads(self.command(['push', 'transaction', str(directory / 'intent.json'), '-j', '-x', '1800', '--dont-broadcast', '--compression', 'zlib']))
        if not signed.get('signatures'):
            raise RuntimeError('Signing produced no signatures')
        save(directory / 'signed.json', signed)
        packed = json.loads(self.command(['convert', 'pack_transaction', str(directory / 'signed.json')]))
        if packed['compression'] not in [0, 'none']:
            raise RuntimeError('Cannot verify packed transaction ID')
        transaction_id = hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()
        save(directory / 'packed.json', packed)
        return packed, transaction_id, guard.rpc('get_info')['last_irreversible_block_num'], hashlib.sha256((directory / 'signed.json').read_bytes()).hexdigest()

    def broadcast(self, packed, transaction_id):
        request = urllib.request.Request('http://127.0.0.1:8888/v1/chain/push_transaction',
                                         data=json.dumps(packed).encode(), headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=30) as response:
            value = json.load(response)
        if value.get('transaction_id') != transaction_id or value['processed']['receipt']['status'] != 'executed':
            raise RuntimeError('Reconcile exact original packet ID')

    def reconcile(self, state):
        info = guard.rpc('get_info')
        cursor = state['cursor']
        deadline = time.monotonic() + 20
        for number in range(cursor, min(info['last_irreversible_block_num'] + 1, cursor + 128)):
            if time.monotonic() >= deadline:
                break
            block = guard.rpc('get_block', {'block_num_or_id': number})
            for entry in block['transactions']:
                identifier = entry['trx'] if isinstance(entry['trx'], str) else entry['trx']['id']
                if identifier == state['transaction_id']:
                    if entry['status'] != 'executed':
                        raise RuntimeError('Original transaction did not execute')
                    return {'transaction_id': identifier, 'block_num': number, 'block_id': block['id']}, number + 1
            cursor = number + 1
        return None, cursor

    def verify_receipt(self, receipt):
        info = guard.rpc('get_info')
        if info['last_irreversible_block_num'] < receipt['block_num']:
            raise RuntimeError('Receipt not irreversible')
        block = guard.rpc('get_block', {'block_num_or_id': receipt['block_num']})
        if block['id'] != receipt['block_id']:
            raise RuntimeError('Receipt canonical block changed')
        entries = [x for x in block['transactions'] if (x['trx'] if isinstance(x['trx'], str) else x['trx']['id']) == receipt['transaction_id']]
        if len(entries) != 1 or entries[0]['status'] != 'executed':
            raise RuntimeError('Receipt transaction missing')

    def start_and_sync(self, job):
        from replacement_runtime import start_and_sync
        return start_and_sync(self, job)

    def commit_roster(self, job):
        node = load(self.root / 'jobs' / job['id'] / 'NODE.json')
        rows = guard.rows()
        if rows[job['name']]['banned']:
            raise RuntimeError('Replacement itself is banned; never resurrect it')
        self.verify_receipt(job['register_receipt'])
        bond = next(x for x in self.table('bpbonds') if x['owner'] == job['name'])
        if bond['balance'] != 100000000 or bond['forfeited'] != 0:
            raise RuntimeError('Fresh bond not intact')
        nodes = guard.load('GUARD_NODE_MAP.json')
        if job['name'] in nodes:
            if nodes[job['name']]['config_dir'] != node['config_dir']:
                raise RuntimeError('Existing roster binding differs')
            return
        if job['replaces'] not in nodes:
            raise RuntimeError('Original owned roster binding missing')
        del nodes[job['replaces']]
        node['config_sha256'] = guard.sha(Path(node['config_dir']) / 'config.ini')
        node['p2p'] = '127.0.0.1:' + str(node['p2p_port'])
        nodes[job['name']] = node
        if len(nodes) != 25:
            raise RuntimeError('Keep exactly the existing25 owned roster slots')
        guard.save('GUARD_NODE_MAP.json', nodes)
