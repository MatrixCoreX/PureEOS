"""New private-chain qualification only. Never import into a public payer.

Native RAM payers and usage are measured on the actual production runtime;
all keys, accounts, funds and signed packets belong to this disposable chain.
"""
import datetime
import hashlib
import json
import os
from decimal import Decimal


def atoms(text):
    return int(Decimal(text.split()[0])*10000)


def deposit(t,key,quantity='0.0001 AIC',token='eosio.token',actor='keyusersolar',
            failure=None,legacy=False,mutate=None):
    auth=[{'actor':actor,'permission':'active'}]
    actions=[{'account':'eosio.key','name':'deposit','authorization':auth,
              'data':{'from':actor,'token_contract':token,'to':key['Public key'],
                      'quantity':quantity,'max_fee':'10.0000 AIC'if legacy else'0.0000 AIC',
                      'request_id':os.urandom(32).hex()}}]
    if legacy:
        actions.append({'account':'eosio.token','name':'transfer','authorization':auth,
                        'data':{'from':actor,'to':'eosio.key','quantity':'10.0000 AIC','memo':'atomic key fee'}})
    actions.append({'account':token,'name':'transfer','authorization':auth,
                    'data':{'from':actor,'to':'eosio.key','quantity':quantity,'memo':'atomic key principal'}})
    if mutate:mutate(actions)
    for action in actions[1:]:
        if action['name']=='transfer':
            action['data']=json.loads(t.run(['convert','pack_hex','--type','transfer','--abi-file',
                str(t.contracts/'eosio.token/eosio.token.abi'),json.dumps(action['data'])]))
    info=t.rpc('get_info');block=bytes.fromhex(info['last_irreversible_block_id'])
    transaction={'expiration':(datetime.datetime.fromisoformat(info['head_block_time']+'+00:00')+
                    datetime.timedelta(seconds=120)).strftime('%Y-%m-%dT%H:%M:%S'),
                 'ref_block_num':info['last_irreversible_block_num']&65535,
                 'ref_block_prefix':int.from_bytes(block[8:12],'little'),
                 'max_net_usage_words':0,'max_cpu_usage_ms':0,'delay_sec':0,
                 'context_free_actions':[],'actions':actions,'transaction_extensions':[]}
    return t.tx(['push','transaction',json.dumps(transaction)],failure=failure)


def ram(t,name):
    value=t.rpc('get_account',{'account_name':name})
    return {key:int(value[key])for key in ['ram_quota','ram_usage']}


def payer_rows(t,table,scope='eosio.key'):
    result=t.rpc('get_table_rows',{'json':True,'code':'eosio.key','scope':str(scope),
                                 'table':table,'show_payer':True,'limit':1000,'time_limit_ms':500})
    assert not result.get('more'),'No partial payer evidence'
    return result['rows']


def snapshot(t):
    return {'state':t.state(),'RAM':{name:ram(t,name)for name in ['eosio.key','keyusersolar','keyuserlunar']},
            'native':{name:{token:t.token_balance(name,token)for token in ['eosio.token','usd','cny']}
                      for name in ['eosio.key','keyusersolar','keyuserlunar']},
            'payers':payer_rows(t,'wallets'),'pending':t.ktable('pending')}


def upgrade_legacy_state(t,args):
    old=hashlib.sha256((args.legacy_artifacts/'eosio.key.wasm').read_bytes()).hexdigest()
    assert old=='7db295ea153410eed1a1810e6122c594d6a904117db232381cf713d1ab8d8416'
    holder=t.key();deposit(t,holder,'2.0000 AIC',legacy=True)
    before=snapshot(t)
    t.tx(['set','contract','eosio.key',str(args.artifacts),'eosio.key.wasm','eosio.key.abi','-p','eosio.key@owner'])
    after=snapshot(t)
    # Code installation itself uses contract RAM; user tables and original RAM
    # payers/quotas must remain unchanged. Never re-init or rewrite old rows.
    before['RAM'].pop('eosio.key');after['RAM'].pop('eosio.key')
    t.check(before==after,'NativeRAM migration preserves all funded7db rows, balances, counters, reserves and original payers')
    t.report['native_ram_legacy_upgrade']={'old_WASM':old,'preserved_snapshot_sha256':hashlib.sha256(
        json.dumps(before,sort_keys=True).encode()).hexdigest(),'scope':'Private pre-seal code replacement only; public deployment requires governance'}


