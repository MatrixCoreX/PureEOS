"""Execute only the qualified, mature native developer proposal."""
from pathlib import Path
import datetime
import hashlib
import json
import os
import subprocess
import sys

os.umask(0o077)
K = Path('/opt/pureeos-key-rate-20261104-r1')
R = Path('/opt/pureeos-keynet-20261103-r2')
F = Path('/opt/pureeos-explorer-key-rate-20261104-r3')
assert json.loads((K / 'QUALIFICATION_r6.json').read_text())['status'] == 'PASS'
assert json.loads((F / 'BUILD_RESULT.json').read_text())['status'] == 'PASS'
assert json.loads((F / 'browser-preview/RESULT.json').read_text())['status'] == 'PASS'
assert json.loads((F / 'INTEGRATION_RESULT.json').read_text())['status'] == 'PASS'
assert not (K / 'EXECUTION_PROGRESS.json').exists(), 'Reconcile earlier execution before retrying'
proposal = json.loads((K / 'PROPOSAL.json').read_text())
assert proposal['status'] == 'PROPOSED_IRREVERSIBLE'
sys.path.insert(0, str(R))
from campaign_helpers import load_chain

f = load_chain()
f.cleos = R / 'native-client/cleos'
state = f.governance()
assert state['pending'] and state['proposal_id'] == proposal['governance']['proposal_id']
for key in ['new_code_hash', 'new_abi_hash', 'review_document_hash']:
    assert state[key] == proposal['proposal'][key]
assert state['review_seconds'] == 1800 and not state.get('execution_blocked_no_electorate', False)
assert state['effective_veto_count'] < state['required_vetoes'], 'Native electorate veto blocks execution'
info = f.rpc('get_info')
assert info['chain_id'] == proposal['chain_id']
head_time = datetime.datetime.fromisoformat(info['head_block_time'])
assert head_time >= datetime.datetime.fromisoformat(state['execute_after']), 'Native review has not matured'
assert info['head_block_num'] - info['last_irreversible_block_num'] < 60
assert abs((datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) - head_time).total_seconds()) < 60
code = (K / 'artifacts-r3/eosio.key.wasm').read_bytes()
abi = Path('/opt/krate-q/r6/candidate.abi.bin').read_bytes()
assert hashlib.sha256(code).hexdigest() == state['new_code_hash']
assert hashlib.sha256(abi).hexdigest() == state['new_abi_hash']
assert f.rpc('get_code_hash', {'account_name': 'eosio.key'})['code_hash'] == proposal['proposal']['old_code_hash']
transaction = {'actions': [{'account': 'eosio', 'name': 'devexec',
    'authorization': [{'actor': 'civicgovlead', 'permission': 'upgrade'}],
    'data': {'proposal_id': state['proposal_id'], 'code': code.hex(), 'abi': abi.hex()}}]}
path = K / 'execution-transaction.json'
path.write_text(json.dumps(transaction) + '\n')
(K / 'EXECUTION_PROGRESS.json').write_text(json.dumps({'status': 'SUBMITTING',
    'proposal_id': state['proposal_id'], 'new_code_hash': state['new_code_hash'],
    'execute_after': state['execute_after']}, indent=2) + '\n')
signed_path = K / 'execution-immutable-signed.json'
assert not signed_path.exists(), 'Reconcile the previous immutable packet first'
signed = json.loads(f.run(['push','transaction',str(path),'-j','-x','1800',
                          '--dont-broadcast','--compression','zlib']))
assert signed['signatures']
signed_path.write_text(json.dumps(signed) + '\n')
packed = json.loads(f.run(['convert','pack_transaction',str(signed_path)]))
transaction_id = hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()
progress = {'status':'SUBMITTING_IMMUTABLE_PACKET','transaction_id':transaction_id,
            'packet_sha256':hashlib.sha256(signed_path.read_bytes()).hexdigest(),
            'proposal_id':state['proposal_id'],'CPU_cap_us':150000}
(K/'EXECUTION_PROGRESS.json').write_text(json.dumps(progress,indent=2)+'\n')
for attempt in range(1,4):
    before = f.rpc('get_info')
    args = [str(f.cleos),'--no-auto-keosd','--url',f.url,'--wallet-url',
            'unix://'+str(f.root/'wallet/keosd.sock'),'push','transaction',str(signed_path),
            '-s','-j','--compression','zlib']
    response = subprocess.run(args,capture_output=True,text=True,timeout=90)
    result = json.loads(response.stdout)
    assert result['transaction_id'] == transaction_id
    trace = result['processed']
    if response.returncode == 0:
        assert trace['receipt']['status'] == 'executed'
        f.report['confirmed_transactions'].append(f.wait_transaction(transaction_id,before['last_irreversible_block_num']))
        break
    output = f.redact(response.stdout+response.stderr)
    (K/('execute-immutable-cpu-'+str(attempt)+'.log')).write_text(output)
    assert trace['receipt'] is None and trace['except']['name']=='tx_cpu_usage_exceeded'
    finalized = f.wait_lib(trace['block_num']+2)
    scanned = []
    for n in range(before['last_irreversible_block_num']+1,finalized['last_irreversible_block_num']+1):
        block = f.rpc('get_block',{'block_num_or_id':n})
        assert not any((t['trx'] if isinstance(t['trx'],str) else t['trx']['id'])==transaction_id for t in block['transactions'])
        scanned.append({'block':n,'id':block['id']})
    assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==proposal['proposal']['old_code_hash']
    current = f.governance()
    assert current['pending'] and current['proposal_id']==state['proposal_id']
    (K/('execute-immutable-cpu-'+str(attempt)+'.json')).write_text(json.dumps({
        'transaction_id':transaction_id,'same_packet':True,'CPU_cap_us':150000,
        'receipt':None,'exception':'tx_cpu_usage_exceeded','canonical_absence':scanned,
        'code_and_pending_proposal_unchanged':True},indent=2)+'\n')
    assert attempt < 3, 'Preserve the immutable packet and investigate repeated CPU rejection'
actual = f.rpc('get_raw_abi', {'account_name': 'eosio.key'})
assert actual['code_hash'] == state['new_code_hash'] and actual['abi_hash'] == state['new_abi_hash']
after = f.governance()
assert not after['pending']
record = {'status': 'UPGRADED_IRREVERSIBLE', 'chain_id': info['chain_id'],
          'transaction_id': result['transaction_id'], 'canonical_receipt': f.report['confirmed_transactions'][-1],
          'new_code_hash': actual['code_hash'], 'new_abi_hash': actual['abi_hash'],
          'proposal_id': state['proposal_id'], 'execute_after': state['execute_after'],
          'review_seconds': 1800, 'before_governance': state, 'after_governance': after,
          'UTC': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'reset': False}
(K / 'EXECUTION.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps({'status': record['status'], 'transaction_id': result['transaction_id'],
                  'new_code_hash': actual['code_hash']}))
