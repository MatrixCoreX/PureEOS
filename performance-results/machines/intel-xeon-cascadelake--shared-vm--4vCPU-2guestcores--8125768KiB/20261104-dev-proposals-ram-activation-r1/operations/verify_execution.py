"""Verify actual irreversible execution block and elapsed native review time."""
from pathlib import Path
import datetime,json,urllib.request
K=Path('/opt/pureeos-dev-proposals-20261104-r1');p=json.loads((K/'PROPOSAL.json').read_text());e=json.loads((K/'EXECUTION.json').read_text())
assert e['status']=='UPGRADED_IRREVERSIBLE'
def rpc(method,data={}):
 return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+method,data=json.dumps(data).encode(),headers={'Content-Type':'application/json'}),timeout=40))
b=rpc('get_block',{'block_num_or_id':e['canonical_receipt']['block_num']});info=rpc('get_info')
assert b['id']==e['canonical_receipt']['block_id']and b['block_num']<=info['last_irreversible_block_num']
r=next(t for t in b['transactions']if(t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==e['transaction_id']);assert r['status']=='executed'
timestamp=datetime.datetime.fromisoformat(b['timestamp']);gov=p['governance'];assert timestamp>=datetime.datetime.fromisoformat(gov['execute_after'])
elapsed=(timestamp-datetime.datetime.fromisoformat(gov['proposed_at'])).total_seconds();assert elapsed>=1800
result={'status':'PASS','transaction_id':e['transaction_id'],'block_num':b['block_num'],'block_id':b['id'],'timestamp':b['timestamp'],'observed_LIB':info['last_irreversible_block_num'],
 'review_elapsed_seconds':elapsed,'review_seconds_required':1800,'CPU_usage_us':r['cpu_usage_us'],'NET_usage_words':r['net_usage_words'],'native_receipt_status':r['status']}
(K/'EXECUTION_CANONICAL.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