def actions(t,result,args):
    found=[]
    def visit(value):
        if isinstance(value,dict):
            action=value.get('act')
            if isinstance(action,dict):
                action=dict(action)
                if isinstance(action.get('data'),str):
                    path=args.artifacts/'eosio.key.abi'if action['account']=='eosio.key'else t.contracts/'eosio.system/eosio.system.abi'if action['account']=='eosio'else t.contracts/'eosio.token/eosio.token.abi'
                    abi=json.loads(path.read_text());kind=next(x['type']for x in abi['actions']if x['name']==action['name'])
                    action['data']=json.loads(t.run(['convert','unpack_hex','--type',kind,'--abi-file',str(path),action['data']]))
                found.append(action)
            for key,item in value.items():
                if key!='act':visit(item)
        elif isinstance(value,list):
            for item in value:visit(item)
    visit(result['processed'].get('action_traces',[]));return found


def run_native_ram(t,source,args):
    def wallet(key):return next((w for w in t.ktable('wallets')if t.pub_equal(w['key'],key['Public key'])),None)
    def balance(key,asset):
        row=wallet(key)
        return next((int(x['amount'])for x in t.ktable('balances',str(row['id']))if int(x['asset_id'])==asset),0)if row else 0
    def native(name,token='eosio.token',symbol='AIC'):return atoms(t.token_balance(name,token).get(symbol,'0.0000 '+symbol))
    # Third-party global custody/identity is admitted separately. Named users
    # subsequently pay no AIC and sponsor only their recipient's actual rows.
    for token,symbol in [('usd','USD'),('cny','CNY')]:
        auth=[{'actor':'keyusersolar','permission':'active'}]
        t.tx(['push','transaction',json.dumps({'actions':[
            {'account':'eosio','name':'buyram','authorization':auth,'data':{'payer':'keyusersolar','receiver':'eosio.key','quant':'0.1000 AIC'}},
            {'account':'eosio.key','name':'regtoken','authorization':auth,'data':{'payer':'keyusersolar','token_contract':token,'token_symbol':'4,'+symbol,'min_bytes':4096}}]})])
    t.owner=t.key();t.receiver=t.key()
    evidence=[]
    t.report['native_sender_ram']={'transfers':evidence,'scope':'Actual native payer/fee/RAM measurements on isolated chain only; partial results retained if a later regression fails.'}
    def checked(label,key,quantity,token='eosio.token',actor='keyusersolar',new=False):
        symbol=quantity.split()[1];asset=next(int(x['id'])for x in t.ktable('assets')if x['token_contract']==token)
        before_ram=ram(t,'eosio.key');before_sender=ram(t,actor);principal=native(actor,token,symbol);aic=native(actor);ledger=balance(key,asset)
        saved={name:t.ktable(name)for name in ['feebuckets','pricing','assets']}
        result=deposit(t,key,quantity,token,actor);traces=actions(t,result,args)
        fee=[x['data']for x in traces if x['account']=='eosio.key'and x['name']=='feereceipt'];assert len(fee)==1
        t.check(all(atoms(fee[0][n])==0 for n in ['total','ram_payment','retained_base','relay_fee'])and not int(fee[0]['purchased_bytes']),label+': zerofee, no execution/minimum/relay/RAM purchase charge')
        t.check(not any(x['account']=='eosio'and x['name']in ['buyram','buyrambytes','logbuyram']for x in traces),label+': no automatic RAM buy')
        t.check(ram(t,'eosio.key')==before_ram,label+': contract RAM quota and usage unchanged')
        t.check(native(actor,token,symbol)==principal-atoms(quantity)and balance(key,asset)==ledger+atoms(quantity),label+': exact principal debit/credit')
        t.check(native(actor)==aic-(atoms(quantity)if token=='eosio.token'else 0),label+': sender AIC pays principal only or remains unchanged for other tokens')
        t.check(t.ktable('feebuckets')==saved['feebuckets']and t.ktable('pricing')==saved['pricing'],label+': fee buckets/pricing untouched')
        for old,current in zip(saved['assets'],t.ktable('assets')):
            t.check(all(current[k]==old[k]for k in ['ram_purchased','ram_used','ram_sponsor','funding_tx']),label+': per-token purchased RAM budget untouched')
        wid=wallet(key)['id'];payer=next(x['payer']for x in payer_rows(t,'balances',wid)if int(x['data']['asset_id'])==asset)
        if new:t.check(payer==actor and ram(t,actor)['ram_usage']>before_sender['ram_usage'],label+': actual native row RAM billed to authorized sender')
        else:t.check(ram(t,actor)==before_sender,label+': existing records add no persistent sender RAM')
        t.check(not t.ktable('pending'),label+': pending scratch erased')
        t.check_reserves();evidence.append({'label':label,'transaction_id':result['transaction_id'],'fee':fee[0],'row_payer':payer,'contract_RAM_before':before_ram,'contract_RAM_after':ram(t,'eosio.key')})
        return result
    for token,symbol,quantity in [('eosio.token','AIC','100.0000 AIC'),('usd','USD','100.0000 USD'),('cny','CNY','100.0000 CNY')]:
        checked('native first '+symbol,t.owner,quantity,token,new=True)
        checked('native repeat '+symbol,t.owner,'0.0100 '+symbol,token)
    # A different donor updates existing records without payer migration.
    before=payer_rows(t,'balances',wallet(t.owner)['id']);checked('different donor keeps original payer',t.owner,'0.0100 AIC',actor='keyuserlunar')
    t.check([(int(x['data']['asset_id']),x['payer'])for x in payer_rows(t,'balances',wallet(t.owner)['id'])]==[(int(x['data']['asset_id']),x['payer'])for x in before], 'Existing balance payer remains original donor when another account transfers')
    # A native USD sender can have zero AIC; no hidden funding/prepayment path.
    t.transfer('keyusersolar','keyuserlunar','1.0000 USD','private test USD',token='usd')
    amount=t.token_balance('keyuserlunar')['AIC'];t.transfer('keyuserlunar','keyusersolar',amount,'private test zero AIC')
    checked('zero AIC native USD sender',t.key(),'0.1000 USD','usd',actor='keyuserlunar',new=True)
    def rejected(label,call):
        before=snapshot(t);call();t.check(snapshot(t)==before,label+': all principal/ledger/payers/RAM/fees roll back')
    rejected('old paid deposit refused',lambda:deposit(t,t.key(),legacy=True,failure='max_fee must be zero'))
    rejected('wrong principal bytes',lambda:deposit(t,t.key(),mutate=lambda a:a[1]['data'].update(quantity='0.0002 AIC'),failure='principal bytes mismatch'))
    rejected('missing principal root',lambda:deposit(t,t.key(),mutate=lambda a:a.pop(),failure='two-action deposit'))
    rejected('wrong sender authorization',lambda:deposit(t,t.key(),mutate=lambda a:a[1].update(authorization=[{'actor':'keyuserlunar','permission':'active'}]),failure='same explicit sender'))
    rejected('wrong token issuer',lambda:deposit(t,t.key(),'0.0001 AIC','keybadtokenx',failure='whitelist'))
    # Reduce only this private user's own RAM using the real sellram action.
    available=ram(t,'keyuserlunar');free=available['ram_quota']-available['ram_usage'];assert free>1024
    t.action('sellram',{'account':'keyuserlunar','bytes':free-32},'keyuserlunar')
    available=ram(t,'keyuserlunar');t.check(available['ram_quota']-available['ram_usage']<128,'Private sender has genuinely insufficient RAM, without privileged limit changes')
    rejected('insufficient sender RAM',lambda:deposit(t,t.key(),'0.0100 AIC',actor='keyuserlunar',failure='insufficient ram'))
    checked('existing rows with nearly exhausted sender RAM',t.owner,'0.0100 AIC',actor='keyuserlunar')
    t.action('buyram',{'payer':'keyuserlunar','receiver':'keyuserlunar','quant':'1.0000 AIC'},'keyuserlunar')
    checked('explicit own RAM topup then deposit',t.key(),'0.0100 AIC',actor='keyuserlunar',new=True)
    # The governance-disabled eosio.wrap account cannot receive notifications.
    # Use an ordinary private account, deliberately close its empty AIC row.
    quantity=t.token_balance('keyramnotify')['AIC']
    t.transfer('keyramnotify','keyusersolar',quantity,'private withdrawal fixture')
    t.action('close',{'owner':'keyramnotify','symbol':'4,AIC'},'keyramnotify',contract='eosio.token')
    # Public-key routes retain their native RAM purchases and paid relay model.
    for operation,recipient,expected_bytes in [('ksend',t.receiver['Public key'],1536),('ksend',t.receiver['Public key'],0),('withdraw','keyramnotify',512),('withdraw','keyramnotify',0)]:
        result=t.execute(operation,t.owner,t.request(t.owner,operation,to=recipient,quantity='0.0100 AIC',memo=''))
        traces=actions(t,result,args);fee=next(x['data']for x in traces if x['account']=='eosio.key'and x['name']=='feereceipt')
        t.check(atoms(fee['total'])>0 and atoms(fee['relay_fee'])>0 and int(fee['allocated_bytes'])==expected_bytes,'Public '+operation+': execution/relay charges and RAM admission unchanged')
        t.check(atoms(fee['total'])==atoms(fee['ram_payment'])+atoms(fee['retained_base'])+atoms(fee['relay_fee']),'Public '+operation+': complete actual fee conservation')
        t.check_reserves()
    for method in ['test_nonce_race','test_proof_attacks','test_hot_key_rotation','test_overspend']:
        getattr(t,method)()
    t.report['native_sender_ram']={'transfers':evidence,'scope':'Actual native ledger/payer/RAM measurements and positive/negative signed transactions on private loopback chain only; no public payments or phone private data.'}
