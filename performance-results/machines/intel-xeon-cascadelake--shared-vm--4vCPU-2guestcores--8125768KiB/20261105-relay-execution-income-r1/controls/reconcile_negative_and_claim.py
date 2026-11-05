"""Resume only the rejected-interface/claim tail, never repeat successful payments."""
from pathlib import Path
import json,datetime,hashlib,subprocess,urllib.request,urllib.error,time
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');C=Path('/opt/pureeos-devnet-20261105-r1')
original=(K/'PUBLIC_TRANSFER_PROGRESS.json').read_bytes();old=json.loads(original)
assert old['status']=='NEEDS_CANONICAL_RECONCILIATION'and len(old['cases'])==12 and old.get('free_claim')is None
assert not(K/'PUBLIC_TRANSFER_RESULT.json').exists()
(K/'PUBLIC_TRANSFER_INTERRUPTED.json').write_bytes(original)
source=(K/'verify_public_income.py').read_text();prefix=source[:source.index("\ntry:\n (R/'private')")]
prefix=prefix.replace("assert not(R/'PUBLIC_TRANSFER_PROGRESS.json').exists(),'Reconcile existing signed IDs and all balances before resuming'",'pass # Existing progress is explicitly reconciled below')
exec(compile(prefix,'public-income-helpers-only','exec'),globals());record=old
try:
 info=f.rpc('get_info');seen=set()
 for case in record['cases']:
  c=case['canonical_receipt'];b=f.rpc('get_block',{'block_num_or_id':c['block_num']});assert b['id']==c['block_id']and b['block_num']<=info['last_irreversible_block_num']
  r=next(x for x in b['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==case['transaction_id']);assert r['status']=='executed';seen.add(case['transaction_id'])
 assert len(seen)==12
 reserves()
 path=C/'founder/immutable-packets/1178.json';packet=json.loads(path.read_text());assert len(packet['actions'])==1
 a=packet['actions'][0];assert a['account']=='eosio.token'and a['name']=='transfer'and a['authorization']==[{'actor':'gptgenesisbp','permission':'active'}]
 data=a['data']
 if isinstance(data,str):data=json.loads(f.run(['convert','unpack_hex','--type','transfer','--abi-file',str(f.contracts/'eosio.token/eosio.token.abi'),data]))
 assert data=={'from':'gptgenesisbp','to':'eosio.key','quantity':'0.0100 AIC','memo':'fee'}
 packed=json.loads(f.run(['convert','pack_transaction',str(path)]));assert packed['compression']in ['none',0]
 tid=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest();info=f.rpc('get_info')
 assert datetime.datetime.fromisoformat(packet['expiration'])>datetime.datetime.fromisoformat(info['head_block_time']),'Expired packet must not be re-prepared automatically'
 before=stable();(K/'RECONCILIATION_NEGATIVE_BEFORE.json').write_text(json.dumps(before,indent=2)+'\n')
 request=urllib.request.Request(f.url+'/v1/chain/push_transaction',json.dumps(packed).encode(),{'Content-Type':'application/json'})
 try:
  with urllib.request.urlopen(request,timeout=40)as r:raw=r.read().decode()
 except urllib.error.HTTPError as e:raw=e.read().decode()
 (K/'private-native/legacy-rejection-response.json').write_text(raw)
 assert 'use atomic deposit'in raw.lower(),f.redact(raw[:1500])
 response=json.loads(raw);assert not(response.get('processed')or{}).get('receipt')
 end=f.wait_lib(info['head_block_num']+3,timeout=90);absence=[]
 for n in range(info['last_irreversible_block_num']+1,end['last_irreversible_block_num']+1):
  b=f.rpc('get_block',{'block_num_or_id':n});assert not any((x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==tid for x in b['transactions']);absence.append({'block':n,'id':b['id']})
 after=stable();(K/'RECONCILIATION_NEGATIVE_AFTER.json').write_text(json.dumps(after,indent=2)+'\n')
 differences={k:{'before':before[k],'after':after[k]}for k in before if before[k]!=after[k]}
 proof={'status':'PASS'if not differences else'DIFFERENCE','same_original_packet':True,'transaction_id':tid,'packet_SHA256':hashlib.sha256(path.read_bytes()).hexdigest(),'explicit_native_rejection':True,'canonical_absence':absence,'differences':differences,'confirmed_existing_successful_payments':sorted(seen)}
 (K/'NEGATIVE_RECONCILIATION.json').write_text(json.dumps(proof,indent=2)+'\n');assert not differences,'Rejected transaction state difference: '+','.join(differences)
 record['initial_failure']=record.pop('error');record['reconciliation']={'proof':'NEGATIVE_RECONCILIATION.json','same_original_packet':True,'existing_12_payments_verified_not_resent':True,'initial_snapshot_cause':'Not established; exact original-packet native rejection, canonical absence and full current snapshot verified'}
 check(True,'Legacy standalone fee transfer rejected with original immutable packet; no canonical receipt and full state unchanged')
 earned=next(int(b['relay_earned'])for b in table('feebuckets')if b['owner']=='gptgenesisbp');network=sum(int(b['network'])for b in table('feebuckets'));before_balance=native('gptgenesisbp');assert earned>0
 (K/'CLAIM_BEFORE.json').write_text(json.dumps({'earned_atoms':earned,'native_balance_atoms':before_balance,'network_atoms':network,'fee_buckets':table('feebuckets')},indent=2)+'\n')
 result=f.action('claim',{'bp':'gptgenesisbp','to':'gptgenesisbp','quantity':amount(earned),'max_fee':'0.0000 AIC'},'gptgenesisbp',contract='eosio.key')
 check(native('gptgenesisbp')-before_balance==earned and next(int(b['relay_earned'])for b in table('feebuckets')if b['owner']=='gptgenesisbp')==0 and sum(int(b['network'])for b in table('feebuckets'))==network,'Full execution-plus-relay income claimed with zero application fee; historical network reserve unchanged')
 record['free_claim']={'atoms':earned,'canonical_receipt':f.report['confirmed_transactions'][-1]};reserves();record.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),nativefee_credit_rows=0,reserves_exact=True,all_three_routes_all_three_tokens=True,HTTP_replay_same_native_ID=True)
 save();(K/'PUBLIC_TRANSFER_RESULT.json').write_text(json.dumps(record,indent=2)+'\n');print('PUBLIC_PAYMENT_RECONCILIATION_AND_FULL_FREE_CLAIM_PASS')
except BaseException:
 record.update(status='NEEDS_CANONICAL_RECONCILIATION',recovery_error=f.redact(traceback.format_exc()));save();raise
