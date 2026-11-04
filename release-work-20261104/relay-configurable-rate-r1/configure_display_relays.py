"""Five on-chain fee entries; only the existing founder HTTP service operates."""
from pathlib import Path
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request

os.umask(0o077)
K = Path('/opt/pureeos-key-rate-20261104-r1')
R = Path('/opt/pureeos-keynet-20261103-r2')
S = K / 'source-r3'
SELECTED = {'canadaboreal': 1000, 'cosmicgarden': 1500, 'deepseekcore': 2000,
            'finlandhaven': 2500, 'gptgenesisbp': 3000}
execution = json.loads((K / 'EXECUTION.json').read_text())
assert execution['status'] == 'UPGRADED_IRREVERSIBLE'
CODE = execution['new_code_hash']
sys.path.insert(0, str(R))
from campaign_helpers import load_chain, import_key

f = load_chain()
f.cleos = R / 'native-client/cleos'
assert f.rpc('get_code_hash', {'account_name': 'eosio.key'})['code_hash'] == CODE
assert f.rpc('get_info')['chain_id'] == '116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
registered = f.table('relays', code='eosio.key', scope='eosio.key')
assert set(r['bp'] for r in registered) <= set(SELECTED), 'Preserve unrelated registrations'
founder = next(r for r in registered if r['bp'] == 'gptgenesisbp')
assert founder['enabled'] and founder['endpoint'] == 'https://aic-test.matrixai.one/relay'
progress_file = K / 'DISPLAY_RELAYS_PROGRESS.json'
progress = json.loads(progress_file.read_text()) if progress_file.exists() else {
    'status': 'CONFIGURING', 'steps': {}, 'new_HTTP_services': 0,
    'new_routes': 0, 'new_relay_keys': 0, 'new_child_permissions': 0}
owner_keys = json.loads((R / 'private/bp-keys.json').read_text())
permissions_before = {bp: f.rpc('get_account', {'account_name': bp})['permissions'] for bp in SELECTED}
nginx = Path('/etc/nginx/sites-available/aic-test')
nginx_before = hashlib.sha256(nginx.read_bytes()).hexdigest()
for bp, bps in SELECTED.items():
    if bp in progress['steps']:
        assert progress['steps'][bp]['status'] == 'FINALIZED', 'Reconcile ambiguous submission: ' + bp
        continue
    if bp != 'gptgenesisbp':
        import_key(f, owner_keys[bp]['account'])
    actions = []
    authorization = [{'actor': bp, 'permission': 'active'}]
    if bp != 'gptgenesisbp':
        actions.append({'account': 'eosio.key', 'name': 'regrelay', 'authorization': authorization,
                        'data': {'bp': bp, 'permission': 'keyrelay', 'endpoint': ''}})
    actions.append({'account': 'eosio.key', 'name': 'setrate', 'authorization': authorization,
                    'data': {'bp': bp, 'fee_bps': bps}})
    path = K / ('register-display-' + bp + '.json')
    path.write_text(json.dumps({'actions': actions}) + '\n')
    progress['steps'][bp] = {'status': 'SUBMITTING', 'fee_bps': bps}
    progress_file.write_text(json.dumps(progress, indent=2) + '\n')
    f.nonce = 1679
    result = f.tx(['push', 'transaction', str(path)])
    progress['steps'][bp].update(status='FINALIZED', transaction_id=result['transaction_id'],
                                 receipt=f.report['confirmed_transactions'][-1])
    progress_file.write_text(json.dumps(progress, indent=2) + '\n')
actual = f.table('relays', code='eosio.key', scope='eosio.key')
rates = f.table('relayrates', code='eosio.key', scope='eosio.key')
assert len(actual) == 5 and {r['bp'] for r in actual} == set(SELECTED)
assert {r['bp'] for r in actual if r['enabled']} == {'gptgenesisbp'}
assert all(r['endpoint'] == '' for r in actual if r['bp'] != 'gptgenesisbp')
assert {r['bp']: r['fee_bps'] for r in rates} == SELECTED
for bp, before in permissions_before.items():
    assert f.rpc('get_account', {'account_name': bp})['permissions'] == before, 'Account permissions changed'
assert hashlib.sha256(nginx.read_bytes()).hexdigest() == nginx_before

# The qualified SDK is public source; secrets remain in the existing private roots.
for path in [S, *S.rglob('*')]:
    if not path.is_symlink():
        path.chmod(0o755 if path.is_dir() else 0o644)
backups = K / 'runtime-backups'
backups.mkdir(mode=0o700, exist_ok=True)
unit = Path('/etc/systemd/system/pureeos-keynet2-relay-20261103.service')
backup = backups / (unit.name + '.before')
if not backup.exists():
    shutil.copy2(unit, backup)
text = unit.read_text().replace(str(R / 'key-service'), str(S))
text = re.sub(r'(?m)^Environment=KEY_CODE_HASH=.*$', 'Environment=KEY_CODE_HASH=' + CODE, text)
assert str(S / 'relayer/server.mjs') in text and 'KEY_CODE_HASH=' + CODE in text
unit.write_text(text)
dropin = Path('/etc/systemd/system/pureeos-key-history-20261103.service.d')
dropin.mkdir(exist_ok=True)
(dropin / '99-relay-rate-20261104.conf').write_text('[Service]\nEnvironment=EXPECTED_KEY_CODE_HASH=' + CODE + '\n')
subprocess.run(['systemctl', 'daemon-reload'], check=True)
restart = ['pureeos-keynet2-relay-20261103.service']
if subprocess.run(['systemctl', 'is-active', '--quiet', 'pureeos-key-history-20261103.service']).returncode == 0:
    restart.append('pureeos-key-history-20261103.service')
subprocess.run(['systemctl', 'restart', *restart], check=True)
deadline = time.monotonic() + 90
while True:
    try:
        with urllib.request.urlopen('http://127.0.0.1:8787/health', timeout=10) as response:
            health = json.load(response)
        assert health['bp'] == 'gptgenesisbp' and health['fee_bps'] == 3000
        break
    except Exception:
        assert time.monotonic() < deadline
        time.sleep(2)
progress.update(status='DISPLAY_RELAYS_CONFIGURED', UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                registered=actual, rates=rates, code_hash=CODE, sole_HTTP_endpoint=founder['endpoint'],
                permissions_unchanged=True, nginx_unchanged=True, native_client_unchanged=True)
progress_file.write_text(json.dumps(progress, indent=2) + '\n')
(K / 'DISPLAY_RELAYS.json').write_text(json.dumps(progress, indent=2) + '\n')
print(json.dumps({'status': progress['status'], 'rates': rates, 'active_HTTP': founder['endpoint'],
                  'new_HTTP_services': 0, 'new_relay_keys': 0}))
