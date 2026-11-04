"""Read-only exact ledger/config/permission comparison across native devexec."""
from pathlib import Path
import datetime,json,sys,urllib.request,urllib.error,time
K=Path('/opt/pureeos-dev-proposals-20261104-r1');phase=sys.argv[1];assert phase in ['before','after-upgrade']
def rpc(method,body={}):
 for attempt in range(3):
  try:return json.load(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+method,data=json.dumps(body).encode(),headers={'Content-Type':'application/json'}),timeout=40))
  except urllib.error.HTTPError as error:
   with(K/'LEDGER_READ_RETRIES.ndjson').open('a')as log:log.write(json.dumps({'method':method,'body':body,'attempt':attempt+1,'status':error.code,'response':error.read().decode()})+'\n')
   if error.code<500 or attempt==2:raise
   time.sleep(1)
if phase=='after-upgrade':
 for attempt in range(30):
  if rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']=='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1':break
  time.sleep(1)
def rows(name,scope='eosio.key'):
 r=rpc('get_table_rows',{'json':True,'code':'eosio.key','scope':scope,'table':name,'limit':1000});assert not r['more'];return r['rows']
stable={n:rows(n)for n in ['config','wallets','assets','nativefees','feebuckets','relays','relayrates','pricing']}
stable['balances']={str(w['id']):rows('balances',str(w['id']))for w in stable['wallets']}
stable['permissions']={a:rpc('get_account',{'account_name':a})['permissions']for a in ['eosio.key','civicgovlead','gptgenesisbp']}
stable['USD_code']=rpc('get_code_hash',{'account_name':'keyusdtoken1'})['code_hash']
record={'status':'PASS','phase':phase,'stable':stable,'raw_abi':rpc('get_raw_abi',{'account_name':'eosio.key'}),'native_ram_usage':rpc('get_account',{'account_name':'eosio.key'})['ram_usage'],
 'chain_id':rpc('get_info')['chain_id'],'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()}
if phase=='before':assert not(K/'LEDGER_before.json').exists()
else:
 before=json.loads((K/'LEDGER_before.json').read_text());assert before['stable']==stable,'Ledger/config/permission state changed during devexec'
 assert record['raw_abi']['abi_hash']==before['raw_abi']['abi_hash'];assert record['raw_abi']['code_hash']=='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
 record['exact_ledger_config_permissions_preserved']=True
(K/('LEDGER_'+phase+'.json')).write_text(json.dumps(record,indent=2)+'\n');print(json.dumps({'status':'PASS','phase':phase,'wallets':len(stable['wallets']),'assets':len(stable['assets']),'native_RAM':record['native_ram_usage']}))
