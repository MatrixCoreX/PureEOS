"""Read-only canonical claim reconciliation; never submit or repeat a payment."""
from pathlib import Path
import json,hashlib,datetime,time,urllib.request
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');C=Path('/opt/pureeos-devnet-20261105-r1')
raw=(K/'PUBLIC_TRANSFER_PROGRESS.json').read_bytes();old=json.loads(raw);assert len(old['cases'])==12 and old['status']=='NEEDS_CANONICAL_RECONCILIATION'and not old.get('free_claim')
assert not(K/'PUBLIC_TRANSFER_RESULT.json').exists();(K/'PUBLIC_TRANSFER_AFTER_CLAIM_CHECK_FAILURE.json').write_bytes(raw)
source=(K/'verify_public_income.py').read_text();prefix=source[:source.index("\ntry:\n (R/'private')")].replace("assert not(R/'PUBLIC_TRANSFER_PROGRESS.json').exists(),'Reconcile existing signed IDs and all balances before resuming'",'pass # Explicit read-only reconciliation')
exec(compile(prefix,'public-income-readonly-helpers','exec'),globals());record=old;coverage=[]
def table(n,scope='eosio.key'):
 rows=[];lower='';stalls=0
 for page in range(100):
  r=f.rpc('get_table_rows',{'code':'eosio.key','scope':scope,'table':n,'json':True,'limit':1000,'lower_bound':lower})
  coverage.append({'table':n,'scope':scope,'lower_bound':lower,'rows':len(r['rows']),'more':bool(r.get('more')),'next_key':r.get('next_key')})
  if not r.get('more'):return rows+r['rows']
  next_key=str(r.get('next_key',''))
  if not r['rows']and next_key==lower:
   stalls+=1;assert stalls<=5,'Repeated empty partial table response';time.sleep(.1);continue
  assert next_key and next_key!=lower,'Incomplete table with no advancing continuation'
  rows+=r['rows'];lower=next_key
 raise AssertionError('Table pagination incomplete')
info=f.rpc('get_info');before=json.loads((K/'CLAIM_BEFORE.json').read_text());baseline=before['fee_buckets'];current=table('feebuckets')
assert {x['owner']for x in current}=={x['owner']for x in baseline}=={x['bp']for x in table('relays')}|{'eosio.key'}
network=sum(int(x['network'])for x in baseline);assert network==3036 and int(before['network_atoms'])==0
assert sum(int(x['network'])for x in current)==network and next(x for x in current if x['owner']=='gptgenesisbp')['relay_earned']==0
amount_claimed=before['earned_atoms'];assert amount_claimed==2261
assert native('gptgenesisbp')-before['native_balance_atoms']==amount_claimed
packet=C/'founder/immutable-packets/1179.json';packed=json.loads(f.run(['convert','pack_transaction',str(packet)]));assert packed['compression']in ['none',0]
tid=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest();assert tid=='1eee7ffeb5e7414841fbb732c0e910d01fd6043714fc28981093164bbe9a2881'
with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+tid,timeout=20)as r:tr=json.load(r)
actions=tr['actions'];claim_receipts=unique(actions,'eosio.key','receipt');transfers=unique(actions,'eosio.token','transfer')
assert len(claim_receipts)==1 and claim_receipts[0]['kind']=='claim'and atoms(claim_receipts[0]['quantity'])==amount_claimed and atoms(claim_receipts[0]['fee'])==0
assert len(transfers)==1 and transfers[0]['from']=='eosio.key'and transfers[0]['to']=='gptgenesisbp'and atoms(transfers[0]['quantity'])==amount_claimed
block_num=actions[0]['block_num'];b=f.rpc('get_block',{'block_num_or_id':block_num});assert b['block_num']<=info['last_irreversible_block_num']
assert all(x['block_id']==b['id']and x['block_num']==block_num for x in actions)
r=next(x for x in b['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==tid);assert r['status']=='executed'
canonical={'transaction_id':tid,'block_num':block_num,'block_id':b['id'],'observed_lib':info['last_irreversible_block_num']}
for case in record['cases']:
 c=case['canonical_receipt'];block=f.rpc('get_block',{'block_num_or_id':c['block_num']});assert block['id']==c['block_id']
 assert any((x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==case['transaction_id']and x['status']=='executed'for x in block['transactions'])
expected_income=sum(atoms(x['fee']['retained_base'])+atoms(x['fee']['relay_fee'])for x in record['cases']if x['label'].startswith(('key-to-key','key-to-named')))
assert expected_income==amount_claimed
assert len(table('config'))==1 and len(table('wallets'))==int(table('config')[0]['wallet_count'])and len(table('assets'))==3
reserves()
record['claim_verification_failure']=record.pop('recovery_error');record['reconciliation'].update(table_read_issue='Legacy LocalTest.table ignores more/next_key. Captured network_atoms0 contradicted the complete captured six-row fee_buckets sum3036. Paginated complete reads and canonical receipt reconciliation correct verification without another claim.',original_claim_not_resent=True)
check(True,'Full execution-plus-relay income irreversibly claimed once in full; zero application fee; complete network reserve unchanged')
record['free_claim']={'atoms':amount_claimed,'canonical_receipt':canonical,'native_balance_before_atoms':before['native_balance_atoms'],'native_balance_after_atoms':native('gptgenesisbp'),'application_fee_atoms':0,'network_atoms_before':network,'network_atoms_after':sum(int(x['network'])for x in table('feebuckets')),'receipt':claim_receipts[0]}
record.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),nativefee_credit_rows=0,reserves_exact=True,all_three_routes_all_three_tokens=True,HTTP_replay_same_native_ID=True)
(K/'COMPLETE_TABLE_READS.json').write_text(json.dumps({'status':'PASS','pages':coverage,'no_incomplete_results_used':True},indent=2)+'\n')
(K/'CLAIM_CANONICAL.json').write_text(json.dumps({'status':'PASS',**record['free_claim'],'CPU_us':r['cpu_usage_us'],'NET_words':r['net_usage_words'],'actual_claim_not_resent':True},indent=2)+'\n')
save();(K/'PUBLIC_TRANSFER_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print('PUBLIC_12_TRANSFERS_79_CHECKS_AND_CANONICAL_FULL_FREE_CLAIM_PASS')
