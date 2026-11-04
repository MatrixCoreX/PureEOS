"""Remote proposal only; this never bypasses the native review/execution gate."""
from pathlib import Path
import datetime
import hashlib
import json
import sys

K = Path('/opt/pureeos-key-rate-20261104-r1')
R = Path('/opt/pureeos-keynet-20261103-r2')
sys.path.insert(0, str(R))
from campaign_helpers import load_chain

assert json.loads((K / 'QUALIFICATION_r6.json').read_text())['status'] == 'PASS'
assert not (K / 'PROPOSAL_PROGRESS.json').exists(), 'Reconcile an earlier submission first'
f = load_chain()
f.cleos = R / 'native-client/cleos'
info = f.rpc('get_info')
assert info['chain_id'] == '116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
state = f.governance()
assert state['review_seconds'] == 1800 and state['sealed'] and not state['pending']
base = f.rpc('get_raw_abi', {'account_name': 'eosio.key'})
assert base['code_hash'] == 'b6509dfb14fe2f23c97e8019a355cc0354a3de2d919f135bce20ddb51cdcf7f2'
code = (K / 'artifacts-r3/eosio.key.wasm').read_bytes()
abi = Path('/opt/krate-q/r6/candidate.abi.bin').read_bytes()
sha = lambda data: hashlib.sha256(data).hexdigest()
build = json.loads((K / 'BUILD_RESULT_r3.json').read_text())
assert sha(code) == build['files']['artifacts-r3/eosio.key.wasm']
assert sha(abi) == json.loads((K / 'QUALIFICATION_r6.json').read_text())['ABI_binary_sha256']
review = {'change': 'BP-owned relay service rates; signed quote binding; canonical legacy intent compatibility',
          'minimum_bps': 1000, 'maximum_bps': 3000, 'service_minimum_AIC': '0.0001',
          'rounding': 'ceil to one AIC atom', 'legacy_default_bps': 3000,
          'claim': 'full income, application-fee-free', 'new_code_hash': sha(code),
          'new_abi_hash': sha(abi), 'qualification': json.loads((K / 'QUALIFICATION_r6.json').read_text()),
          'native_review_seconds': 1800, 'reset': False, 'consensus_change': False,
          'service_scope': 'Only founder HTTP; four empty-endpoint inactive comparison registrations, no new service/route/hotkey/permission',
          'legacy': 'Signed total cap plus original30% rate ceiling; new SDK pins exact quote',
          'retained_failure': 'r1 omitted-extension repacking mismatch; fixed in signed request serializers'}
review_bytes = (json.dumps(review, sort_keys=True, ensure_ascii=False, indent=2) + '\n').encode()
(K / 'REVIEW.json').write_bytes(review_bytes)
proposal = {'target': 'eosio.key', 'old_code_hash': base['code_hash'],
            'old_abi_hash': base['abi_hash'], 'new_code_hash': sha(code),
            'new_abi_hash': sha(abi), 'review_document_hash': sha(review_bytes)}
authorization = [{'actor': 'civicgovlead', 'permission': 'upgrade'}]
transaction = {'actions': [{'account': 'eosio', 'name': name,
                            'authorization': authorization, 'data': data}
                           for name, data in [('refreshgov', {}), ('devpropose', proposal)]]}
path = K / 'proposal-transaction.json'
path.write_text(json.dumps(transaction, indent=2) + '\n')
progress = {'status': 'SUBMITTING', 'proposal': proposal, 'chain_id': info['chain_id'],
            'UTC': datetime.datetime.now(datetime.timezone.utc).isoformat()}
(K / 'PROPOSAL_PROGRESS.json').write_text(json.dumps(progress, indent=2) + '\n')
f.nonce = 1679
result = f.tx(['push', 'transaction', str(path)])
state = f.governance()
assert state['pending'] and state['target'] == 'eosio.key'
assert state['new_code_hash'] == proposal['new_code_hash'] and state['new_abi_hash'] == proposal['new_abi_hash']
duration = (datetime.datetime.fromisoformat(state['execute_after']) -
            datetime.datetime.fromisoformat(state['proposed_at'])).total_seconds()
assert duration == 1800
record = {'status': 'PROPOSED_IRREVERSIBLE', 'transaction_id': result['transaction_id'],
          'canonical_receipt': f.report['confirmed_transactions'][-1], 'proposal': proposal,
          'governance': state, 'chain_id': info['chain_id'], 'review_seconds': duration}
(K / 'PROPOSAL.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({'status': record['status'], 'transaction_id': result['transaction_id'],
                  'proposal_id': state['proposal_id'], 'execute_after': state['execute_after']}))
