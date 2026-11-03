#!/usr/bin/env python3
"""Functional qualification on a NEW PRIVATE loopback TEST chain. No TPS.

Uses the pinned SDK LocalTest utilities, not any live-chain campaign subclass.
All child processes stop in finally. No systemctl, peers or existing data dirs.
"""
import argparse, datetime, hashlib, json, os, shlex, subprocess, sys, time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package',type=Path,required=True)
    p.add_argument('--work',type=Path,required=True)
    p.add_argument('--artifacts',type=Path,required=True)
    p.add_argument('--native-cleos',type=Path)
    p.add_argument('--fixture',action='store_true',help='TEST ONLY alternate system table source; never production consensus evidence')
    args=p.parse_args()
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sdk-tests'))
    from pureeos_single_choice_local_test import LocalTest
    source=Path(__file__).resolve().parents[1]
    args.work.mkdir(mode=0o700,parents=True,exist_ok=False)
    os.chmod(args.work,0o700)
    for n in ['nodeos','cleos','keosd','helios-util']:
        d=args.work/'programs'/n;d.mkdir(parents=True)
        wrapper=d/n
        wrapper.write_text('#!/bin/sh\nexec '+shlex.quote(str(args.package/'bin'/n))+' "$@"\n')
        wrapper.chmod(0o700)
    class Test(LocalTest):
        local_signers=('eosio','gptkeyorigin')
        def __init__(self,*a,**kw):
            super().__init__(*a,**kw)
            self.bootstrapping=True
            self.report['chain_test_SHA256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
            self.report['node_runtime']=subprocess.check_output(['node','--version'],text=True).strip()
            if args.native_cleos:self.cleos=args.native_cleos
        def tx(self,args,*,failure=None):
            for attempt in range(1,6):
                try:return super().tx(args,failure=failure)
                except RuntimeError as error:
                    if failure is not None or not self.bootstrapping or'tx_cpu_usage_exceeded'not in str(error)or attempt==5:raise
                    self.report.setdefault('deployment_cpu_retries',[]).append({'attempt':attempt,'command':args[:3],'error':'atomic bootstrap transaction exceeded unchanged 150ms CPU limit'})
                    print('RETRY: atomic bootstrap CPU deadline; unchanged caps',flush=True)
                    self.wait_lib(self.rpc('get_info')['head_block_num']+2)
        def start_node(self,phase,extra=()):
            super().start_node(phase,extra)
        def setup(self):
            ordinary=['civicgovlead','civicgovauth','civicgovcode','keyusersolar','keyuserlunar',
                      'gptkeyorigin','keyrelaymoon','keytesttoken','keybadtokenx','keysysmockup']
            self.accounts={n:self.key() for n in ['eosio',*ordinary]}
            self.bls={n:self.key(True) for n in self.local_signers}
            self.root.joinpath('config').mkdir();self.root.joinpath('wallet').mkdir()
            profile=(args.package/'config/test-low-cost/config.ini').read_text().splitlines()
            profile=[x for x in profile if not x.startswith(('http-server-address','p2p-listen-endpoint'))]
            profile += ['plugin = eosio::producer_api_plugin','enable-stale-production = true',
                        'http-server-address = 127.0.0.1:'+str(self.http_port),
                        'p2p-listen-endpoint = 127.0.0.1:'+str(self.p2p_port),'verbose-http-errors = true']
            for n in self.local_signers:
                profile.append('producer-name = '+n)
                for k in [self.accounts[n],self.bls[n]]:
                    profile.append('signature-provider = '+k['Public key']+'=KEY:'+k['Private key'])
            (self.root/'config/config.ini').write_text('\n'.join(profile)+'\n')
            os.chmod(self.root/'config/config.ini',0o600)
            genesis={'initial_timestamp':datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000'),
                     'initial_key':self.accounts['eosio']['Public key'],
                     'initial_savanna':{'version':2,'finalizer_key':self.bls['eosio']['Public key'],
                     'proof_of_possession':self.bls['eosio']['Proof of Possession']},
                     'initial_configuration':json.loads((args.package/'config/test-low-cost/chain-parameters.json').read_text())}
            (self.root/'genesis.json').write_text(json.dumps(genesis))
            self.wallet=self.spawn([str(args.package/'bin/keosd'),'--data-dir',str(self.root/'wallet'),
                                    '--config-dir',str(self.root/'wallet'),'--http-server-address',''],'wallet.log')
            for _ in range(100):
                if (self.root/'wallet/keosd.sock').exists():break
                if self.wallet.poll()is not None:raise RuntimeError('keosd startup failed')
                time.sleep(.1)
            self.run(['wallet','create','-n','localtest','--file',str(self.root/'wallet/password.txt')])
            for k in self.accounts.values():self.run(['wallet','import','-n','localtest'],input_text=k['Private key']+'\n')
            self.start_node('genesis',['--genesis-json',str(self.root/'genesis.json')])
            info=self.wait_lib(17,timeout=90);self.report['chain_id']=info['chain_id']
            system=['eosio.token','eosio.stake','eosio.rex','eosio.ram','eosio.ramfee','eosio.bpay',
                    'eosio.vpay','eosio.saving','eosio.fees','eosio.msig','eosio.wrap','eosio.reserv','eosio.key']
            for n in [*system,*ordinary]:
                k=self.accounts.get(n,self.accounts['eosio'])['Public key']
                active=self.accounts['civicgovauth']['Public key']if n=='civicgovlead'else k
                self.tx(['create','account','eosio',n,k,active])
            self.tx(['set','account','permission','civicgovlead','upgrade',self.accounts['civicgovcode']['Public key'],'active'])
            for n in ['eosio.token','keytesttoken']:
                self.tx(['set','contract',n,str(self.contracts/'eosio.token')])
            if (args.artifacts/'test-only-token/keybadtokenx.wasm').exists():
                self.tx(['set','contract','keybadtokenx',str(args.artifacts/'test-only-token'),'keybadtokenx.wasm','keybadtokenx.abi'])
                auth={'threshold':1,'keys':[{'key':self.accounts['keybadtokenx']['Public key'],'weight':1}],
                      'accounts':[{'permission':{'actor':'keybadtokenx','permission':'eosio.code'},'weight':1}],'waits':[]}
                self.tx(['set','account','permission','keybadtokenx','active',json.dumps(auth),'owner'])
                self.action('create',{'owner':'keyusersolar','quantity':'100.0000 XTK'},'keybadtokenx',contract='keybadtokenx')
                self.action('create',{'owner':'keyusersolar','quantity':'100.0000 AIC'},'keybadtokenx',contract='keybadtokenx')
            self.action('create',{'issuer':'eosio','maximum_supply':'2100000000.0000 AIC'},'eosio.token',contract='eosio.token')
            self.action('issue',{'to':'eosio','quantity':'100000000.0000 AIC','memo':'private functional test'},'eosio',contract='eosio.token')
            self.action('create',{'issuer':'keytesttoken','maximum_supply':'1000000.000000 USDT'},'keytesttoken',contract='keytesttoken')
            self.action('issue',{'to':'keytesttoken','quantity':'10000.000000 USDT','memo':'private test token'},'keytesttoken',contract='keytesttoken')
            for n in ordinary:self.tx(['transfer','eosio',n,'100000.0000 AIC','test setup'])
            self.action('transfer',{'from':'keytesttoken','to':'keyusersolar','quantity':'1000.000000 USDT','memo':'test setup'},'keytesttoken',contract='keytesttoken')
            self.tx(['set','contract','eosio',str(self.contracts/'eosio.bios')])
            supported=self.rpc('get_supported_protocol_features',{'exclude_disabled':True},namespace='producer')
            byid={f['feature_digest']:f for f in supported}
            activated={f['feature_digest']for f in self.rpc('get_activated_protocol_features',{'limit':1000})['activated_protocol_features']}
            def activate(digest):
                if digest in activated:return
                for dep in byid[digest]['dependencies']:activate(dep)
                self.action('activate',{'feature_digest':digest},'eosio');activated.add(digest)
            required={'PUREEOS_ROTATION_CONTEXT','PUREEOS_SEVEN_DAY_GOVERNANCE','GET_CODE_HASH','CRYPTO_PRIMITIVES',
                      'DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2'}
            found=set()
            for f in supported:
                labels={s['value']for s in f['specification']}
                if labels&required:activate(f['feature_digest']);found|=labels&required
            self.check(found==required,'Required native features activated; deferred execution remains disabled')
            self.tx(['set','contract','eosio.bpay',str(self.contracts/'eosio.bpay')])
            self.tx(['set','contract','eosio.rex',str(self.contracts/'eosio.system/.rex'),'rex.results.wasm','rex.results.abi'])
            self.tx(['set','contract','eosio.reserv',str(self.contracts/'eosio.system/.powerup'),'powup.results.wasm','powup.results.abi'])
            self.deploy_system()
            self.action('init',{'version':0,'core':'4,AIC'},'eosio')
            self.action('setramrate',{'bytes_per_block':1024},'eosio')
            for n in ['eosio.key','eosio.reserv',*ordinary]:
                self.action('buyram',{'payer':'eosio','receiver':n,'quant':'100.0000 AIC'},'eosio')
            resource={'current_weight_ratio':None,'target_weight_ratio':None,'assumed_stake_weight':None,
                      'target_timestamp':None,'exponent':2.0,'decay_secs':86400,'min_price':'1.0000 AIC','max_price':'100.0000 AIC'}
            self.action('cfgpowerup',{'args':{'net':resource,'cpu':resource,'powerup_days':30,'min_powerup_fee':'0.0001 AIC'}},'eosio')
            # Users pay/own their own stake; never borrow voting stake from treasury.
            for n in ordinary:
                info=self.rpc('get_info');bid=bytes.fromhex(info['last_irreversible_block_id'])
                self.tx(['push','transaction',json.dumps({'expiration':(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(seconds=90)).strftime('%Y-%m-%dT%H:%M:%S'),
                    'ref_block_num':info['last_irreversible_block_num']&65535,'ref_block_prefix':int.from_bytes(bid[8:12],'little'),
                    'max_net_usage_words':0,'max_cpu_usage_ms':0,'delay_sec':0,'context_free_actions':[],'transaction_extensions':[],
                    'actions':[{'account':'eosio.token','name':'transfer','authorization':[{'actor':'eosio','permission':'active'}],
                        'data':{'from':'eosio','to':n,'quantity':'0.0001 AIC','memo':'self-owned bootstrap resources'}},
                        {'account':'eosio','name':'delegatebw','authorization':[{'actor':n,'permission':'active'}],
                         'data':{'from':n,'receiver':n,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':'100.0000 AIC','transfer':False}}]})])
            codeauth={'threshold':1,'keys':[],'accounts':[{'permission':{'actor':'eosio.key','permission':'eosio.code'},'weight':1}],'waits':[]}
            self.tx(['set','account','permission','eosio.key','active',json.dumps(codeauth),'owner'])
            wallet_artifacts=args.artifacts/'test-only-wallet'if args.fixture else args.artifacts
            self.tx(['set','contract','eosio.key',str(wallet_artifacts),'eosio.key.wasm','eosio.key.abi','-p','eosio.key@owner'])
            if args.fixture:
                self.tx(['set','contract','keysysmockup',str(args.artifacts/'test-only-system'),'keysysmockup.wasm','keysysmockup.abi'])
                self.action('setmarket',{'cpu':0,'net':0},'keysysmockup',contract='keysysmockup')
            self.cfg={'chain_id':info['chain_id'],'deployment':hashlib.sha256(os.urandom(32)).hexdigest(),
                'fee_contract':'eosio.token','fee_symbol':'4,AIC','system_hash':self.rpc('get_code_hash',{'account_name':'keysysmockup'if args.fixture else'eosio'})['code_hash'],
                'policy':{'minimum_fee':100,'unit_fee':10,'new_row_fee':500,'maximum_multiplier_bps':100000,
                          'epoch_seconds':1,'max_wallets':10,'max_balances':30,'max_assets':10,'max_native_payers':20}}
            # eosio.key active is code-only; deployment init is owner-authorized before sealing.
            self.action('init',self.cfg,'eosio.key@owner',contract='eosio.key')
            self.cfg['version']=2
            for n in ['gptkeyorigin','keyrelaymoon']:
                self.action('bondbp',{'producer':n},n)
                self.action('regproducer',{'producer':n,'producer_key':self.accounts[n]['Public key'],'url':'','location':0},n)
            if args.fixture:self.mock_bp()
            self.hot=self.key()
            self.run(['wallet','import','-n','localtest'],input_text=self.hot['Private key']+'\n')
            self.tx(['set','account','permission','keyrelaymoon','keyrelay',self.hot['Public key'],'active'])
            for op in ['ksend','withdraw','cancel']:
                self.tx(['set','action','permission','keyrelaymoon','eosio.key',op,'keyrelay'])
            self.action('regrelay',{'bp':'keyrelaymoon','permission':'keyrelay','endpoint':'https://relay.example.invalid'},'keyrelaymoon',contract='eosio.key')
            self.action('updateauth',{'account':'eosio.key','permission':'owner','parent':'','auth':codeauth},'eosio.key@owner')
            self.action('govinit',{'developer':'civicgovlead','review_seconds':604800,'protected_accounts':[]},'eosio@owner')
            self.check(self.governance()['sealed']and self.governance()['review_seconds']==604800,'Seven-day governance sealed after eosio.key deployment')
            self.check(self.table('global2')[0]['new_ram_per_block']==1024,'RAM1024 initialization preserved')
            permissions=self.rpc('get_account',{'account_name':'eosio.key'})['permissions']
            self.check(all(not p['required_auth']['keys']for p in permissions if p['perm_name']in ['owner','active']),
                'Wallet owner and active have no deployer private keys after initialization')
            self.report['scope']='eosio.key production WASM functional/security integration on one private TEST process; not TPS or independent multi-node consensus qualification'
            self.report['wallet_artifacts']={p.name:hashlib.sha256(p.read_bytes()).hexdigest()for p in wallet_artifacts.glob('eosio.key.*')}
            if args.fixture:self.report['scope']='TEST ONLY alternate-system-table wallet build; native consensus unchanged; injected eligibility/market cases are wallet logic evidence, not native blacklist qualification'
            if args.native_cleos:
                binary=args.native_cleos.parent/'cleos-key'
                if not binary.exists():binary=args.native_cleos
                self.report['native_cleos_SHA256']=hashlib.sha256(binary.read_bytes()).hexdigest()
            self.check(self.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==self.report['wallet_artifacts']['eosio.key.wasm'],
                'Actual deployed wallet WASM hash equals the selected production/fixture artifact')
            self.bootstrapping=False
        def ktable(self,name,scope='eosio.key'):
            return self.table(name,code='eosio.key',scope=scope)
        def token_balance(self,account,token='eosio.token'):
            rows=self.table('accounts',code=token,scope=account)
            return {r['balance'].split()[1]:r['balance']for r in rows}
        def transfer(self,account,to,amount,memo,token='eosio.token',failure=None):
            return self.action('transfer',{'from':account,'to':to,'quantity':amount,'memo':memo},account,contract=token,failure=failure)
        def memo(self,key,cap='100000'):
            return 'PK2|'+key['Public key']+'|'+cap+'|'+os.urandom(32).hex()
        def state(self):
            return {n:self.ktable(n)for n in ['config','wallets','assets','nativefees','feebuckets','pricing']}|{'balances':[
                {'wallet':w['id'],'rows':self.ktable('balances',str(w['id']))}for w in self.ktable('wallets')]}
        def signed(self,op,request,owner,original=None,actor=None,attack=None,ttl=120):
            payer=self.accounts[actor.split('@')[0]]if actor else(self.accounts['keyusersolar']if op.startswith('self')else self.hot)
            result=subprocess.run(['node',str(source/'tests/sign.mjs')],input=json.dumps({
                'config':self.cfg,'info':self.rpc('get_info'),'operation':op,'request':request,
                'key':owner['Private key'],'payer_key':payer['Private key'],'payer':'keyusersolar'if op.startswith('self')else'',
                'original':original['body']if original else None,'actor':actor,'attack':attack,'ttl':ttl}),capture_output=True,text=True,timeout=30)
            if result.returncode:raise RuntimeError('Test signing bridge failed: '+self.redact(result.stderr))
            return json.loads(result.stdout)
        def request(self,owner,op='ksend',**patch):
            wallet=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],owner['Public key']))
            common={'from':owner['Public key'],'auth_epoch':str(wallet['auth_epoch']),'request_id':os.urandom(32).hex(),'relayer':'keyrelaymoon','reward_to':'keyrelaymoon','max_fee':'10.0000 AIC'}
            if op in ['cancel','selfcancel']:common['next_epoch']=str(int(common['auth_epoch'])+7)
            else:common.update({'token_contract':'eosio.token','quantity':'1.0000 AIC','memo':'native account receives full amount',
                              'to':self.receiver['Public key']if op=='ksend'else 'keyuserlunar'})
            if op.startswith('self'):common.update(relayer='',reward_to='')
            return common|patch
        @staticmethod
        def pub_equal(a,b):
            # cleos may render legacy EOS keys. Compare decoded native serialization.
            def decode(s):
                alphabet='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';v=0
                for c in s[7:]if s.startswith('PUB_K1_')else s[3:]:v=v*58+alphabet.index(c)
                return v.to_bytes(37,'big')[:33]
            return decode(a)==decode(b)
        def execute(self,op,owner,request=None,sig=None,failure=None,actor=None,attack=None):
            request=request or self.request(owner,op)
            signed=sig or self.signed(op,request,owner,actor=actor,attack=attack)
            if sig and (request!=sig['prepared']['request'] or actor or attack):signed=self.signed(op,request,owner,original=sig,actor=actor,attack=attack)
            return self.tx(['push','transaction',json.dumps(signed['signed']),'-s'],failure=failure)
        def tests(self):
            self.owner,self.receiver=self.key(),self.key()
            self.transfer('keyusersolar','eosio.key','100.0000 AIC','fee')
            self.transfer('keyrelaymoon','eosio.key','10.0000 AIC','fee')
            self.transfer('keyusersolar','eosio.key','1000.0000 AIC',self.memo(self.owner))
            self.transfer('keyusersolar','eosio.key','100.000000 USDT',self.memo(self.owner),token='keytesttoken')
            self.check(len(self.ktable('wallets'))==1 and len(self.ktable('assets'))==2,'Native deposits create one canonical wallet and distinct multi-token reserves')
            read=subprocess.run(['node',str(source/'tests/read.mjs')],input=json.dumps({'endpoint':self.url,'key':self.owner['Public key'],
                'token':'keytesttoken','symbol':'6,USDT'}),capture_output=True,text=True,timeout=30)
            assert read.returncode==0,self.redact(read.stderr)
            self.check(json.loads(read.stdout)['balance']=='100000000','JS SDK reads actual key/asset secondary indices and exact six-decimal balance')
            if args.native_cleos:self.test_cli()
            self.test_nonce_race()
            self.test_proof_attacks()
            self.test_hot_key_rotation()
            before=self.state();native=self.token_balance('keyusersolar')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.receiver,'0'),failure='fee exceeds')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native,'Fee cap rejection rolls back token transfer, fee, wallet and reserve')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC','not a key',failure='use PK2')
            req=self.request(self.owner);sig=self.signed('ksend',req,self.owner)
            self.execute('ksend',self.owner,req,sig)
            after=self.state()
            self.execute('ksend',self.owner,req,sig,failure='duplicate transaction')
            self.check(self.state()==after,'Duplicate owner intent cannot debit twice')
            req=self.request(self.owner,'withdraw',quantity='3.4567 AIC');sig=self.signed('withdraw',req,self.owner)
            self.execute('withdraw',self.owner,req|{'quantity':'3.4568 AIC'},sig,failure='recovered key')
            before=self.token_balance('keyuserlunar')['AIC'];tx=self.execute('withdraw',self.owner,req,sig)
            from decimal import Decimal
            diff=Decimal(self.token_balance('keyuserlunar')['AIC'].split()[0])-Decimal(before.split()[0])
            self.check(diff==Decimal('3.4567'),'Public-key to native text account delivers exact AIC principal; fee is additional')
            req=self.request(self.owner,'withdraw',token_contract='keytesttoken',quantity='1.234567 USDT')
            row_count=self.ktable('config')[0]['balance_count']
            self.execute('withdraw',self.owner,req)
            self.check(self.ktable('config')[0]['balance_count']==row_count+1,'Opening a native token recipient row is charged and consumes the bounded storage budget')
            self.check(self.token_balance('keyuserlunar','keytesttoken')['USDT']=='1.234567 USDT','Six-decimal other-token withdrawal to native text account is full amount')
            self.execute('withdraw',self.owner,self.request(self.owner,'withdraw',to='noaccounthere'),failure='invalid withdrawal')
            self.execute('ksend',self.owner,self.request(self.owner),attack='domain',failure='recovered key')
            self.execute('ksend',self.owner,actor='keyrelaymoon@active',failure='recovered key')
            cancel=self.request(self.owner,'cancel');old=self.request(self.owner);oldproof=self.signed('ksend',old,self.owner)
            self.execute('cancel',self.owner,cancel)
            self.execute('ksend',self.owner,old,oldproof,failure='authorization epoch revoked')
            self.check(True,'Cancellation advances only the revocation epoch and invalidates outstanding older authorizations')
            self.action('setrelay',{'bp':'keyrelaymoon','enabled':False},'keyrelaymoon',contract='eosio.key')
            self.execute('withdraw',self.owner,failure='relay disabled')
            self.execute('selfout',self.owner)
            self.check(True,'Owner-signed self withdrawal works while selected commercial relay is disabled')
            self.execute('selfcancel',self.owner)
            self.check(True,'Owner self cancellation works independently of commercial relay eligibility')
            self.action('setrelay',{'bp':'keyrelaymoon','enabled':True},'keyrelaymoon',contract='eosio.key')
            # Native standby maintenance must revoke commercial eligibility immediately.
            self.action('setmaint',{'producer':'keyrelaymoon','maintenance':True},'keyrelaymoon')
            if args.fixture:self.mock_bp(maintenance_state=2)
            self.execute('ksend',self.owner,failure='BP in maintenance')
            self.execute('selfout',self.owner)
            self.check(True,('Fixture'if args.fixture else'Real native')+' BP maintenance rejects relay work while preserving owner self exit')
            earned=next(b['relay_earned']for b in self.ktable('feebuckets')if b['owner']=='keyrelaymoon')
            amount=f'{int(earned)//10000}.{int(earned)%10000:04d} AIC'
            self.action('claim',{'bp':'keyrelaymoon','to':'keyrelaymoon','quantity':amount,'max_fee':'1.0000 AIC'},'keyrelaymoon',contract='eosio.key')
            self.check(next(b['relay_earned']for b in self.ktable('feebuckets')if b['owner']=='keyrelaymoon')==0,'Revoked relay can claim previously earned service income without new relay eligibility')
            self.action('refundfee',{'owner':'keyusersolar','to':'keyusersolar','quantity':'1.0000 AIC','max_fee':'1.0000 AIC'},'keyusersolar',contract='eosio.key')
            self.check(all(not a['busy']for a in self.ktable('assets')),'All outgoing token interactions finish without outstanding busy locks')
            if (args.artifacts/'test-only-token/keybadtokenx.wasm').exists():self.test_incompatible_token()
            self.test_storage_and_nonce()
            if args.fixture:self.test_fixture_states()
            else:self.test_real_market()
            self.test_large_nonce()
            self.test_overspend()
            self.check_reserves()
            self.report['final_state']=self.state()
        def test_large_nonce(self):
            large='9007199254740993'
            request=self.request(self.owner,'selfcancel',next_epoch=large)
            self.execute('selfcancel',self.owner,request)
            read=subprocess.run(['node',str(source/'tests/read.mjs')],input=json.dumps({'endpoint':self.url,'key':self.owner['Public key'],
                'token':'eosio.token','symbol':'4,AIC'}),capture_output=True,text=True,timeout=30)
            assert read.returncode==0,self.redact(read.stderr)
            self.check(str(json.loads(read.stdout)['wallet']['auth_epoch'])==large,'Actual RPC and JS SDK retain an on-chain revocation epoch above 2^53 exactly')
            if args.native_cleos:
                prepared=json.loads(self.run(['key','prepare',self.owner['Public key'],'keyuserlunar','1.0000 AIC','--self','--payer','keyusersolar','--max-fee','1.0000 AIC']))
                self.check(prepared['request']['auth_epoch']==large,'Native cleos prepares the exact uint64 epoch without JavaScript rounding')
                signed=json.loads(self.run(['key','sign',json.dumps(prepared)]))
                self.tx(['key','push',json.dumps(signed),'-p','keyusersolar@active'])
            else:self.execute('selfout',self.owner)
            wallet=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            self.check(int(wallet['auth_epoch'])==int(large),'Owner-authorized withdrawal succeeds with large epoch without consuming a transfer sequence')
        def test_nonce_race(self):
            from concurrent.futures import ThreadPoolExecutor
            from decimal import Decimal
            sender=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            epoch=int(sender['auth_epoch']);balance=int(next(b['amount']for b in self.ktable('balances',str(sender['id']))if int(b['asset_id'])==1))
            recipients=['keyuserlunar','keyusersolar'];before=[Decimal(self.token_balance(n)['AIC'].split()[0])for n in recipients]
            requests=[self.request(self.owner,'withdraw',to=to,quantity=q,memo='independent device '+str(i))for i,(to,q)in enumerate(zip(recipients,['0.1000 AIC','0.2000 AIC']))]
            packets=[self.signed('withdraw',r,self.owner)for r in requests];assert all(int(r['auth_epoch'])==epoch for r in requests)
            lib=self.rpc('get_info')['last_irreversible_block_num'];children=[]
            try:
                for packet in packets:
                    children.append(subprocess.Popen([str(self.cleos),'--no-auto-keosd','--url',self.url,'--wallet-url','unix://'+str(self.root/'wallet/keosd.sock'),
                        'push','transaction',json.dumps(packet['signed']),'-s','-j'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True))
                with ThreadPoolExecutor(max_workers=2)as executor:outputs=list(executor.map(lambda c:c.communicate(timeout=60),children))
                assert all(c.returncode==0 for c in children),self.redact(str(outputs))
                confirmations=[self.wait_transaction(json.loads(o[0])['transaction_id'],lib,timeout=90)for o in outputs]
                now=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
                remaining=int(next(b['amount']for b in self.ktable('balances',str(sender['id']))if int(b['asset_id'])==1))
                deltas=[Decimal(self.token_balance(n)['AIC'].split()[0])-before[i]for i,n in enumerate(recipients)]
                self.check(int(now['auth_epoch'])==epoch and balance-remaining==3000+260 and deltas==[Decimal('0.1000'),Decimal('0.2000')],
                    'Two independently prepared same-epoch transactions both finalize; exact principals and one extra fee each, no shared nonce allocator')
                self.report['independent_device_concurrency']={'epoch':str(epoch),'confirmations':confirmations,'fee_atoms_each':130,'principal_atoms':[1000,2000]}
            finally:
                for c in children:
                    if c.poll()is None:c.kill();c.wait()
        def test_proof_attacks(self):
            req=self.request(self.owner,'withdraw',quantity='0.0100 AIC');signed=self.signed('withdraw',req,self.owner)
            for attack,error in [('expiration','recovered key'),('extra-root','recovered key'),('domain','recovered key'),('invalid-proof',''),('extra-cfd','extra context-free data')]:
                before=self.state();native=self.token_balance('keyuserlunar')
                self.execute('withdraw',self.owner,req,signed,attack=attack,failure=error)
                self.check(self.state()==before and self.token_balance('keyuserlunar')==native,'Owner proof attack '+attack+' rejects without principal, fee or reserve changes')
            self.execute('withdraw',self.owner,req,signed)
            self.check(True,'Failed same-ID invalid proof does not poison later valid transaction')
            after=self.state();self.execute('withdraw',self.owner,req,signed,failure='duplicate transaction')
            self.check(self.state()==after,'Exact native retry after success cannot debit twice')
            self.tx(['push','transaction',json.dumps(signed['signed']),'-s','--compression','zlib'],failure='duplicate transaction')
            self.check(self.state()==after,'Changing native packet compression cannot create a second payment')
            expired=self.signed('withdraw',self.request(self.owner,'withdraw',quantity='0.0100 AIC'),self.owner,ttl=1)
            self.wait_lib(self.rpc('get_info')['head_block_num']+6)
            self.tx(['push','transaction',json.dumps(expired['signed']),'-s'],failure='expired')
            self.wait_lib(self.rpc('get_info')['head_block_num']+6)
            self.execute('withdraw',self.owner,req,signed,failure='duplicate transaction')
            shortreq=self.request(self.owner,'withdraw',quantity='0.0100 AIC')
            short=self.signed('withdraw',shortreq,self.owner,ttl=15)
            self.execute('withdraw',self.owner,shortreq,short)
            self.wait_lib(self.rpc('get_info')['head_block_num']+36)
            before=self.state();self.execute('withdraw',self.owner,shortreq,short,failure='expired')
            self.check(self.state()==before,'Already executed short-lived packet remains rejected after expiry and native dedup pruning')
            self.report['native_replay_expiration_source_review']={'dedup_pruning':'now > expiration','native_acceptance':'expiration >= pending_block_time','gap':False}
        def test_hot_key_rotation(self):
            req=self.request(self.owner,'ksend');old=self.signed('ksend',req,self.owner)
            replacement=self.key();self.run(['wallet','import','-n','localtest'],input_text=replacement['Private key']+'\n')
            self.tx(['set','account','permission','keyrelaymoon','keyrelay',replacement['Public key'],'active'])
            before=self.state();self.execute('ksend',self.owner,req,old,failure='satisfy')
            self.check(self.state()==before,'Revoked native BP relay hot key cannot authorize pending wallet transaction')
            self.hot=replacement
            renewed=self.signed('ksend',req,self.owner,original=old)
            self.check(renewed['prepared']['transaction_id']==old['prepared']['transaction_id']and renewed['signature']==old['signature'],
                'Rotating only native resource signature preserves holder proof and native transaction ID')
            self.execute('ksend',self.owner,req,renewed)
        def test_overspend(self):
            owner=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            balance=int(next(b['amount']for b in self.ktable('balances',str(owner['id']))if int(b['asset_id'])==1))
            request=self.request(self.owner,'selfout',quantity=f'{balance//10000}.{balance%10000:04d} AIC')
            before=self.state();native=self.token_balance('keyuserlunar')
            self.execute('selfout',self.owner,request,failure='insufficient principal balance')
            self.check(self.state()==before and self.token_balance('keyuserlunar')==native,'Attempt to spend entire principal without additional AIC fee fails atomically')
        def check_reserves(self):
            from decimal import Decimal
            balances=[b for w in self.ktable('wallets')for b in self.ktable('balances',str(w['id']))]
            for a in self.ktable('assets'):
                liability=sum(int(b['amount'])for b in balances if int(b['asset_id'])==int(a['id']))
                if int(a['id'])==1:
                    liability+=sum(int(f['amount'])for f in self.ktable('nativefees'))
                    liability+=sum(int(f['network'])+int(f['relay_earned'])for f in self.ktable('feebuckets'))
                precision,symbol=a['token_symbol'].split(',')
                raw=self.token_balance('eosio.key',a['token_contract']).get(symbol,'0 '+symbol)
                actual=int(Decimal(raw.split()[0])*(10**int(precision)))
                self.check(actual==int(a['reserve'])==liability,'Exact vault reserve equals all ledger liabilities for '+a['token_contract']+'::'+a['token_symbol'])
        def test_real_market(self):
            self.action('powerup',{'payer':'keyusersolar','receiver':'keyusersolar','days':30,'net_frac':0,
                'cpu_frac':500000000000000,'max_payment':'1000.0000 AIC'},'keyusersolar')
            market=self.table('powup.state',scope='0')[0]
            use=max(max(int(market[r]['adjusted_utilization']),int(market[r]['utilization']))*10000//int(market[r]['weight'])for r in ['cpu','net'])
            expected=10000+90000*use*use//100000000
            self.execute('selfout',self.owner)
            actual=int(self.ktable('pricing')[0]['multiplier_bps'])
            self.check(actual==expected and actual>10000,'Real on-chain PowerUp market reservation raises dynamic wallet fee multiplier deterministically')
            self.report['real_market_test']={'cpu':market['cpu'],'net':market['net'],'expected_multiplier_bps':expected,'actual_multiplier_bps':actual}
        def mock_bp(self,**patch):
            self.action('setbp',{'owner':'keyrelaymoon','key':self.accounts['keyrelaymoon']['Public key'],
                'active':True,'banned':False,'state':0,'balance':100000000,'maintenance_state':0}|patch,
                'keysysmockup',contract='keysysmockup')
        def test_fixture_states(self):
            self.mock_bp()
            self.action('setmarket',{'cpu':10000,'net':0},'keysysmockup',contract='keysysmockup')
            self.execute('ksend',self.owner)
            self.check(int(self.ktable('pricing')[0]['multiplier_bps'])==100000,'TEST FIXTURE: fully occupied resource market increases on-chain multiplier to configured maximum')
            self.mock_bp(banned=True)
            snapshot=self.state();req=self.request(self.owner)
            self.execute('ksend',self.owner,req,failure='blacklisted BP')
            self.action('regrelay',{'bp':'keyrelaymoon','permission':'anotherkey','endpoint':'https://new.example.invalid'},
                        'keyrelaymoon',contract='eosio.key',failure='blacklisted BP')
            self.check(self.state()==snapshot,'TEST FIXTURE: blacklist blocks existing relay and permission re-registration without debiting owner')
            self.execute('selfout',self.owner)
            self.mock_bp(state=1)
            self.execute('ksend',self.owner,failure='normal full BP bond')
            self.mock_bp(active=False)
            self.execute('ksend',self.owner,failure='registered active BP')
            self.mock_bp()
            self.tx(['set','contract','keysysmockup',str(self.contracts/'eosio.token'),'eosio.token.wasm','eosio.token.abi'])
            self.execute('ksend',self.owner,failure='system code changed')
            self.execute('selfout',self.owner)
            self.check(True,'TEST FIXTURE: system schema hash change fails closed for commercial sends and retains owner self withdrawal')
        def test_incompatible_token(self):
            def mode(value):self.action('setmode',{'value':value},'keybadtokenx',contract='keybadtokenx')
            mode(1);before=self.state()
            self.transfer('keyusersolar','eosio.key','1.0000 XTK',self.memo(self.owner),token='keybadtokenx',failure='exact reserve increase')
            self.check(self.state()==before,'Taxed incoming token is rejected atomically instead of under-crediting recipient')
            mode(3)
            self.action('transfer',{'from':'keyusersolar','to':'eosio.key','quantity':'1.0000 XTK','memo':self.memo(self.owner)},
                        'keyuserlunar',contract='keybadtokenx',failure='missing authority')
            self.check(self.state()==before,'Forged from notification cannot consume another native account fee credit')
            mode(0);self.transfer('keyusersolar','eosio.key','10.0000 XTK',self.memo(self.owner),token='keybadtokenx')
            inner=self.request(self.owner,'selfout',token_contract='keybadtokenx',quantity='1.0000 XTK')
            inner['request_id']=os.urandom(32).hex()
            self.action('arm',{'request':inner,'sig':self.signed('selfout',inner,self.owner)['signature']},'keybadtokenx',contract='keybadtokenx')
            for value,error in [(1,'full principal'),(2,'missing standard outgoing'),(4,'authorization'),(5,'top-level owner operation')]:
                mode(value);before=self.state();native=self.token_balance('keyuserlunar','keybadtokenx')
                self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keybadtokenx',quantity='1.0000 XTK'),failure=error)
                self.check(self.state()==before and self.token_balance('keyuserlunar','keybadtokenx')==native,'Incompatible outgoing token mode '+str(value)+' rolls back principal, fee, epoch and reserve')
            mode(0);self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keybadtokenx',quantity='1.0000 XTK'))
            before_ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            before_rows=int(self.ktable('config')[0]['balance_count'])
            mode(6);self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keybadtokenx',quantity='1.0000 XTK'))
            after_ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            self.check(after_ram-before_ram>=4096 and int(self.ktable('config')[0]['balance_count'])==before_rows,
                'KNOWN RELEASE BLOCKER: unreviewed issuer bills custody RAM outside the wallet row counter')
            self.report['unreviewed_issuer_RAM_exposure']={'reproduced':True,'extra_RAM_bytes':after_ram-before_ram,'wallet_row_counter_changed':False}
            self.report['release_qualified']=False
            mode(0)
            self.transfer('keyusersolar','eosio.key','1.0000 AIC','fee',token='keybadtokenx',failure='fee funding accepts only configured AIC')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.owner),token='keybadtokenx')
            real=self.token_balance('keyuserlunar')['AIC']
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keybadtokenx',quantity='0.5000 AIC'))
            self.check(self.token_balance('keyuserlunar')['AIC']==real and self.token_balance('keyuserlunar','keybadtokenx')['AIC']=='0.5000 AIC',
                'Identically named AIC from another issuer is a separate asset and cannot fund real AIC fees')
        def test_storage_and_nonce(self):
            # Owner-signed escape drains AIC exactly after its independently paid base fee.
            sender=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            balance=int(next(b['amount']for b in self.ktable('balances',str(sender['id']))if int(b['asset_id'])==1))
            # Empty memo => six work units. At low occupancy this is the 100-atom floor;
            # fixture hash-change self exit deliberately uses the same minimum profile.
            q=balance-100
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',quantity=f'{q//10000}.{q%10000:04d} AIC',memo=''))
            empty=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            epoch=int(empty['auth_epoch']);self.transfer('keyusersolar','eosio.key','10.0000 AIC',self.memo(self.owner))
            refill=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],self.owner['Public key']))
            self.check(int(refill['auth_epoch'])==epoch and refill['id']==sender['id'],'Zero AIC balance and later funding preserve wallet identity and revocation epoch')
            # Invalid compressed secp256k1 point x=0 has no square root; construct a
            # correct Base58 checksum so this tests the native curve check itself.
            payload=b'\x02'+b'\0'*32;checksum=hashlib.new('ripemd160',payload+b'K1').digest()[:4]
            alphabet='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz';value=int.from_bytes(payload+checksum,'big');encoded=''
            while value:value,r=divmod(value,58);encoded=alphabet[r]+encoded
            invalid={'Public key':'PUB_K1_'+encoded};before=self.state()
            self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(invalid),failure='not on secp256k1')
            self.check(self.state()==before,'Checksum-valid off-curve recipient is rejected before allocating a wallet')
            while len(self.ktable('wallets'))<10:self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.key()))
            before=self.state();self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.key()),failure='wallet storage capacity')
            self.check(self.state()==before,'Configured wallet storage cap rejects RAM growth with atomic fee/principal rollback')
        def test_cli(self):
            self.run(['wallet','import','-n','localtest'],input_text=self.owner['Private key']+'\n')
            native_before=self.token_balance('keyuserlunar')['AIC']
            self.tx(['transfer','keyusersolar',self.owner['Public key'],'1.0000 AIC',
                     '--max-key-fee','1.0000 AIC','--key-fee-fund','1.0000 AIC'])
            prepared=json.loads(self.run(['key','prepare',self.owner['Public key'],'keyuserlunar','2.3456 AIC',
                '--bp','keyrelaymoon','--max-fee','1.0000 AIC']))
            sdk=subprocess.run(['node',str(source/'tests/sign.mjs')],input=json.dumps({'abi':str(args.artifacts/'eosio.key.abi'),
                'config':self.cfg,'prepared':prepared,'info':self.rpc('get_info'),'operation':prepared['operation'],'request':prepared['request'],'key':self.owner['Private key'],'payer_key':self.hot['Private key']}),
                capture_output=True,text=True,timeout=30)
            assert sdk.returncode==0,self.redact(sdk.stderr)
            self.check(json.loads(sdk.stdout)['digest']==prepared['digest'],'Native cleos and JS SDK produce the same owner signing digest')
            signed=json.loads(self.run(['key','sign',json.dumps(prepared)]))
            before_export=self.state()
            exported=json.loads(self.run(['key','push',json.dumps(signed),'-d','-p','keyrelaymoon@keyrelay']))
            unsigned=json.loads(self.run(['key','push',json.dumps(signed),'-d','-s']))
            self.wait_lib(self.rpc('get_info')['head_block_num']+3)
            self.check(self.state()==before_export and len(exported['signatures'])==1 and len(unsigned['signatures'])==0,
                'Native cleos --dont-broadcast exports safely, including --skip-sign, without executing the authorized transfer')
            self.tx(['key','push',json.dumps(signed),'-p','keyrelaymoon@keyrelay'])
            from decimal import Decimal
            self.check(Decimal(self.token_balance('keyuserlunar')['AIC'].split()[0])-Decimal(native_before.split()[0])==Decimal('2.3456'),
                'Real cleos prepare/sign/push public-key to native text account sends full principal')
            self.test_relay_headers(signed)
            if not args.fixture:self.test_http_relay()
            # Original text-account transfer remains available.
            self.tx(['transfer','keyusersolar','keyuserlunar','0.0001 AIC','existing native transfer'])
            before=self.state();native=self.token_balance('keyusersolar')
            self.tx(['transfer','keyusersolar',self.receiver['Public key'],'1.0000 AIC','--max-key-fee','0.0000 AIC',
                '--key-fee-fund','1.0000 AIC'],failure='fee exceeds')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native,'Cleos combined fee funding and public-key deposit roll back atomically')
        def test_relay_headers(self,signed):
            import threading
            from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
            captured={}
            class Handler(BaseHTTPRequestHandler):
                def do_POST(self):
                    captured['headers']=dict(self.headers);captured['body']=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    self.send_response(202);self.send_header('Content-Type','application/json');self.end_headers();self.wfile.write(b'{"status":"test-captured"}')
                def log_message(self,*args):pass
            server=ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            endpoint='http://127.0.0.1:'+str(server.server_port)
            try:
                for name,value in [('Authorization','test-rpc-credential'),('X-Private-RPC','sentinel')]:
                    self.run(['--header',name+': '+value,'key','submit',json.dumps(signed),'--relay-url',endpoint,'--allow-http-for-test'])
                    headers={k.lower():v for k,v in captured['headers'].items()}
                    assert name.lower()not in headers and captured['body']==signed
                self.check(True,'Native cleos external relay request forwards no RPC credentials/custom headers')
                self.run(['key','submit',json.dumps(signed),'--relay-url',endpoint+'@example.invalid','--allow-http-for-test'],failure='Valid HTTPS endpoint')
                self.check(True,'Native cleos loopback exception rejects URL userinfo/remote-host confusion')
            finally:server.shutdown();server.server_close();thread.join()
        def test_http_relay(self):
            import urllib.request
            from native_savanna_genesis_test import free_port
            port=free_port();keyfile=self.root/'wallet/relay-key.txt'
            keyfile.write_text(self.hot['Private key']+'\n');keyfile.chmod(0o600)
            state=self.root/'relay-state';state.mkdir(mode=0o700)
            log=(self.root/'relay.log').open('w');self.logs.append(log)
            server=subprocess.Popen(['node',str(source/'relayer/server.mjs')],cwd=source,stdout=log,stderr=subprocess.STDOUT,
                env={**os.environ,'NODEOS_URL':self.url,'BP_ACCOUNT':'keyrelaymoon','BP_PERMISSION':'keyrelay','RELAY_KEY_FILE':str(keyfile),
                     'RELAY_STATE':str(state),'KEY_CODE_HASH':self.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash'],'PORT':str(port)})
            endpoint='http://127.0.0.1:'+str(port)
            try:
                for _ in range(100):
                    try:
                        with urllib.request.urlopen(endpoint+'/health',timeout=1)as r:json.load(r)
                        break
                    except OSError:
                        if server.poll()is not None:raise RuntimeError('Reference relay exited: '+(self.root/'relay.log').read_text()[-2000:])
                        time.sleep(.1)
                prepared=json.loads(self.run(['key','prepare',self.owner['Public key'],self.receiver['Public key'],'0.1234 AIC',
                    '--bp','keyrelaymoon','--max-fee','1.0000 AIC']))
                signed=json.loads(self.run(['key','sign',json.dumps(prepared)]))
                lib=self.rpc('get_info')['last_irreversible_block_num']
                reply=json.loads(self.run(['key','submit',json.dumps(signed),'--relay-url',endpoint,'--allow-http-for-test']))
                confirmed=self.wait_transaction(reply['transaction_id'],lib,timeout=90)
                for _ in range(25):
                    with urllib.request.urlopen(endpoint+'/status/'+reply['intent'],timeout=5)as r:status=json.load(r)
                    if status['status']=='finalized':break
                    time.sleep(2)
                self.check(status['status']=='finalized'and status['confirmation']['block_id']==confirmed['block_id'],
                    'Native cleos submits to real BP relay; relay finality matches canonical irreversible transaction receipt')
                again=json.loads(self.run(['key','submit',json.dumps(signed),'--relay-url',endpoint,'--allow-http-for-test']))
                self.check(again['transaction_id']==reply['transaction_id']and again['status']=='finalized','Actual HTTP relay retry returns the same transaction instead of spending twice')
                self.report['http_relay_confirmation']=confirmed
            finally:self.stop(server)
    test=Test(args.work,args.package/'contracts',resource_split=True)
    try:
        test.setup();test.tests();test.report['status']='PASS'
    except Exception as e:
        test.report['status']='FAIL';test.report['error']=test.redact(str(e))
        print('FAIL: '+test.redact(str(e)),flush=True)
    finally:
        for child in [test.node,test.wallet]:
            try:test.stop(child)
            except Exception as e:test.report.setdefault('shutdown_errors',[]).append(str(e));test.report['status']='FAIL'
        for log in test.logs:log.close()
        (args.work/'RESULT.json').write_text(json.dumps(test.report,indent=2)+'\n')
        print('RESULT '+test.report['status'],flush=True)
    return 0 if test.report['status']=='PASS'else 1
if __name__=='__main__':raise SystemExit(main())
