"""Real native purchases and reserve conservation; remote isolated chain only."""
import json,os,subprocess,datetime
from decimal import Decimal

def atoms(text):return int(Decimal(text.split()[0])*10000)
def run_atomic(t,source,args):
    def amount(n,symbol='AIC'):return f'{n//10000}.{n%10000:04d} {symbol}'
    def native(owner,token='eosio.token',symbol='AIC'):
        return atoms(t.token_balance(owner,token).get(symbol,'0.0000 '+symbol))
    def wallet(key):
        return next((w for w in t.ktable('wallets')if t.pub_equal(w['key'],key['Public key'])),None)
    def balance(key,asset=1):
        w=wallet(key)
        return 0 if w is None else next((int(b['amount'])for b in t.ktable('balances',str(w['id']))if int(b['asset_id'])==asset),0)
    def trace_acts(result):
        found=[]
        def visit(v):
            if isinstance(v,dict):
                if isinstance(v.get('act'),dict)and v.get('receiver')==v['act'].get('account'):
                    action=v['act']
                    # Bounded native ABI serialization can leave individual
                    # trace payloads as hex. Decode that exact payload against
                    # the chain ABI instead of assuming every trace is JSON.
                    if action['name']in ['receipt','feereceipt','logbuyram']and isinstance(action['data'],str):
                        abi_path=args.artifacts/'eosio.key.abi'if action['account']=='eosio.key'else t.contracts/'eosio.system/eosio.system.abi'
                        abi=json.loads(abi_path.read_text());kind=next(x['type']for x in abi['actions']if x['name']==action['name'])
                        decoded=json.loads(t.run(['convert','unpack_hex','--type',kind,'--abi-file',str(abi_path),action['data']]))
                        action={**action,'data':decoded}
                        t.report.setdefault('binary_trace_decodes',[]).append({'account':action['account'],'action':action['name'],'type':kind,'decoder':'offline reviewed ABI'})
                    found.append(action)
                for k,x in v.items():
                    if k!='act':visit(x)
            elif isinstance(v,list):
                for x in v:visit(x)
        visit(result['processed'].get('action_traces',[]));return found
    def checked(label,call,expected_bytes):
        a=t.rpc('get_account',{'account_name':'eosio.key'});before=t.state()
        result=call();acts=trace_acts(result)
        receipts=[x['data']for x in acts if x['account']=='eosio.key'and x['name']=='feereceipt']
        t.check(len(receipts)==1,label+': exactly one canonical fee breakdown')
        f=receipts[0];buy=[x['data']for x in acts if x['account']=='eosio'and x['name']=='logbuyram']
        cost=atoms(f['ram_payment']);quota=int(t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])-int(a['ram_quota'])
        t.check(int(f['allocated_bytes'])==expected_bytes,label+': expected conservative allocation budget')
        t.check(atoms(f['total'])==cost+atoms(f['retained_base'])+atoms(f['relay_fee']),label+': no duplicated RAM fee; exact fee equation')
        transfer_receipts=[x['data']for x in acts if x['account']=='eosio.key'and x['name']=='receipt']
        assert len(transfer_receipts)==1
        relay=transfer_receipts[0]['relayer'];owner=relay or 'eosio.key'
        prior={r['owner']:r for r in before['feebuckets']}
        after={r['owner']:r for r in t.ktable('feebuckets')}
        old=prior.get(owner,{'network':0,'relay_earned':0});now=after[owner]
        execution=atoms(f['retained_base']);service=atoms(f['relay_fee'])
        t.check(int(now['network'])-int(old['network'])==(0 if relay else execution) and
            int(now['relay_earned'])-int(old['relay_earned'])==(execution+service if relay else 0),
            label+': actual relay earns execution plus relay fee; no-relay destination and old reserves preserved')
        t.check(all(after[k]==v for k,v in prior.items()if k!=owner),label+': unrelated fee buckets unchanged')
        if expected_bytes:
            t.check(len(buy)==1 and buy[0]['payer']=='eosio.key'and buy[0]['receiver']=='eosio.key'and
                atoms(buy[0]['quantity'])==cost and int(buy[0]['bytes'])==quota==int(f['purchased_bytes'])and quota>=expected_bytes,
                label+': native purchase, exact gross AIC cost and actual acquired quota verified')
        else:t.check(not buy and not cost and quota==0,label+': existing records buy no RAM')
        t.check(not t.ktable('pending'),label+': internal payment ticket consumed')
        t.check_reserves()
        t.report.setdefault('atomic_transfers',[]).append({'label':label,'transaction_id':result['transaction_id'],
             'fee':f,'native_RAM_buy':buy,'quota_delta':quota})
        return f
    def deposit(key,quantity,token='eosio.token',cap='10.0000 AIC',failure=None,actor='keyusersolar'):
        auth=[{'actor':actor,'permission':'active'}]
        actions=[{'account':'eosio.key','name':'deposit','authorization':auth,'data':{'from':actor,'token_contract':token,'to':key['Public key'],'quantity':quantity,'max_fee':cap,'request_id':os.urandom(32).hex()}},
            {'account':'eosio.token','name':'transfer','authorization':auth,'data':{'from':actor,'to':'eosio.key','quantity':cap,'memo':'atomic key fee'}},
            {'account':token,'name':'transfer','authorization':auth,'data':{'from':actor,'to':'eosio.key','quantity':quantity,'memo':'atomic key principal'}}]
        for a in actions[1:]:
            packed=t.run(['convert','pack_hex','--type','transfer','--abi-file',str(t.contracts/'eosio.token/eosio.token.abi'),json.dumps(a['data'])])
            a['data']=json.loads(packed)
            assert isinstance(a['data'],str) and len(a['data'])%2==0 and all(c in '0123456789abcdefABCDEF' for c in a['data']), 'Invalid canonical transfer encoding'
        info=t.rpc('get_info');block=bytes.fromhex(info['last_irreversible_block_id'])
        packet={'expiration':(datetime.datetime.fromisoformat(info['head_block_time']+'+00:00')+datetime.timedelta(seconds=120)).strftime('%Y-%m-%dT%H:%M:%S'),
            'ref_block_num':info['last_irreversible_block_num']&65535,'ref_block_prefix':int.from_bytes(block[8:12],'little'),
            'max_net_usage_words':0,'max_cpu_usage_ms':0,'delay_sec':0,'context_free_actions':[],'actions':actions,'transaction_extensions':[]}
        return t.tx(['push','transaction',json.dumps(packet)],failure=failure)
    def reject(label,call):
        before=t.state();market=t.table('rammarket');ram=t.rpc('get_account',{'account_name':'eosio.key'});n={x:(native(x),native(x,'usd','USD'))for x in ['eosio.key','keyusersolar','keyuserlunar']}
        call()
        t.check(before==t.state()and not t.ktable('pending')and all(n[x]==(native(x),native(x,'usd','USD'))for x in n)
            and ram['ram_quota']==t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']
            and ram['ram_usage']==t.rpc('get_account',{'account_name':'eosio.key'})['ram_usage']and market==t.table('rammarket'),label+': full principal/fee/RAM/budget/count rollback')
    t.owner=t.key();t.receiver=t.key();third=t.key()
    t.check(not t.ktable('nativefees')and all(not int(a['ram_purchased'])for a in t.ktable('assets')),
        'No manual fee prepayment or sponsored token capacity exists at test start')
    t.report['bootstrap_RAM']= {k:t.rpc('get_account',{'account_name':'eosio.key'})[k]for k in ['ram_quota','ram_usage']}
    # Start with USD and a recipient without AIC, proving fee-token custody does
    # not depend on prior AIC public-key admission or a manual token regtoken.
    donor=native('keyusersolar');f=checked('named-to-key first USD',lambda:deposit(t.owner,'100.0000 USD','usd'),3584)
    t.check(balance(t.owner,2)==1000000 and balance(t.owner,1)==0 and donor-native('keyusersolar')==atoms(f['total']),
        'USD recipient receives full principal without AIC; native sender pays exact additional AIC fee')
    reject('USD key sender without AIC',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=t.receiver['Public key'],token_contract='usd',quantity='1.0000 USD',memo=''),failure='insufficient additional AIC'))
    donor=native('keyusersolar');f=checked('named-to-key first AIC balance',lambda:deposit(t.owner,'20.0000 AIC'),512)
    t.check(balance(t.owner,1)==200000 and donor-native('keyusersolar')==200000+atoms(f['total']),
        'Direct AIC deposit pays exact fee once, with full principal and no prepaid credit')
    checked('named-to-key existing AIC balance',lambda:deposit(t.owner,'1.0000 AIC'),0)
    checked('key-to-key new recipient',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=t.receiver['Public key'],quantity='3.0000 AIC',memo='')),1536)
    t.check(balance(t.receiver)==30000,'Public-key recipient gets full 3 AIC')
    checked('key-to-key existing recipient',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=t.receiver['Public key'],quantity='1.0000 AIC',memo='')),0)
    checked('key-to-key first USD balance',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=t.receiver['Public key'],token_contract='usd',quantity='2.0000 USD',memo='')),512)
    checked('key-to-key new USD-only recipient',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=third['Public key'],token_contract='usd',quantity='2.0000 USD',memo='')),1536)
    t.check(balance(third,1)==0 and balance(third,2)==20000,'New USD-only receiving key needs no AIC or native account')
    reject('New recipient exceeds signed cap',lambda:t.execute('ksend',t.owner,t.request(t.owner,to=t.key()['Public key'],max_fee='0.0000 AIC',memo=''),failure='fee exceeds signed maximum'))
    reject('Named deposit exceeds fee cap',lambda:deposit(t.key(),'1.0000 AIC',cap='0.0000 AIC',failure='funding'))
    reject('Wrong issuer',lambda:deposit(t.key(),'1.0000 AIC','keybadtokenx',failure='whitelist'))
    cny_only=t.key()
    checked('named-to-key first CNY',lambda:deposit(cny_only,'100.0000 CNY','cny'),3584)
    t.check(balance(cny_only,3)==1000000 and balance(cny_only,1)==0,'CNY recipient receives full principal without AIC')
    reject('CNY key sender without AIC',lambda:t.execute('ksend',cny_only,t.request(cny_only,to=t.owner['Public key'],token_contract='cny',quantity='1.0000 CNY',memo=''),failure='insufficient additional AIC'))
    checked('named-to-key CNY holder receives AIC fee funds',lambda:deposit(cny_only,'2.0000 AIC'),512)
    checked('key-to-key first CNY balance',lambda:t.execute('ksend',cny_only,t.request(cny_only,to=t.owner['Public key'],token_contract='cny',quantity='2.0000 CNY',memo='')),512)
    checked('key-to-key existing CNY balance',lambda:t.execute('ksend',cny_only,t.request(cny_only,to=t.owner['Public key'],token_contract='cny',quantity='1.0000 CNY',memo='')),0)
    checked('key-to-named first CNY balance',lambda:t.execute('withdraw',cny_only,t.request(cny_only,'withdraw',to='keyuserlunar',token_contract='cny',quantity='1.0000 CNY',memo='')),512)
    t.check(native('keyuserlunar','cny','CNY')==10000,'Named CNY recipient receives full principal automatically')
    checked('key-to-named existing CNY balance',lambda:t.execute('withdraw',cny_only,t.request(cny_only,'withdraw',to='keyuserlunar',token_contract='cny',quantity='1.0000 CNY',memo='')),0)

    reject('Standalone legacy fee funding',lambda:t.transfer('keyusersolar','eosio.key','0.1000 AIC','fee',failure='use atomic deposit'))
    reject('Standalone legacy key memo',lambda:t.transfer('keyusersolar','eosio.key','0.1000 AIC',t.memo(t.owner),failure='use atomic deposit'))
    # Both a normal USD recipient and a core-token recipient are opened in the
    # same transaction. No owner action or manual external token::open is used.
    t.check('USD'not in t.token_balance('keyuserlunar','usd'),'Named USD recipient initially has no balance row')
    checked('key-to-named first USD balance',lambda:t.execute('withdraw',t.owner,t.request(t.owner,'withdraw',to='keyuserlunar',token_contract='usd',quantity='1.0000 USD',memo='')),512)
    t.check(native('keyuserlunar','usd','USD')==10000,'Named USD recipient receives entire principal automatically')
    checked('key-to-named existing USD balance',lambda:t.execute('withdraw',t.owner,t.request(t.owner,'withdraw',to='keyuserlunar',token_contract='usd',quantity='1.0000 USD',memo='')),0)
    native_to=next(n for n in ['eosio.saving','eosio.wrap','eosio.msig']if 'AIC'not in t.token_balance(n))
    checked('key-to-named first AIC balance',lambda:t.execute('withdraw',t.owner,t.request(t.owner,'withdraw',to=native_to,quantity='0.1000 AIC',memo='')),512)
    checked('key-to-named existing AIC balance',lambda:t.execute('withdraw',t.owner,t.request(t.owner,'withdraw',quantity='0.1000 AIC',memo='')),0)
    checked('self withdrawal without relay fee',lambda:t.execute('selfout',t.owner,t.request(t.owner,'selfout',quantity='0.1000 AIC',memo='')),0)
    t.check(not t.ktable('nativefees'),'All ordinary transfers complete without manual nativefee credit')
    for op in ['payram','settleram']:
        reject('External '+op,lambda op=op:t.action(op,{'ticket':os.urandom(32).hex()},'keyusersolar',contract='eosio.key',failure=''))
    # Tests below preserve exact proof/domain/TAPOS/expiry/retry protection and
    # same-epoch independent-device transfers after internal RAM continuations.
    # Independent holders/devices can sign two requests for the same unopened
    # recipient before either is submitted. Native execution opens it once.
    fresh=t.key();requests=[t.request(t.owner,to=fresh['Public key'],quantity='0.0100 AIC',memo='')for _ in range(2)]
    packets=[t.signed('ksend',r,t.owner)for r in requests]
    quota=t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']
    lib=t.rpc('get_info')['last_irreversible_block_num'];children=[]
    try:
        for packet in packets:
            children.append(subprocess.Popen([str(t.cleos),'--no-auto-keosd','--url',t.url,'--wallet-url','unix://'+str(t.root/'wallet/keosd.sock'),
                'push','transaction',json.dumps(packet['signed']),'-s','-j'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
        outputs=[c.communicate(timeout=60)for c in children]
        results=[]
        # An overloaded functional host may definitively reject one submitted
        # packet on CPU. Reconcile its exact native ID before retrying the SAME
        # signed bytes. Never require the peer's legitimately executed transfer
        # to be absent, and never re-sign an ambiguous payment.
        for i,(child,output) in enumerate(zip(children,outputs)):
            try:result=json.loads(output[0])
            except Exception:raise RuntimeError('Ambiguous concurrent response; retain originals, no retry')
            expected=packets[i]['prepared']['transaction_id'];assert result['transaction_id']==expected
            if child.returncode:
                trace=result['processed'];assert trace.get('receipt')is None and trace.get('except',{}).get('name')=='tx_cpu_usage_exceeded',t.redact(str(output))
                end=t.wait_lib(trace['block_num']+2)['last_irreversible_block_num'];scanned=[];found=[]
                for n in range(lib+1,end+1):
                    block=t.rpc('get_block',{'block_num_or_id':n});scanned.append({'block':n,'id':block['id']})
                    found.extend(x for x in block['transactions']if(x['trx']if isinstance(x['trx'],str)else x['trx']['id'])==expected)
                assert not found,'CPU-refused original unexpectedly present: stop and reconcile, no retry'
                nonce=t.nonce+1
                result=t.execute('ksend',t.owner,requests[i],packets[i]);assert result['transaction_id']==expected
                frozen=json.loads(t.run(['convert','pack_transaction',str(t.root/'immutable-packets'/(str(nonce)+'.json'))]))
                original=packets[i]['packet'];assert frozen['packed_trx']==original['packed_trx']and frozen['packed_context_free_data']==original['packed_context_free_data']and frozen['signatures']==original['signatures']
                t.report.setdefault('atomic_race_CPU_refusals',[]).append({'transaction_id':expected,'canonical_absence':scanned,'same_signed_bytes_after_definitive_CPU_refusal':True,'no_re_sign':True,'unchanged_CPU_cap_us':150000})
            else:assert result['processed']['receipt']['status']=='executed'
            results.append(result)
        confirmed=[t.wait_transaction(r['transaction_id'],lib,timeout=90)for r in results]
        fees=[a['data']for r in results for a in trace_acts(r)if a['account']=='eosio.key'and a['name']=='feereceipt']
        t.check(balance(fresh)==200 and len(fees)==2 and sorted(int(f['allocated_bytes'])for f in fees)==[0,1536],
            'Independently pre-signed requests submitted together create one shared recipient record and deliver both full principals')
        t.check(t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']-quota==sum(int(f['purchased_bytes'])for f in fees),
            'Concurrent new-recipient requests purchase RAM only once; the later transaction charges no RAM')
        t.report['atomic_recipient_race']={'fees':fees,'confirmations':confirmed}
    finally:
        for c in children:
            if c.poll()is None:c.kill();c.wait()
    t.test_nonce_race();t.test_proof_attacks();t.test_hot_key_rotation();t.test_overspend()
    for rate in [1000,1500,2000,2500,3000]:
        t.action('setrate',{'bp':'keyrelaymoon','fee_bps':rate},'keyrelaymoon',contract='eosio.key')
        f=checked('relay rate '+str(rate),lambda rate=rate:t.execute('ksend',t.owner,t.request(t.owner,quantity='0.0001 AIC',memo='',relay_fee_bps=rate)),0)
        base=atoms(f['total'])-atoms(f['relay_fee'])
        t.check(atoms(f['relay_fee'])==max(1,(base*rate+9999)//10000),'Exact configured relay fee with upward rounding '+str(rate))
    for op in ['cancel','selfcancel']:
        before={r['owner']:r for r in t.ktable('feebuckets')}
        result=t.execute(op,t.owner,t.request(t.owner,op))
        receipt=next(a['data']for a in trace_acts(result)if a['account']=='eosio.key'and a['name']=='receipt')
        fee=atoms(receipt['fee']);owner='keyrelaymoon'if op=='cancel'else'eosio.key'
        now=next(r for r in t.ktable('feebuckets')if r['owner']==owner);old=before[owner]
        t.check(fee>0 and int(now['relay_earned'])-int(old['relay_earned'])==(fee if op=='cancel'else 0)and
            int(now['network'])-int(old['network'])==(0 if op=='cancel'else fee),
            op+': cancellation uses the same execution-income destination and preserves historical network reserves')
    earned=next(f for f in t.ktable('feebuckets')if f['owner']=='keyrelaymoon');before=native('keyrelaymoon')
    t.action('claim',{'bp':'keyrelaymoon','to':'keyrelaymoon','quantity':'0.0010 AIC','max_fee':'0.0000 AIC'},'keyrelaymoon',contract='eosio.key')
    after=next(f for f in t.ktable('feebuckets')if f['owner']=='keyrelaymoon')
    t.check(native('keyrelaymoon')==before+10 and int(after['relay_earned'])==int(earned['relay_earned'])-10 and after['network']==earned['network'],
        'Claim still pays full earned quantity with zero application fee and no prepaid credit')
    before=t.state()
    t.action('claim',{'bp':'keyrelaymoon','to':'keyrelaymoon','quantity':amount(int(after['relay_earned'])+1),'max_fee':'0.0000 AIC'},
        'keyrelaymoon',contract='eosio.key',failure='insufficient confirmed relay earnings')
    t.check(t.state()==before,'Claim cannot redeem more than newly earned execution plus relay fees')
    remaining=int(after['relay_earned']);before_native=native('keyrelaymoon')
    t.action('claim',{'bp':'keyrelaymoon','to':'keyrelaymoon','quantity':amount(remaining),'max_fee':'0.0000 AIC'},'keyrelaymoon',contract='eosio.key')
    now=next(r for r in t.ktable('feebuckets')if r['owner']=='keyrelaymoon')
    t.check(int(now['relay_earned'])==0 and now['network']==after['network']and native('keyrelaymoon')==before_native+remaining,
        'Relay can claim all execution plus relay income in full without spending historical network reserve')
    if args.signup_artifacts:
        from signup_bridge_cases import run_signup_bridge
        run_signup_bridge(t,source,args,deposit,balance,native,trace_acts)
    # Capacity rejection occurs in settlement AFTER the attempted native buy.
    # The whole transaction must restore the real curve, quotas and all funds.
    while int(t.ktable('config')[0]['wallet_count'])<20:deposit(t.key(),'0.0001 AIC')
    reject('Capacity after attempted native RAM buy',lambda:deposit(t.key(),'0.0001 AIC',failure='wallet storage capacity reached'))
    checked('Existing recipient remains usable at key capacity',lambda:t.execute('ksend',t.owner,t.request(t.owner,quantity='0.0001 AIC',memo='')),0)

    t.check_reserves()
    t.report['scope']='Remote isolated real native TEST chain, production candidate WASM; atomic AIC/USD/CNY 3-route functional/security qualification, not TPS or live deployment'
