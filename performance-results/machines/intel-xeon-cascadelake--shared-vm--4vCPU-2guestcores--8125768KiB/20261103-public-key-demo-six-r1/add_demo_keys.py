"""User-requested six funded demo keys, three with new native accounts.

Runs only on the retained TEST host. Keys stay remote; immutable signed
transactions and canonical receipts make restart/reconciliation reviewable.
"""
from pathlib import Path
import datetime,fcntl,hashlib,json,os,time,urllib.parse,urllib.request
from campaign_helpers import R,load_chain,action,permission,import_key,transfer

os.umask(0o077)
CHAIN='38391d73bbcaac3c51af7009d0d11b09ca263895c1bf55a96494c3194f7f4d65'
D=R/'key-demo-six-20261103-r1';D.mkdir(mode=0o700,exist_ok=True)
lock=(D/'execution.lock').open('a+');fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
def write(path,value):
    temporary=path.with_suffix(path.suffix+'.tmp')
    with temporary.open('w')as stream:
        json.dump(value,stream,indent=2);stream.write('\n');stream.flush();os.fsync(stream.fileno())
    temporary.chmod(0o600);temporary.replace(path)

f=load_chain();f.cleos=R/'native-client/cleos'
assert f.rpc('get_info')['chain_id']==CHAIN
assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']=='1e0ed2c38e994dea218948483c1333fbd4a0a54c647eb8d8f5932b3d76ac98ea'
names=['keymapleseed','keyriverbank','keystarlight',None,None,None]
keyfile=R/'private/key-demo-six-20261103-r1-keys.json'
if keyfile.exists():
    keys=json.loads(keyfile.read_text())
    assert len(keys)==6 and[n['native_account']for n in keys]==names
else:
    keys=[{'index':i+1,'native_account':name,**f.key()}for i,name in enumerate(names)]
    write(keyfile,keys)
for key in keys:import_key(f,key)
progress=D/'PROGRESS.json'
record=json.loads(progress.read_text())if progress.exists()else{
    'status':'PREPARING','chain_id':CHAIN,'steps':{},'keys':[{'index':k['index'],'public_key':k['Public key'],'native_account':k['native_account']}for k in keys]}
assert record['chain_id']==CHAIN
write(progress,record)

