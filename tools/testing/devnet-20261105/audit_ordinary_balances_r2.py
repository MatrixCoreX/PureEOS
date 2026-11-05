"""Audit90 own ordinary balances during a stable drained interval, no service pause."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import datetime,json,time,urllib.request
R=Path('/opt/pureeos-devnet-20261105-r1');O=R/'FINAL_ORDINARY90_BALANCES.json';assert not O.exists();deadline=time.monotonic()+600
actors=[x['name']for x in json.loads((R/'voting/PLAN.json').read_text())['voters']];assert len(actors)==90
def rpc(n,d={}):return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+n,json.dumps(d).encode(),{'Content-Type':'application/json'}),timeout=15))
def cash(a):
 x=rpc('get_currency_balance',{'code':'eosio.token','account':a,'symbol':'AIC'});return int(x[0].split()[0].replace('.',''))if x else 0
while True:
 assert time.monotonic()<deadline,'No stable drained interval observed'
 s=json.loads((R/'activity/STATE.json').read_text())
 if s.get('pending')is not None:time.sleep(1);continue
 info=rpc('get_info')
 with ThreadPoolExecutor(max_workers=4)as p:balances=dict(zip(actors,p.map(cash,actors)))
 after=json.loads((R/'activity/STATE.json').read_text())
 if after.get('pending')is not None or after['completed_batches']!=s['completed_batches']:time.sleep(.5);continue
 assert balances==s['balances']==after['balances']and sum(balances.values())==s['initial_ordinary_liquid_total_smallest_units']+int(s.get('external_credit_smallest_units',0))
 out={'status':'PASS','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'ordinary_accounts':90,'completed_batches':s['completed_batches'],'business_transactions':s['business_transactions'],'mix':{k:s[k]for k in ['transfers','ticks','notes']},'balances':balances,'exact_liquid_total_smallest_units':sum(balances.values()),'original_initial_total_smallest_units':s['initial_ordinary_liquid_total_smallest_units'],'known_external_credit_smallest_units':int(s.get('external_credit_smallest_units',0)),'external_adjustments':s.get('external_adjustments',[]),'ordinary_source_SHA256':s['source_history'][-1]['SHA256'],'last_completed_batch':s['last_completed_batch'],'scope':'Stable pending=None before/after same completed counter; all90 actual native balances equal sender journal, sum equals preserved initial total plus explicitly proven external credit. No BP/business pause, no payment replay or TPS claim.'}
 O.write_text(json.dumps(out,indent=2)+'\n');print('FINAL_ORDINARY90_NATIVE_BALANCES_MATCH_PASS');break
