"""Recover first immutable canonical payment; run only unsubmitted test tail."""
from pathlib import Path
import json,subprocess,sys,traceback,datetime
R=Path('/opt/pureeos-key-unbounded-20261107-r1');sys.path.insert(0,str(R/'controllers'))
import ops_common as o
assert not(R/'PUBLIC_QA_RESUME_STARTED.json').exists(),'Never repeat recovery or paid tail'
assert subprocess.check_output(['systemctl','show','pureeos-key-growth-postqa-20261107.service','-p','MainPID','--value'],text=True).strip()=='0'
old=(R/'PUBLIC_QA_PROGRESS.json').read_bytes();prior=json.loads(old);assert prior['status']=='NEEDS_CANONICAL_RECONCILIATION'and not prior['cases']
proof=json.loads((R/'FIRST_PAYMENT_CANONICAL_RECONCILIATION.json').read_text());assert proof['status']=='PASS'and proof['same_original_ID']
payment=proof['canonical_receipt'];tid=payment['transaction_id'];b=o.rpc('get_block',{'block_num_or_id':payment['block_num']});assert b['id']==payment['block_id']and any((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==tid and t['status']=='executed'for t in b['transactions'])
o.guard();o.require_codes(o.NEW);o.authority_guard();o.save(R/'PUBLIC_QA_RESUME_STARTED.json',{'status':'CANONICAL_FIRST_ID_VERIFIED_SKIP_IT','transaction_id':tid,'canonical_receipt':payment,'no_new_keys_or_first_payment_replay':True});(R/'PUBLIC_QA_INTERRUPTED.json').write_bytes(old)
source=(R/'controllers/public_qa.py').read_text();prefix=source[:source.index('\ntry:\n assert not')];exec(compile(prefix,'qualified-public-qa-helpers-only','exec'),globals());record=prior
try:
 record['initial_failure']=record.pop('error');record['status']='RECOVERING_CANONICAL_FIRST_PAYMENT';save()
 keys=json.loads((R/'public-qa-private/keys.json').read_text());assert{k:v['Public key']for k,v in keys.items()}==record['public_keys'];A=keys['A'];acts=history(tid);fees=unique(acts,'eosio.key','feereceipt');assert len(fees)==1;fee=fees[0]
 buys=[x for x in unique(acts,'eosio','logbuyram')if x['payer']==x['receiver']=='eosio.key'];assert len(buys)==1
 quota=o.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'];oldquota=json.loads((R/'governance/key/REVIEW.json').read_text())['before_target_RAM']['ram_quota']
 check(int(fee['allocated_bytes'])==1536 and int(fee['purchased_bytes'])==int(buys[0]['bytes'])==int(quota)-int(oldquota)>=1536,'Recovered first: real native RAM budget and acquired quota')
 check(atoms(fee['ram_payment'])==atoms(buys[0]['quantity'])and atoms(fee['total'])==atoms(fee['ram_payment'])+atoms(fee['retained_base'])+atoms(fee['relay_fee'])and atoms(fee['total'])<=10000,'Recovered first: exact bounded actual fee equation')
 receipts=unique(acts,'eosio.key','receipt');check(len(receipts)==1 and not receipts[0]['relayer']and atoms(fee['relay_fee'])==0,'Recovered first: direct native route has no commercial relay')
 check(balance(A)==20000,'Recovered first: full2AIC principal credited exactly once')
 counts=table('growth')[0];base=record['legacy_counts_before'];check(int(counts['wallet_count'])==int(base['wallet_count'])+1 and int(counts['balance_count'])==int(base['balance_count'])+1 and int(counts['asset_count'])==int(base['asset_count']),'Recovered first: lazy exact counters initialized once')
 reserves();record['cases'].append({'label':'named-to-key AIC','transaction_id':tid,'canonical_receipt':payment,'fee':fee,'native_KEY_RAM':buys,'KEY_quota_delta':int(quota)-int(oldquota),'SDK_quote':None,'own_new_relay_income_atoms':0,'growth_before':record['before_growth'],'growth_after':table('growth'),'recovery':'Same original signed bytes after explicit CPU rejection/canonical absence; actual canonical block268332, reported hint268331 not assumed. Original ephemeral SDK quote and before-fee-bucket values were not durably captured, so no independent first-case historical-bucket delta/quote equality is claimed.'});record['status']='RUNNING_RECOVERED_TAIL';record['reconciliation']={'first_original_ID':tid,'canonical_receipt':payment,'same_packet_only_retry':True,'all_previously_successful_payments_skipped':True};save()
 progress=json.loads((R/'public-qa-packets/deposit-0.json').read_text());progress.update(status='IRREVERSIBLE_CANONICAL_RECOVERED',canonical_receipt=payment);o.save(R/'public-qa-packets/deposit-0.json',progress)
 # Reopen ONLY the existing owned QA wallet, validating its sole public key.
 session_source=(R/'controllers/native_session.py').read_text();session_source=session_source.replace("password=self.W/'password.txt';assert not password.exists(),'New isolated signer only; do not replay'","password=self.W/'password.txt';assert password.exists() and (o.R/'PUBLIC_QA_RESUME_STARTED.json').exists(),'Existing isolated QA signer required'")
 original="self.run(['wallet','create','-n','growthqa','--file',str(password)]);self.wallet('import_key',['growthqa',role['Private key']]);role=None"
 replacement="self.run(['wallet','open','-n','growthqa']);self.wallet('unlock',['growthqa',password.read_text().strip()]);pubkeys=self.wallet('get_public_keys',[]);assert len(pubkeys)==1 and role['Public key'] in self.run(['convert','public_key',pubkeys[0]]),'Only the original owned funder key';role=None"
 assert original in session_source;session_source=session_source.replace(original,replacement);ns={};exec(compile(session_source,'controlled-existing-owned-signer-only','exec'),ns);NativeSession=ns['NativeSession']
 tail=source[source.index('  def deposit(k,q,'):source.index('\nexcept BaseException:')]
 needle="  for token,sym,budget in [('eosio.token','AIC',1536),('usd','USD',512),('cny','CNY',512)]:"
 assert needle in tail;tail=tail.replace(needle,needle+"\n   if sym=='AIC':continue # Exact original first payment already irreversibly verified; never resend")
 code="with NativeSession()as session:\n  A,B,C=[keys[n]for n in ['A','B','C']];named='keygrowcheck'\n"+tail+'\n';exec(compile(code,'only-unsubmitted-owned-QA-tail','exec'),globals())
 assert json.loads((R/'PUBLIC_QA_RESULT.json').read_text())['status']=='PASS'
 with(R/'final-audit-r2.log').open('w')as log:subprocess.run(['python3','-B',str(R/'controllers/final_audit.py')],stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
 o.save(R/'POST_QA_AND_AUDIT.json',{'status':'PASS','QA_result_SHA256':o.sha((R/'PUBLIC_QA_RESULT.json').read_bytes()),'final_audit_SHA256':o.sha((R/'FINAL_AUDIT.json').read_bytes()),'retained_initial_failure':True,'first_ID_reconciled_not_resigned':tid,'post_browser_and_Git_finalization':'Separate'})
except BaseException:
 record.update(status='NEEDS_CANONICAL_RECONCILIATION',resume_error=traceback.format_exc());save();o.save(R/'PUBLIC_QA_RESUME_FAILURE.json',{'status':'FAIL','error':traceback.format_exc(),'never_repeat_successful_cases':True});raise
