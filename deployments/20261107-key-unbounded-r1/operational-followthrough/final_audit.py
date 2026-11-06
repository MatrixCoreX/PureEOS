"""Read-only post-upgrade integrity audit after new owned functional QA."""
from pathlib import Path
import json,datetime,subprocess
import ops_common as o
from stable_snapshot import snapshot
R=o.R;before=json.loads((R/'STABLE_BEFORE.json').read_text());o.require_codes(o.NEW);o.authority_guard()
qa=json.loads((R/'PUBLIC_QA_RESULT.json').read_text());assert qa['status']=='PASS'
execution=json.loads((R/'governance/key/EXECUTION.json').read_text());assert execution['status']=='UPGRADED_IRREVERSIBLE'
proposal=json.loads((R/'governance/key/PROPOSAL.json').read_text());elapsed=(o.stamp(execution['canonical_receipt']['timestamp'])-o.stamp(proposal['governance']['proposed_at'])).total_seconds();assert elapsed>=1800
for case in[*qa['cases'],qa['claim']]:
 receipt=case['canonical_receipt'];block=o.rpc('get_block',{'block_num_or_id':receipt['block_num']});assert block['id']==receipt['block_id'];matches=[t for t in block['transactions']if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==case['transaction_id']];assert len(matches)==1 and matches[0]['status']=='executed'
after=snapshot();assert after['permissions']==before['permissions']and after['token_stats']==before['token_stats']and after['consensus']==before['consensus']
assert all(after['codes'][n]==h for n,h in before['codes'].items()if n!='eosio.key')
for n,row in before['processes'].items():
 if o.native_unit(n):assert after['processes'][n]==row
assert len(after['native_schedule']['active']['producers'])==len(before['native_schedule']['active']['producers'])==21
cfg=after['KEY_config'][0];old=before['KEY_config'][0]
for k,v in old.items():
 if k not in ['next_wallet','next_asset','wallet_count','balance_count','asset_count']:assert cfg[k]==v,k
assert not o.rows('eosio.key','eosio.key','pending')and not o.rows('eosio.key','eosio.key','nativefees')and not o.rows('eosio.signup','eosio.signup','pendingkey')
baseline=json.loads((R/'LIVE_BASELINE.json').read_text());since=baseline['time_UTC'];log=subprocess.run(['journalctl','-k','--since',since,'--no-pager','-o','cat'],capture_output=True,text=True);assert log.returncode==0
new_oom=[s for s in log.stdout.splitlines()if any(x in s.lower()for x in ['out of memory:','oom-kill:','killed process'])];assert not new_oom,('New kernel OOM requires investigation',len(new_oom))
o.save(R/'FINAL_AUDIT.json',{'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'review_elapsed_chain_seconds':elapsed,'native_processes_unchanged':sum(o.native_unit(n)for n in before['processes']),'original_monitored_authorities_unchanged':len(before['permissions']),'native_consensus_unchanged':True,'active_BP_count':21,'token_stats_unchanged':after['token_stats'],'other_codes_unchanged':True,'no_new_kernel_OOM_since_task_baseline':True,'all_new_QA_payments_canonically_verified':len(qa['cases']),'new_claim_canonically_verified':True,'no_persistent_fee_or_transient_pending_rows':True,'before':before,'after':after,'scope':'Existing global/native invariants and exact new functional receipts; ordinary sender automatic restarts separately observed, not repaired and no zero-rejection/TPS/full Android claim'})
print('FINAL_READONLY_INTEGRITY_PASS')
