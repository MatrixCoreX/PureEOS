"""Reconcile exactly one known canonical inbound credit, no new payment or native balance write."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import datetime,hashlib,json,os,signal,subprocess,time,urllib.request
R=Path('/opt/pureeos-devnet-20261105-r1');A=R/'activity';J=R/'ORDINARY_MARKER_RECONCILIATION.json';assert not J.exists();txid='09f0eb17fd5be203b25f4b0b80c5552dc46178780096ff51385cbad9e092fec4';unit='pureeos-devnet-ordinary-activity-20261105.service'
def rpc(n,d={}):return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8888/v1/chain/'+n,json.dumps(d).encode(),{'Content-Type':'application/json'}),timeout=15))
with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+txid,timeout=20)as x:hist=json.load(x)
root=[a for a in hist['actions']if a['act']['account']=='eosio.token'and a['act']['name']=='transfer'and a['action_ordinal']==1];assert len(root)==1;root=root[0];d=root['act']['data'];assert d['from']=='gptgenesisbp'and d['to']=='civicuserone'and d['quantity']=='0.0001 AIC'and d['memo']=='TEST native proposal execution resources'
b=rpc('get_block',{'block_num_or_id':root['block_num']});assert b['id']==root['block_id']and rpc('get_info')['last_irreversible_block_num']>=b['block_num'];matches=[x for x in b['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==txid];assert len(matches)==1 and matches[0]['status']=='executed'
record={'status':'DRAINING_OWN_ORDINARY_SENDER_ONLY','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'canonical_credit':{'transaction_id':txid,'block_num':b['block_num'],'block_id':b['id'],'action_ordinal':1,'from':d['from'],'to':d['to'],'amount_atomic':1,'quantity':d['quantity']},'node_PIDs_before':{x['unit']:int(subprocess.check_output(['systemctl','show',x['unit'],'-p','MainPID','--value'],text=True))for x in json.loads((R/'DEPTH_RUNTIME_UPGRADE.json').read_text())['completed']}}
def save():J.write_text(json.dumps(record,indent=2)+'\n')
save();deadline=time.monotonic()+240
while True:
 assert time.monotonic()<deadline,'No drained ordinary interval';pid=int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True));assert pid>0;os.kill(pid,signal.SIGSTOP)
 s=json.loads((A/'STATE.json').read_text())
 if s.get('pending')is None:break
 os.kill(pid,signal.SIGCONT);time.sleep(1)
subprocess.run(['systemctl','stop','--no-block',unit],check=True)
try:os.kill(pid,signal.SIGCONT)
except ProcessLookupError:pass
while int(subprocess.check_output(['systemctl','show',unit,'-p','MainPID','--value'],text=True)):
 assert time.monotonic()<deadline;time.sleep(.1)
s=json.loads((A/'STATE.json').read_text());assert s.get('pending')is None and not s.get('external_adjustments');before=(A/'STATE.json').read_bytes();(A/'STATE.before-canonical-marker-credit.json').write_bytes(before)
actors=list(s['balances'])
def cash(a):
 x=rpc('get_currency_balance',{'code':'eosio.token','account':a,'symbol':'AIC'});return int(x[0].split()[0].replace('.',''))if x else 0
with ThreadPoolExecutor(max_workers=4)as p:actual=dict(zip(actors,p.map(cash,actors)))
expected=dict(s['balances']);expected['civicuserone']+=1;assert actual==expected and sum(actual.values())==s['initial_ordinary_liquid_total_smallest_units']+1,'Only the exact proven one-atom delta is allowed'
source=A/'sender.py';old=source.read_bytes();(A/'sender.before-canonical-marker-credit.py').write_bytes(old);text=old.decode();needle="sum(S['balances'].values())==S['initial_ordinary_liquid_total_smallest_units']";assert text.count(needle)==1;text=text.replace(needle,"sum(S['balances'].values())==S['initial_ordinary_liquid_total_smallest_units']+int(S.get('external_credit_smallest_units',0))");source.write_text(text)
s['balances']=expected;s['external_credit_smallest_units']=1;s['external_adjustments']=[record['canonical_credit']];s['last_journal_reconciliation_UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();tmp=A/'STATE.credit-repair.pending';tmp.write_text(json.dumps(s,indent=2)+'\n');os.replace(tmp,A/'STATE.json')
q=A/'QUALIFIED.json';(A/'QUALIFIED.before-canonical-marker-credit.json').write_bytes(q.read_bytes());qual=json.loads(q.read_text());qual['sender_SHA256']=hashlib.sha256(source.read_bytes()).hexdigest();qual['journal_correction']='One proven existing proposal-marker inbound credit; no new chain writes, transfers or initial-total rewrite';q.write_text(json.dumps(qual,indent=2)+'\n')
record.update(status='REPAIRED_EXACT_CANONICAL_CREDIT_NO_CHAIN_MUTATION',original_initial_total=s['initial_ordinary_liquid_total_smallest_units'],external_credit_smallest_units=1,actual_total=sum(actual.values()),original_state_SHA256=hashlib.sha256(before).hexdigest(),old_sender_SHA256=hashlib.sha256(old).hexdigest(),new_sender_SHA256=qual['sender_SHA256'],original_initial_total_preserved=True,all90_balances_match=True,old_failures_retained=True,scope='Only derived ordinary journal accounts for existing owner-authorized inbound credit. Native balances/keys/votes/bonds/caps/nodes/history/relay untouched; old errors retained.')
subprocess.run(['systemctl','start',unit],check=True);record['resumed_UTC']=datetime.datetime.now(datetime.timezone.utc).isoformat();record['source_counter_before']=s['completed_batches'];save()
while True:
 new=json.loads((A/'STATE.json').read_text())
 if new['completed_batches']>s['completed_batches']and new.get('pending')is None:break
 assert time.monotonic()<deadline;time.sleep(1)
assert all(int(subprocess.check_output(['systemctl','show',u,'-p','MainPID','--value'],text=True))==p for u,p in record['node_PIDs_before'].items());record.update(status='PASS',verified_following_batch=new['completed_batches'],native_node_PIDs_unchanged=True,finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat());save();print('EXACT_CANONICAL_MARKER_CREDIT_JOURNAL_REPAIR_AND_FOLLOWING_REAL_BATCH_PASS')
