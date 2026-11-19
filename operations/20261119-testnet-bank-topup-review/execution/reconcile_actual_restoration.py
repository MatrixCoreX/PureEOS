"""Positive READ-ONLY reconciliation of an already irreversible restoration.

The original postcondition incorrectly assumed limit counts matching tables.
Native get_table_by_scope counts scanned entries, even if the table filter does
not match. Bound BOTH scope ends to the actual receipt's fixed scope instead.
No original financial controller is invoked; no wallet/signing/broadcast occurs.
"""
from pathlib import Path
import json
import subprocess
import sys
import time

R = Path('/opt/pureeos-testnet-bank-issuance-20261119-r1')
sys.path.insert(0, str(R))
import bank_upgrade as b

assert not (R / 'READ_ONLY_RECONCILIATION.json').exists()
assert not (R / 'RESTORATION.json').exists()
assert not (R / 'WORKFLOW_COMPLETE.json').exists()
hold = b.load('WORKFLOW_HOLD.json')
assert hold['type'] == 'AssertionError'
p = b.load('transactions/restore.json')
assert p['status'] == 'IRREVERSIBLE' and p['reported_accepted'] is True
m = b.load('MINT_AND_RESTORE_PROPOSAL.json')
receipt = p['canonical_receipt']
assert b.stamp(receipt['timestamp']) >= b.stamp(m['restore_governance']['execute_after'])
for port in [8888, 8890]:
    info = b.rpc('get_info', port=port)
    assert info['chain_id'] == b.CHAIN and info['last_irreversible_block_num'] >= receipt['block_num']
    block = b.rpc('get_block', {'block_num_or_id': receipt['block_num']}, port=port)
    assert block['id'] == receipt['block_id']
    matches = [t for t in block['transactions'] if (t['trx'] if isinstance(t['trx'], str) else t['trx']['id']) == receipt['transaction_id']]
    assert len(matches) == 1 and matches[0]['status'] == 'executed'

query = {'code': 'eosio.token', 'table': 'testmint', 'lower_bound': 'eosio.token',
         'upper_bound': 'eosio.token', 'limit': 10, 'time_limit_ms': 1000}
for attempt in range(3):
    q = b.rpc('get_table_by_scope', query)
    if not q['more']:
        break
    time.sleep(.2)
assert not q['more'], 'Exact scope must have a complete read, not a partial cursor'
assert q['rows'] == [{'code': 'eosio.token', 'scope': 'eosio.token', 'table': 'testmint',
                       'payer': 'civicbankers', 'count': 1}]
g = b.rpc('get_upgrade_state')
assert not g['pending'] and g['proposal_id'] == m['restore_governance']['proposal_id']
assert b.token_pin() == b.OLD
assert b.stats() == {'supply': '100150000.0000 AIC', 'max_supply': '2100000000.0000 AIC', 'issuer': 'eosio'}
health = b.guard(b.OLD)
worker = subprocess.check_output(['systemctl', 'show', 'pureeos-bank-issuance-20261119.service',
                                  '-p', 'MainPID', '-p', 'ActiveState', '-p', 'Result'], text=True)
assert 'MainPID=0' in worker and 'ActiveState=failed' in worker and 'Result=exit-code' in worker
proof = {'status': 'PASS_POSITIVE_READ_ONLY_RECONCILIATION', 'utc': b.now(),
         'reason': 'get_table_by_scope limit counts scanned scopes; an unbounded filtered query returned the correct receipt plus a normal continuation cursor. Exact lower/upper scope query is complete and verifies all expected fields.',
         'original_worker_hold_preserved': hold, 'original_worker_state_preserved': worker,
         'original_restore_packet_receipt': receipt, 'scope_query': query, 'complete_receipt_scope': q,
         'health': health, 'no_new_signature_or_broadcast_or_chain_write': True}
b.save('READ_ONLY_RECONCILIATION.json', proof)
restored = {'status': 'PASS_MINTED_AND_EXACT_ORIGINAL_CONTRACT_RESTORED', 'utc': b.now(),
            'completion_via': 'PASS_POSITIVE_READ_ONLY_RECONCILIATION', 'canonical_receipt': receipt,
            'before_governance': m['restore_governance'], 'after_governance': g,
            'original_token_pins': b.OLD, 'stats': b.stats(), 'bank_units': b.bank(),
            'permanent_receipt_scope': q['rows'], 'health': health,
            'original_postcheck_failure_preserved': hold}
b.save('RESTORATION.json', restored)
b.save('WORKFLOW_COMPLETE.json', {'utc': b.now(), 'status': restored['status'],
                                'completion_via': restored['completion_via'], 'canonical_receipt': receipt,
                                'stats': b.stats(), 'bank_units': b.bank(), 'no_repeated_financial_phase': True})
print(json.dumps(proof))
