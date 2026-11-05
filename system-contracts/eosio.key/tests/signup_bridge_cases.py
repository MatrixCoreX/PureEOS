"""Atomic, strict-depth-4 real-fee/REX signup; isolated qualification only."""
import json,os
from decimal import Decimal

def run_signup_bridge(t,source,args,deposit,balance,native,trace_acts):
    baseline={'ram_quota':t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']}
    def pending():return t.table('pendingkey',code='eosio.signup',scope='eosio.signup')
    def snapshot():
        return {'key':t.state(),'market':t.table('rammarket'),'signup':native('eosio.signup'),
                'signup_RAM':t.rpc('get_account',{'account_name':'eosio.signup'})['ram_usage'],
                'pending':pending(),'key_RAM':t.rpc('get_account',{'account_name':'eosio.key'})['ram_quota']}
    def rejected(label,call):
        before=snapshot();call();t.check(snapshot()==before,label+': complete KEY/fees/resources/pending rollback')
    def req(name='mobilekeyone',op='withdraw',**kw):
        return t.request(t.owner,op,to='eosio.signup',quantity='0.2500 AIC',memo=name+'-'+t.owner['Public key'],**kw)
    t.check(json.loads((t.root/'genesis.json').read_text())['initial_configuration']['max_inline_action_depth']==4,'Signup: actual native max inline depth remains four')
    t.check(t.table('rexpool')and int(t.table('rexpool')[0]['total_rex'].split()[0].replace('.',''))>0,
            'Signup: real funded REX and fee-forwarding contract remain active')
    t.action('createkey',{'request_id':os.urandom(32).hex()},'keyusersolar',contract='eosio.signup',failure='exact two-root atomic registration required')
    t.check(not pending(),'Signup: standalone permissionless continuation cannot create or spend')
    for attack,error in [('bridge-missing','exact owner operation root layout'),('bridge-nonce','continuation mismatch'),
                         ('bridge-auth','continuation mismatch'),('bridge-extra','exact owner operation root layout'),
                         ('bridge-account','continuation mismatch'),('bridge-reorder','')]:
        request=req(op='selfout');signed=t.signed('selfout',request,t.owner,attack=attack)
        rejected('Signup signed malformed '+attack,lambda signed=signed,error=error:t.execute('selfout',t.owner,request,signed,failure=error))
    # Force a late failure in root2: the first KEY debit, fee allocation, transfer
    # and transient pending row all execute first, then are rolled back together.
    for quantity,error in [('0.2001 AIC','minimum RAM allocation'),('0.2000 AIC','sufficient for CPU')]:
        request=t.request(t.owner,'withdraw',to='eosio.signup',quantity=quantity,memo='mobilekeyone-'+t.owner['Public key'])
        rejected('Signup insufficient budget '+quantity,lambda request=request,error=error:t.execute('withdraw',t.owner,request,failure=error))
    r=req();r['max_fee']='0.0000 AIC'
    rejected('Signup signed maximum fee',lambda:t.execute('withdraw',t.owner,r,failure='fee exceeds signed maximum'))
    for name,op in [('mobilekeyone','withdraw'),('mobilekeytwo','selfout')]:
        before=balance(t.owner);held=native('eosio.signup');ram=t.rpc('get_account',{'account_name':'eosio.signup'})['ram_usage']
        request=req(name,op);result=t.execute(op,t.owner,request);acts=trace_acts(result)
        fees=[x['data']for x in acts if x['account']=='eosio.key'and x['name']=='feereceipt'];assert len(fees)==1
        fee=fees[0];cost=int(Decimal(fee['total'].split()[0])*10000)
        account=t.rpc('get_account',{'account_name':name});permissions={p['perm_name']:p['required_auth']for p in account['permissions']}
        t.check(all(a['threshold']==1 and len(a['keys'])==1 and t.pub_equal(a['keys'][0]['key'],t.owner['Public key'])and
                    not a['accounts']and not a['waits']for a in permissions.values()),'Signup '+op+': exact owner/active public key')
        t.check(int(account['ram_quota'])>=4096 and account['self_delegated_bandwidth']['net_weight']=='0.1000 AIC'and
                account['self_delegated_bandwidth']['cpu_weight']=='0.1000 AIC','Signup '+op+': RAM >=4KiB and both stakes owned by new account')
        t.check(before-balance(t.owner)==2500+cost and native('eosio.signup')==held,
                'Signup '+op+': exact registration principal plus bounded KEY fee, no retained signup funds')
        ram_after=t.rpc('get_account',{'account_name':'eosio.signup'})['ram_usage'];deltas=[]
        def collect_ram(value):
            if isinstance(value,dict):
                if 'act'in value and 'receiver'in value:
                    for delta in value.get('account_ram_deltas',[]):
                        if delta['account']=='eosio.signup':deltas.append({'receiver':value['receiver'],'account':value['act']['account'],'name':value['act']['name'],'delta':int(delta['delta'])})
                for key,child in value.items():
                    if key!='act':collect_ram(child)
            elif isinstance(value,list):
                for child in value:collect_ram(child)
        collect_ram(result['processed'].get('action_traces',[]))
        bridge_ram=sum(d['delta']for d in deltas if d['receiver']=='eosio.signup')
        token_ram=sum(d['delta']for d in deltas if d['receiver']=='eosio.token')
        row=t.rpc('get_table_rows',{'json':True,'code':'eosio.token','scope':'eosio.signup','table':'accounts','limit':2,'show_payer':True})['rows']
        observation={'before':ram,'after':ram_after,'pending_empty':not pending(),'action_deltas':deltas,'AIC_rows_with_payer':row}
        t.report.setdefault('signup_RAM_observations',[]).append(observation)
        # Standard token::sub_balance adopts its own pre-opened row on the
        # first spend. That legitimate one-time token RAM is distinct from
        # the bridge's same-transaction pending storage, which must net zero.
        t.check(not pending()and bridge_ram==0 and sum(d['delta']for d in deltas)==ram_after-ram and
                token_ram==ram_after-ram and all(r['payer']=='eosio.signup'for r in row),
                'Signup '+op+': pending RAM nets zero; only independently traced standard-token row ownership may persist')
        if op=='selfout':t.check(ram_after==ram,'Signup repeat: no additional persistent RAM per registration')
        t.check(len([a for a in acts if a['account']=='eosio.signup'and a['name']=='createkey'])==1 and
                len([a for a in acts if a['account']=='eosio'and a['name']=='donatetorex'])>=1,
                'Signup '+op+': actual second root and real fee→REX path execute within depth four')
        t.check_reserves()
        t.report.setdefault('signup_bridge',[]).append({'name':name,'operation':op,'transaction_id':result['transaction_id'],'fee':fee,'RAM':account['ram_quota']})
    rejected('Signup existing name',lambda:t.execute('withdraw',t.owner,req(),failure='already registered'))
    before=native('keyusersolar')
    t.transfer('keyusersolar','eosio.signup','0.2500 AIC','mobilelegacy-'+t.owner['Public key'])
    t.check(before-native('keyusersolar')==2500 and t.rpc('get_account',{'account_name':'mobilelegacy'})['ram_quota']>=4096 and not pending(),
            'Signup: legacy native-payer one-root registration remains functional')
    t.check(not t.ktable('nativefees'),'Signup never requires a native prepaid fee balance')