def confirm_saved(receipt):
    block=f.rpc('get_block',{'block_num_or_id':receipt['block_num']})
    assert block['id']==receipt['block_id']
    assert f.rpc('get_info')['last_irreversible_block_num']>=receipt['block_num']
    assert any((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==receipt['transaction_id']and t['status']=='executed'for t in block['transactions'])

def execute(label,args):
    step=record['steps'].get(label)
    if step and step.get('receipt'):
        confirm_saved(step['receipt']);return step['receipt']
    signed_file=D/(label+'.signed.json')
    if not step:
        assert not signed_file.exists(),'Reconcile a prepared packet missing from the journal'
        start=f.rpc('get_info')['last_irreversible_block_num']
        packed=json.loads(f.run(args+['--dont-broadcast','--return-packed','--json','--expiration','1800']))
        assert packed['compression']in ['none',0]and packed['signatures']
        txid=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()
        write(signed_file,packed)
        step={'status':'SIGNED','transaction_id':txid,'start_LIB':start,'signed_file':str(signed_file)}
        record['steps'][label]=step;write(progress,record)
    packed=json.loads(signed_file.read_text())
    assert hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()==step['transaction_id']
    # Persist before sending. A restart reuses this SAME signed packet/ID.
    step['status']='BROADCAST_ATTEMPTED';write(progress,record)
    try:
        response=f.rpc('push_transaction',packed)
        assert response['transaction_id']==step['transaction_id']
    except Exception as error:
        step['broadcast_error']=f.redact(str(error));write(progress,record)
    receipt=f.wait_transaction(step['transaction_id'],step['start_LIB'],timeout=180)
    confirm_saved(receipt);step.update(status='IRREVERSIBLE',receipt=receipt);write(progress,record)
    print(json.dumps({'step':label,'transaction_id':receipt['transaction_id'],'irreversible_block':receipt['block_num']}),flush=True)
    time.sleep(1)
    return receipt

def native(owner,contract='eosio.token',symbol='AIC'):
    values=f.rpc('get_currency_balance',{'code':contract,'account':owner,'symbol':symbol})
    return values[0]if values else '0.0000 '+symbol

try:
    # Existing founder allocation only; no issue, ordinary90, votes or BP bonds.
    for key in keys:
        name=key['native_account']
        if not name:continue
        assert len(name)==12
        create_label='native-create-'+name
        if create_label not in record['steps']:
            # Any pre-existing account is an ownership collision, never adopt it.
            try:
                f.rpc('get_account',{'account_name':name})
            except RuntimeError as error:
                assert 'unknown key'in str(error).lower()or'unknown account'in str(error).lower()
            else:raise RuntimeError('Native account already exists: '+name)
        auth=permission(key['Public key'])
        execute(create_label,['push','transaction',json.dumps({'actions':[
            action('newaccount','gptgenesisbp',{'creator':'gptgenesisbp','name':name,'owner':auth,'active':auth}),
            action('buyram','gptgenesisbp',{'payer':'gptgenesisbp','receiver':name,'quant':'1.0000 AIC'})]})])
        execute('native-fund-'+name,['push','transaction',json.dumps({'actions':[
            transfer('gptgenesisbp',name,'125.0000 AIC','TEST own resources and native-account balance'),
            action('delegatebw',name,{'from':name,'receiver':name,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':'100.0000 AIC','transfer':False}),
            action('open',name,{'owner':name,'symbol':'4,USD','ram_payer':name},'keyusdtoken1'),
            action('transfer','gptgenesisbp',{'from':'gptgenesisbp','to':name,'quantity':'5.0000 USD','memo':'TEST separate native-account balance'},'keyusdtoken1')]})])
    for key in keys:
        for contract,symbol,quantity in [('eosio.token','AIC','10.0000 AIC'),('keyusdtoken1','USD','20.0000 USD')]:
            execute('key-'+str(key['index'])+'-'+symbol,['transfer','gptgenesisbp',key['Public key'],quantity,'-c',contract,'--max-key-fee','2.0000 AIC'])
    wallets=f.table('wallets',code='eosio.key',scope='eosio.key')
    assets={int(a['id']):a for a in f.table('assets',code='eosio.key',scope='eosio.key')}
    results=[]
    deadline=time.monotonic()+180
    for key in keys:
        wallet=next(w for w in wallets if w['key']==key['Public key'])
        balances=f.table('balances',code='eosio.key',scope=str(wallet['id']))
        holdings=[{'contract':assets[int(b['asset_id'])]['token_contract'],
                   'symbol':assets[int(b['asset_id'])]['token_symbol'],'atoms':int(b['amount'])}for b in balances]
        assert {(h['symbol'],h['atoms'])for h in holdings}=={('4,AIC',100000),('4,USD',200000)}
        name=key['native_account']
        native_balances=None
        if name:
            account=f.rpc('get_account',{'account_name':name})
            for role in ['owner','active']:
                authority=next(p for p in account['permissions']if p['perm_name']==role)['required_auth']
                assert authority==permission(key['Public key'])
            native_balances={'AIC':native(name),'USD':native(name,'keyusdtoken1','USD')}
            assert native_balances=={'AIC':'15.0000 AIC','USD':'5.0000 USD'}
        query=urllib.parse.urlencode({'public_key':key['Public key']})
        while True:
            with urllib.request.urlopen('http://127.0.0.1:7000/v2/state/get_key_accounts?'+query,timeout=20)as response:associated=json.load(response)
            accounts=associated.get('account_names',associated.get('accounts',[]))
            if (name is None and not accounts)or(name is not None and accounts==[name]):break
            assert time.monotonic()<deadline,'History public-key account index did not catch up'
            time.sleep(2)
        results.append({'index':key['index'],'public_key':key['Public key'],'native_account':name,
            'wallet_id':wallet['id'],'key_balances':holdings,'native_balances':native_balances,
            'indexed_accounts':accounts,'browser_URL':'https://aic-test.matrixai.one/#/key/'+key['Public key']})
    record.update(status='PASS',UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),results=results,
        binding_semantics='The same K1 public key is owner/active authority of its native account; custody and native balances are separate',
        key_private_material_remote_only=True,ordinary_90_unchanged=True,BP_votes_bonds_unchanged=True,
        new_servers=0,code_changes=0,TPS=False)
    write(progress,record);write(D/'RESULT.json',record)
    print(json.dumps({'status':'PASS','chain_id':CHAIN,'results':results}),flush=True)
except BaseException as error:
    record.update(status='NEEDS_RECONCILIATION',error=f.redact(str(error)))
    write(progress,record)
    raise RuntimeError(f.redact(str(error)))from None
