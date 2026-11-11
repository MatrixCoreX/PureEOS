"""One endpoint-only regrelay action; preserve BP/key/rate/backend/journal. No financial transfer."""
from pathlib import Path
import json,datetime,hashlib
import ops_common as o
from admin_session import AdminSession
R=o.R;BP='gptgenesisbp';NEW='https://relay1.matrixai.one';OLD='https://aic-test.matrixai.one/relay'
assert not(R/'CUTOVER_STARTED.json').exists(),'Reconcile original signed packet; never repeat'
assert json.loads((R/'TLS_RESULT.json').read_text())['status']=='PASS'
assert json.loads((R/'PHONE_UPDATE_BOTH_R31.json').read_text())['status']=='PASS'
assert json.loads((R/'WALLET_LIVE_READONLY.json').read_text())['status']=='PASS_EXACT_APK_READONLY_DISCOVERY'
o.guard();o.require_codes(o.NEW);o.authority_guard();base=json.loads((R/'BASELINE.json').read_text())
assert o.rpc('get_account',{'account_name':BP})['permissions']==base['BP_account']['permissions']
relays=o.rows('eosio.key','eosio.key','relays');rates=o.rows('eosio.key','eosio.key','relayrates');assert relays==base['relays']and rates==base['rates']
row=next(x for x in relays if x['bp']==BP);assert row['enabled']and row['permission']=='keyrelay'and row['endpoint']==OLD
assert int(next(x for x in rates if x['bp']==BP)['fee_bps'])==3000
journal=Path('/opt/pureeos-devnet-20261105-r1/relay-state/journal.json');jobs=json.loads(journal.read_text());original=json.loads((R/'JOURNAL_BEFORE.json').read_text())
assert hashlib.sha256(journal.read_bytes()).hexdigest()==original['SHA256']
assert all(x['status']in['finalized','expired']for x in jobs.values())
o.save(R/'CUTOVER_STARTED.json',{'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'BP':BP,'old_endpoint':OLD,'new_endpoint':NEW,'journal_SHA256':original['SHA256']})
with AdminSession()as signer:
 receipt=signer.send('relay1-endpoint-only',[{'account':'eosio.key','name':'regrelay','authorization':[{'actor':BP,'permission':'active'}],'data':{'bp':BP,'permission':'keyrelay','endpoint':NEW}}])
after=o.rows('eosio.key','eosio.key','relays');expected=[dict(x,endpoint=NEW)if x['bp']==BP else x for x in relays];assert after==expected
assert o.rows('eosio.key','eosio.key','relayrates')==rates
assert o.rpc('get_account',{'account_name':BP})['permissions']==base['BP_account']['permissions']
assert hashlib.sha256(journal.read_bytes()).hexdigest()==original['SHA256']
o.guard();o.require_codes(o.NEW);o.authority_guard()
o.save(R/'CUTOVER_RESULT.json',{'status':'PASS_IRREVERSIBLE_ENDPOINT_ONLY','canonical_receipt':receipt['canonical_receipt'],'relay':next(x for x in after if x['bp']==BP),'all_rates_unchanged':True,'all_permissions_unchanged':True,'original_journal_byte_exact':True,'backend_services_unchanged':True,'temporary_signer_closed':True})
print('RELAY1_ENDPOINT_CUTOVER_IRREVERSIBLE_PASS',receipt['transaction_id'])
