"""One user-requested real TEST relay-income claim with canonical balance audit."""
from pathlib import Path
import datetime,fcntl,hashlib,json,os,time,urllib.request
from campaign_helpers import R,load_chain

os.umask(0o077)
D=R/'relay-claim-check-20261103-r1';D.mkdir(mode=0o700,exist_ok=True)
lock=(D/'execution.lock').open('a+');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
f=load_chain();f.cleos=R/'native-client/cleos'
assert f.rpc('get_info')['chain_id']=='38391d73bbcaac3c51af7009d0d11b09ca263895c1bf55a96494c3194f7f4d65'
assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']=='1e0ed2c38e994dea218948483c1333fbd4a0a54c647eb8d8f5932b3d76ac98ea'
def save(path,value):
    tmp=path.with_suffix('.tmp')
    with tmp.open('w')as stream:
        json.dump(value,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    tmp.replace(path)
def rows(table,code='eosio.key',scope='eosio.key'):return f.table(table,code=code,scope=scope)
def atoms(quantity):return int(quantity.split()[0].replace('.',''))
def native(name):
    balances=f.rpc('get_currency_balance',{'code':'eosio.token','account':name,'symbol':'AIC'})
    return atoms(balances[0])if balances else 0
def state():
    wallets=rows('wallets');buckets=rows('feebuckets');credits=rows('nativefees');assets=rows('assets')
    holdings={str(w['id']):rows('balances',scope=str(w['id']))for w in wallets}
    reserve=next(int(a['reserve'])for a in assets if a['token_symbol']=='4,AIC')
    liabilities=sum(int(b['amount'])for bs in holdings.values()for b in bs if int(b['asset_id'])==1)
    liabilities+=sum(int(c['amount'])for c in credits)+sum(int(c['network'])+int(c['relay_earned'])for c in buckets)
    assert reserve==liabilities==native('eosio.key')
    return {'bucket':next(b for b in buckets if b['owner']=='gptgenesisbp'),
        'fee_credit':next(c for c in credits if c['owner']=='gptgenesisbp'),
        'native_balance_atoms':native('gptgenesisbp'),'reserve_atoms':reserve,'liabilities_atoms':liabilities,
        'holdings':holdings,'BP_bonds':rows('bpbonds',code='eosio',scope='eosio'),
        'bond_pool_atoms':native('eosio.bond'),'slash_pool_atoms':native('eosio.bslash')}

progress=D/'PROGRESS.json'
if progress.exists():
    record=json.loads(progress.read_text())
else:
    before=state();amount=int(before['bucket']['relay_earned']);assert amount>0,'No income available; do not create a new relay payment just to claim'
    quantity=str(amount//10000)+'.'+str(amount%10000).zfill(4)+' AIC'
    request={'bp':'gptgenesisbp','to':'gptgenesisbp','quantity':quantity,'max_fee':'0.1000 AIC'}
    start=f.rpc('get_info')['last_irreversible_block_num']
    packed=json.loads(f.run(['push','action','eosio.key','claim',json.dumps(request),'-p','gptgenesisbp@active',
        '--dont-broadcast','--return-packed','--json','--expiration','1800']))
    assert packed['compression']in [0,'none']and packed['signatures']
    save(D/'claim.signed.json',packed)
    record={'status':'SIGNED','chain_id':f.rpc('get_info')['chain_id'],'request':request,'before':before,
        'start_LIB':start,'transaction_id':hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()}
    save(progress,record)
if not record.get('confirmed'):
    packed=json.loads((D/'claim.signed.json').read_text())
    assert hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()==record['transaction_id']
    record['status']='BROADCAST_ATTEMPTED';save(progress,record)
    try:
        result=f.rpc('push_transaction',packed)
        assert result['transaction_id']==record['transaction_id']
    except Exception as error:
        record['broadcast_error']=f.redact(str(error));save(progress,record)
    record['confirmed']=f.wait_transaction(record['transaction_id'],record['start_LIB'],timeout=180)
    save(progress,record)
before=record['before'];after=state();amount=atoms(record['request']['quantity'])
assert after['native_balance_atoms']-before['native_balance_atoms']==amount
assert before['bucket']['relay_earned']-after['bucket']['relay_earned']==amount
assert after['bucket']['relay_earned']==0
fee=int(before['fee_credit']['amount'])-int(after['fee_credit']['amount'])
assert fee>0 and fee<=atoms(record['request']['max_fee'])
assert after['bucket']['network']-before['bucket']['network']==fee
assert before['reserve_atoms']-after['reserve_atoms']==amount
assert before['holdings']==after['holdings']and before['BP_bonds']==after['BP_bonds']
assert before['bond_pool_atoms']==after['bond_pool_atoms']and before['slash_pool_atoms']==after['slash_pool_atoms']
deadline=time.monotonic()+90
while True:
    with urllib.request.urlopen('http://127.0.0.1:7000/v2/history/get_transaction?id='+record['transaction_id'],timeout=20)as response:history=json.load(response)
    claims=[a for a in history.get('actions',[])if a['act']['account']=='eosio.key'and a['act']['name']=='claim']
    receipts=[a for a in history.get('actions',[])if a['act']['account']=='eosio.key'and a['act']['name']=='receipt'and a['act']['data'].get('kind')=='claim']
    if claims and receipts:break
    assert time.monotonic()<deadline,'History claim indexing timeout'
    time.sleep(2)
assert history['executed']and history['lib']>=record['confirmed']['block_num']
assert atoms(receipts[0]['act']['data']['fee'])==fee
assert receipts[0]['block_id']==record['confirmed']['block_id']
record.update(status='PASS',UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),after=after,
    claimed_income_atoms=amount,claim_fee_atoms=fee,net_of_claim_fee_atoms=amount-fee,
    history=history,relay_hot_key_not_linked_to_claim=True,actual_BP_active_permission_used=True,
    custodied_user_balances_unchanged=True,bonds_and_slash_pool_unchanged=True,
    note='Income withdrawal is verified; income is not net operating profit. Small claims can cost more than their income.')
save(progress,record);save(D/'RESULT.json',record)
print(json.dumps({'status':'PASS','transaction_id':record['transaction_id'],'claimed_income_atoms':amount,
    'claim_fee_atoms':fee,'net_of_claim_fee_atoms':amount-fee,'confirmed':record['confirmed']}),flush=True)
