"""New read-only final proof. No signed/private packet bytes are exported."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

R = Path('/opt/pureeos-testnet-bank-issuance-20261119-r1')
sys.path.insert(0, str(R))
import bank_upgrade as b

assert not (R / 'FINAL_LIVE_AUDIT.json').exists(), 'Completed final audit is not replayed'
done = b.load('WORKFLOW_COMPLETE.json')
assert done['status'] == 'PASS_MINTED_AND_EXACT_ORIGINAL_CONTRACT_RESTORED'
health = b.guard(b.OLD)
p = b.load('PROPOSAL.json')
m = b.load('MINT_AND_RESTORE_PROPOSAL.json')
r = b.load('RESTORATION.json')
assert b.stamp(m['canonical_receipt']['timestamp']) >= b.stamp(p['governance']['execute_after'])
assert b.stamp(r['canonical_receipt']['timestamp']) >= b.stamp(m['restore_governance']['execute_after'])
assert (b.stamp(p['governance']['execute_after']) - b.stamp(p['governance']['proposed_at'])).total_seconds() == 1800
assert (b.stamp(m['restore_governance']['execute_after']) - b.stamp(m['restore_governance']['proposed_at'])).total_seconds() == 1800
transactions = {}
for label in ['propose-r2', 'execute-mint-restore-proposal', 'restore']:
    meta = b.load('transactions/' + label + '.json')
    assert meta['status'] == 'IRREVERSIBLE'
    receipt = meta['canonical_receipt']
    for port in [8888, 8890]:
        block = b.rpc('get_block', {'block_num_or_id': receipt['block_num']}, port=port)
        assert block['id'] == receipt['block_id']
        ids = [x for x in block['transactions'] if (x['trx'] if isinstance(x['trx'], str) else x['trx']['id']) == receipt['transaction_id']]
        assert len(ids) == 1 and ids[0]['status'] == 'executed'
    signed_path = R / 'private' / label / 'signed.json'
    packet = json.loads((R / 'private' / label / 'packed.json').read_text())
    assert hashlib.sha256(bytes.fromhex(packet['packed_trx'])).hexdigest() == receipt['transaction_id']
    assert hashlib.sha256(signed_path.read_bytes()).hexdigest() == meta['signed_file_sha256']
    count = len(json.loads(signed_path.read_text())['signatures'])
    assert count == (2 if label == 'execute-mint-restore-proposal' else 1)
    closed = b.load('SIGNER_' + label + '_CLOSED.json')
    assert closed['status'] == 'PASS'
    transactions[label] = {'canonical_receipt': receipt, 'signature_count_only': count,
                           'signed_file_sha256_only': meta['signed_file_sha256'], 'signer_closed': closed}
assert len({x['canonical_receipt']['transaction_id'] for x in transactions.values()}) == 3
assert not (R / 'private/propose/signed.json').exists(), 'Initial refused signer never signed'

authority_source = b.load('ISSUANCE_AUTHORITY_PREFLIGHT.json')
authority = {}
for name in ['eosio', 'eosio.prods', 'eosio.token', 'eosio.dev', 'civicgovauth', 'civicbankers']:
    a = b.rpc('get_account', {'account_name': name})
    authority[name] = a['permissions']
    if authority_source and name in authority_source['accounts']:
        assert a['permissions'] == authority_source['accounts'][name]['permissions']
timers = {}
for unit in ['pureeos-stability-memory-20261116.timer', 'pureeos-stability-logs-20261116.timer',
             'pureeos-auto-replacement-20261119.timer']:
    timers[unit] = subprocess.check_output(['systemctl', 'show', unit, '-p', 'ActiveState', '-p', 'SubState'], text=True).strip()
    assert 'ActiveState=active' in timers[unit]
worker_state = dict(line.split('=', 1) for line in subprocess.check_output(['systemctl', 'show', 'pureeos-bank-issuance-20261119.service', '-p', 'MainPID', '-p', 'ActiveState', '-p', 'Result'], text=True).splitlines() if '=' in line)
assert worker_state['MainPID'] == '0', 'Own worker must be stopped'
if worker_state['Result'] != 'success':
    reconciliation = b.load('READ_ONLY_RECONCILIATION.json')
    assert reconciliation['status'] == 'PASS_POSITIVE_READ_ONLY_RECONCILIATION'
    assert reconciliation['no_new_signature_or_broadcast_or_chain_write'] is True
    assert reconciliation['original_restore_packet_receipt'] == r['canonical_receipt']
    assert r['completion_via'] == 'PASS_POSITIVE_READ_ONLY_RECONCILIATION'
    assert worker_state['Result'] == 'exit-code' and worker_state['ActiveState'] == 'failed'
else:
    assert worker_state['ActiveState'] == 'inactive'
original_wallet_pid = subprocess.check_output(['systemctl', 'show', 'pureeos-devnet-wallet-20261105.service', '-p', 'MainPID', '--value'], text=True).strip()
assert original_wallet_pid == '0'
auto_root = Path('/opt/pureeos-testnet-cpu-stability-20261119-r1')
auto = json.loads((auto_root / 'AUTO_REPLACEMENT_LATEST.json').read_text())
assert b.stamp(auto['utc']) >= b.stamp(r['canonical_receipt']['timestamp']), 'Wait for the original automatic timer to observe restored code'
assert auto['status'] not in ['HOLD_RECONCILE_NO_NEW_FINANCIAL_ACTION', 'SKIP_OTHER_NATIVE_OPERATION'], 'Wait for a fresh qualified automatic trust check'
job_summary = [{k: job[k] for k in ['id', 'name', 'replaces', 'stage', 'reserved_units']} for path in sorted((auto_root / 'jobs').glob('*/STATE.json')) if (job := json.loads(path.read_text()))]
result = {'status': 'PASS_COMPLETE_TWO_FULL_REVIEWS_ONE_MINT_EXACT_RESTORATION', 'utc': b.now(),
          'stats': b.stats(), 'bank_current_units': b.bank(), 'mint_bank_before_units': m['bank_before_units'],
          'mint_bank_after_units': m['bank_after_units'], 'mint_amount_units': 1500000000,
          'original_token_pins': b.token_pin(), 'transactions': transactions,
          'first_review_seconds': 1800, 'second_review_seconds': 1800, 'mint_receipt_before_restore': m['mint_receipt'],
          'permanent_receipt_scope_after_restore': r['permanent_receipt_scope'], 'authorities': authority,
          'health': health, 'timers': timers, 'original_wallet_PID': original_wallet_pid, 'own_worker_exited': worker_state,
          'auto_replacement_latest': auto, 'auto_jobs': job_summary,
          'private_keys_or_signed_packets_exported': False,
          'read_only_reconciliation': b.load('READ_ONLY_RECONCILIATION.json') if (R / 'READ_ONLY_RECONCILIATION.json').exists() else None}
b.save('FINAL_LIVE_AUDIT.json', result)
print(json.dumps(result))
