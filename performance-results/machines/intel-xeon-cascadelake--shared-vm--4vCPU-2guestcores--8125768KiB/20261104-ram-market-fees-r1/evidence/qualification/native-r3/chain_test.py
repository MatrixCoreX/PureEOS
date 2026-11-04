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
    p.add_argument('--review-seconds',type=int,choices=[1800,604800],default=604800)
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
        def run(self,args,*,input_text=None,failure=None):
            result=subprocess.run([str(self.cleos),'--no-auto-keosd','--url',self.url,
                '--wallet-url',f'unix://{self.root}/wallet/keosd.sock',*args],input=input_text,
                capture_output=True,text=True,timeout=40)
            output=self.redact(result.stdout+result.stderr)
            self.last_cpu_response=None
            if result.returncode and 'tx_cpu_usage_exceeded'in output:
                try:
                    value=json.JSONDecoder().raw_decode(output.lstrip())[0]
                    if value.get('processed',{}).get('receipt')is None and value['processed'].get('except',{}).get('name')=='tx_cpu_usage_exceeded':
                        self.last_cpu_response=(value,output)
                except (ValueError,KeyError,TypeError):pass
            diagnostic=output if len(output)<=10000 else output[:4000]+'\n... middle omitted ...\n'+output[-6000:]
            if self.last_cpu_response is not None:raise RuntimeError('Definitive tx_cpu_usage_exceeded: '+diagnostic)
            if failure is not None:
                if result.returncode==0 or failure.lower()not in output.lower():
                    raise RuntimeError(f'Expected rejection ({failure}), got {result.returncode}: {diagnostic}')
            elif result.returncode:raise RuntimeError(f'cleos {args[:3]} failed: {diagnostic}')
            return result.stdout
        def audit_state(self):
            state={'wallet':self.state(),'governance':self.governance(),'code':{},'native':{},'RAM':{}}
            standard=self.report['artifacts']['eosio.token/eosio.token.wasm']
            for code in ['eosio','eosio.key','eosio.token','keyusdtoken1']:
                state['code'][code]=self.rpc('get_code_hash',{'account_name':code})['code_hash']
            for owner in ['eosio.key','keyusersolar','keyuserlunar','keyrelaymoon','keyusdtoken1']:
                state['RAM'][owner]=self.rpc('get_account',{'account_name':owner})['ram_usage']
                for contract,symbol in [('eosio.token','AIC'),('keyusdtoken1','USD')]:
                    if state['code'][contract]==standard:
                        state['native'][owner+':'+symbol]=self.rpc('get_currency_balance',{'code':contract,'account':owner,'symbol':symbol})
            return state
        def tx(self,args,*,failure=None):
            # Freeze/sign exactly once. CPU retries resend identical bytes and
            # the same native ID; they never create another payment intent.
            self.nonce+=1
            folder=self.root/'immutable-packets';folder.mkdir(exist_ok=True)
            path=folder/(str(self.nonce)+'.json')
            try:exported=self.run([*args,'-j','-x',str(120+self.nonce),'--dont-broadcast'])
            except RuntimeError as error:
                if failure is not None and failure.lower()in str(error).lower():return None
                raise
            signed=json.loads(exported);assert 'actions'in signed and 'signatures'in signed
            path.write_text(json.dumps(signed))
            packed=json.loads(self.run(['convert','pack_transaction',str(path)]))
            assert packed['compression']in ['none',0]
            transaction_id=hashlib.sha256(bytes.fromhex(packed['packed_trx'])).hexdigest()
            packet_hash=hashlib.sha256(json.dumps(packed,sort_keys=True).encode()).hexdigest()
            # Preserve native set-contract's default wire compression. Replaying
            # its signed export without zlib changes NET billing and can reject
            # a valid deployment at the unchanged per-transaction NET cap.
            compression=args[args.index('--compression')+1]if '--compression'in args else(
                'zlib'if args[:2]==['set','contract']else 'none')
            for attempt in range(1,6):
                before=None if self.bootstrapping else self.audit_state()
                info=self.rpc('get_info')
                try:
                    raw=self.run(['push','transaction',str(path),'-s','-j','--compression',compression],failure=failure)
                    if failure is not None:return None
                    result=json.loads(raw)
                    assert result['transaction_id']==transaction_id,'Native client changed frozen transaction bytes'
                    assert result['processed']['receipt']['status']=='executed'
                    confirmed=self.wait_transaction(transaction_id,info['last_irreversible_block_num'])
                    confirmed['reported_block_num']=result['processed']['block_num']
                    self.report.setdefault('confirmed_transactions',[]).append(confirmed)
                    return result
                except RuntimeError as error:
                    if 'tx_cpu_usage_exceeded'not in str(error)or attempt==5:raise
                    response=getattr(self,'last_cpu_response',None)
                    if not response:raise
                    result,raw=response
                    if result['transaction_id']!=transaction_id:raise RuntimeError('CPU failure native ID mismatch')
                    finalized=self.wait_lib(max(info['head_block_num'],result['processed']['block_num'])+2)
                    scanned=[]
                    for n in range(info['last_irreversible_block_num']+1,finalized['last_irreversible_block_num']+1):
                        block=self.rpc('get_block',{'block_num_or_id':n});scanned.append({'block_num':n,'block_id':block['id']})
                        for receipt in block['transactions']:
                            trx=receipt['trx'];actual=trx if isinstance(trx,str)else trx['id']
                            if actual==transaction_id:raise RuntimeError('Retry stopped: original transaction is canonical; reconcile its receipt')
                    after=None if self.bootstrapping else self.audit_state()
                    if before!=after:raise RuntimeError('Retry stopped: wallet/native/RAM/governance state changed')
                    errors=self.root/'cpu-failures';errors.mkdir(exist_ok=True)
                    index=len(self.report.get('resource_cpu_retries',[]))+1;file=errors/(str(index)+'.log');file.write_text(raw)
                    digest=hashlib.sha256(json.dumps(before,sort_keys=True).encode()).hexdigest()
                    self.report.setdefault('resource_cpu_retries',[]).append({'attempt':attempt,'transaction_id':transaction_id,
                        'exception':'tx_cpu_usage_exceeded','receipt':None,'unchanged_CPU_cap_us':150000,
                        'state_before_after_sha256':digest,'state_unchanged':True,'canonical_absence':scanned,
                        'immutable_packet_sha256':packet_hash,'same_native_id_retry':True,'bootstrap':self.bootstrapping,
                        'error_log':str(file),'error_sha256':hashlib.sha256(raw.encode()).hexdigest(),
                        'scope':'Private single-node namespace only; no live-chain retry or capacity claim'})
                    print('RETRY: same immutable native packet; explicit CPU rejection, canonical absence and unchanged state; caps unchanged',flush=True)
        def start_node(self,phase,extra=()):
            super().start_node(phase,extra)
        def setup(self):
            ordinary=['civicgovlead','civicgovauth','civicgovcode','keyusersolar','keyuserlunar',
                      'gptkeyorigin','keyrelaymoon','keyusdtoken1','keybadtokenx','keysysmockup','keyramnotify']
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
            for n in ['eosio.token','keyusdtoken1']:
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
            self.action('create',{'issuer':'keyusdtoken1','maximum_supply':'1000000.0000 USD'},'keyusdtoken1',contract='keyusdtoken1')
            self.action('issue',{'to':'keyusdtoken1','quantity':'10000.0000 USD','memo':'private test token'},'keyusdtoken1',contract='keyusdtoken1')
            for n in ordinary:
                # Complete the large private RAM-market test funding before
                # govinit removes bootstrap eosio signing authority.
                amount='600000.0000 AIC'if n=='keyusersolar'else'100000.0000 AIC'
                self.tx(['transfer','eosio',n,amount,'test setup'])
            self.action('transfer',{'from':'keyusdtoken1','to':'keyusersolar','quantity':'1000.0000 USD','memo':'test setup'},'keyusdtoken1',contract='keyusdtoken1')
            self.tx(['set','contract','eosio',str(self.contracts/'eosio.bios')])
            supported=self.rpc('get_supported_protocol_features',{'exclude_disabled':True},namespace='producer')
            byid={f['feature_digest']:f for f in supported}
            activated={f['feature_digest']for f in self.rpc('get_activated_protocol_features',{'limit':1000})['activated_protocol_features']}
            def activate(digest):
                if digest in activated:return
                for dep in byid[digest]['dependencies']:activate(dep)
                self.action('activate',{'feature_digest':digest},'eosio');activated.add(digest)
            required={'PUREEOS_ROTATION_CONTEXT','PUREEOS_SEVEN_DAY_GOVERNANCE','GET_CODE_HASH','CRYPTO_PRIMITIVES',
                      'DISABLE_DEFERRED_TRXS_STAGE_1','DISABLE_DEFERRED_TRXS_STAGE_2','RAM_RESTRICTIONS'}
            found=set()
            for f in supported:
                labels={s['value']for s in f['specification']}
                if labels&required:activate(f['feature_digest']);found|=labels&required
            self.check(found==required,'Required native features activated; deferred execution remains disabled')
            self.check(self.governance()['review_seconds']==args.review_seconds,
                       'Governance protocol activation selects the same native review profile before sealing')
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
                self.mock_ram(self.table('rammarket',scope='eosio')[0])
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
            wrong_review=604800 if args.review_seconds==1800 else 1800
            self.action('govinit',{'developer':'civicgovlead','review_seconds':wrong_review,'protected_accounts':['eosio.key']},'eosio@owner',failure='review_seconds must match')
            self.check(not self.governance()['sealed'],'Native build rejects the other review profile before sealing')
            self.action('govinit',{'developer':'civicgovlead','review_seconds':args.review_seconds,'protected_accounts':['eosio.key']},'eosio@owner')
            self.check(self.governance()['sealed']and self.governance()['review_seconds']==args.review_seconds,'Native governance sealed after eosio.key deployment with selected review '+str(args.review_seconds)+' seconds')
            self.check('eosio.key'in self.governance()['protected_accounts'],'Wallet is explicitly protected by selected native governance review')
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
            return {n:self.ktable(n)for n in ['config','wallets','assets','nativefees','feebuckets','pricing','relayrates','relays']}|{'balances':[
                {'wallet':w['id'],'rows':self.ktable('balances',str(w['id']))}for w in self.ktable('wallets')]}
        def register_token(self,token='eosio.token',symbol='4,AIC',quantity='1.0000 AIC',minimum=4096,payer='keyusersolar',failure=None):
            actions=[{'account':'eosio','name':'buyram','authorization':[{'actor':payer,'permission':'active'}],
                      'data':{'payer':payer,'receiver':'eosio.key','quant':quantity}},
                     {'account':'eosio.key','name':'regtoken','authorization':[{'actor':payer,'permission':'active'}],
                      'data':{'payer':payer,'token_contract':token,'token_symbol':symbol,'min_bytes':minimum}}]
            return self.tx(['push','transaction',json.dumps({'actions':actions})],failure=failure)
        def asset_row(self,token,symbol):
            return next(r for r in self.ktable('assets')if r['token_contract']==token and r['token_symbol']==symbol)
        def test_whitelist_governance(self):
            import base64
            # A valid extra WASM custom section changes the proposal hash without
            # changing execution. It tests the existing review gate, not a shortcut
            # that activates a new token before seven actual days have elapsed.
            folder=args.artifacts/'test-only-wallet'if args.fixture else args.artifacts
            code=(folder/'eosio.key.wasm').read_bytes()+bytes((0,2,1,ord('w')))
            base=self.rpc('get_raw_abi',{'account_name':'eosio.key'});abi=base64.b64decode(base['abi'])
            manifest=json.loads((source/'token_allowlist.json').read_text())
            document=json.dumps(manifest,sort_keys=True,separators=(',',':')).encode()
            proposal={'target':'eosio.key','old_code_hash':base['code_hash'],'old_abi_hash':base['abi_hash'],
                      'new_code_hash':hashlib.sha256(code).hexdigest(),'new_abi_hash':hashlib.sha256(abi).hexdigest(),
                      'review_document_hash':hashlib.sha256(document).hexdigest()}
            def actions(actor,permission):
                return {'actions':[{'account':'eosio','name':name,'authorization':[{'actor':actor,'permission':permission}],
                                    'data':data}for name,data in [('refreshgov',{}),('devpropose',proposal)]]}
            def large_action(name,data,failure):
                # WASM hex exceeds Linux's per-argument limit. Read the native
                # transaction from a file, as an actual deployment client does.
                path=self.root/('governance-'+name+'.json')
                path.write_text(json.dumps({'actions':[{'account':'eosio','name':name,
                    'authorization':[{'actor':'civicgovlead','permission':'active'if name=='setcode'else'upgrade'}],'data':data}]}))
                return self.tx(['push','transaction',str(path)],failure=failure)
            before=self.governance()
            self.tx(['push','transaction',json.dumps(actions('keyusersolar','active'))],failure='missing authority')
            self.check(self.governance()==before,'Ordinary account cannot publish wallet whitelist-code proposal')
            large_action('setcode',{'account':'eosio.key','vmtype':0,'vmversion':0,'code':code.hex()},'reviewed native devexec capability')
            self.tx(['push','transaction',json.dumps(actions('civicgovlead','upgrade'))])
            state=self.governance()
            delta=(datetime.datetime.fromisoformat(state['execute_after'])-datetime.datetime.fromisoformat(state['proposed_at'])).total_seconds()
            self.check(state['pending']and state['target']=='eosio.key'and delta==args.review_seconds,
                       'Native developer proposal binds wallet hashes and exact selected review deadline')
            large_action('devexec',{'proposal_id':state['proposal_id'],'code':code.hex(),'abi':abi.hex()},'review period has not ended')
            self.action('devveto',{'voter':'eosio','proposal_id':state['proposal_id']},'eosio@veto')
            self.check(self.governance()['vetoes']==['eosio'],'Actual native frozen bootstrap electorate can veto the proposed wallet upgrade')
            self.action('devcancel',{'proposal_id':state['proposal_id']},'civicgovlead@upgrade')
            self.check(not self.governance()['pending']and self.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==base['code_hash'],
                       'Cancelled proposal and rejected early execution leave the approved wallet code unchanged')
            self.report['whitelist_governance']={'mechanism':'native refreshgov/devpropose/devveto/devexec',
                'review_seconds':args.review_seconds,'manifest':manifest,'manifest_sha256':hashlib.sha256(document).hexdigest(),
                'unauthorized_proposal_rejected':True,'direct_setcode_rejected':True,'early_execution_rejected':True,
                'native_veto_verified':True,'proposal_cancelled':True,'mature_execution_retested':False,
                'bootstrap_note':'AIC and USD are initialized in the approved test wallet before governance sealing; no early proposal execution'}
        def test_registration(self):
            before=self.state();native=self.token_balance('keyusersolar')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC','fee',failure='token RAM budget exhausted')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native,
                       'AIC also requires paid RAM registration before accepting fee credit')
            self.action('regtoken',{'payer':'keyusersolar','token_contract':'eosio.token','token_symbol':'4,AIC','min_bytes':4096},
                        'keyusersolar',contract='eosio.key',failure='buy RAM and register')
            before_ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])
            if args.fixture:
                self.action('open',{'owner':'eosio.key','symbol':'4,AIC','ram_payer':'keyusersolar'},'keyusersolar',contract='eosio.token')
            self.register_token()
            row=self.asset_row('eosio.token','4,AIC')
            after_ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])
            self.check(int(row['ram_purchased'])==after_ram-before_ram and int(row['ram_used'])==2048 and row['ram_sponsor']=='keyusersolar',
                       'Native payer buys real RAM; registration credits only actual purchased bytes to AIC')
            self.transfer('keyusersolar','eosio.key','100.0000 AIC','fee')
            before=self.state();native=self.token_balance('keyusersolar','keyusdtoken1')
            self.transfer('keyusersolar','eosio.key','1.0000 USD',self.memo(self.key()),token='keyusdtoken1',failure='token RAM registration required')
            self.check(self.state()==before and self.token_balance('keyusersolar','keyusdtoken1')==native,
                       'Unregistered other token deposit rolls back principal, fees and all rows')
            quota=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])
            before=self.state();native=self.token_balance('keyusersolar')
            self.register_token('keyusdtoken1','4,USD',minimum=1073741824,failure='below registration minimum')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native and
                       int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])==quota,
                       'Failed registration atomically refunds native RAM purchase and leaves no token admission')
            self.register_token('keyusdtoken1','5,USD',failure='reviewed whitelist')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native,
                       'Wrong token precision cannot obtain admission and refunds the paired RAM purchase')
            bad=[{'account':'eosio','name':'buyram','authorization':[{'actor':'keyusersolar','permission':'active'}],
                  'data':{'payer':'keyusersolar','receiver':'eosio.key','quant':'1.0000 AIC'}},
                 {'account':'eosio.key','name':'regtoken','authorization':[{'actor':'keyuserlunar','permission':'active'}],
                  'data':{'payer':'keyusersolar','token_contract':'keyusdtoken1','token_symbol':'4,USD','min_bytes':4096}}]
            self.tx(['push','transaction',json.dumps({'actions':bad})],failure='share the explicit RAM payer')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native,
                       'Another account cannot register against a different RAM payer authorization')
            # Small genuine purchase to exercise exhaustion before global caps.
            from decimal import Decimal
            market=self.table('rammarket',scope='eosio')[0]
            base=int(Decimal(market['base']['balance'].split()[0]))
            quote=int(Decimal(market['quote']['balance'].split()[0])*10000)
            target=6144;net=(target*quote+base-target-1)//(base-target)
            gross=(net*200+198)//199+2
            if args.fixture:
                self.action('open',{'owner':'eosio.key','symbol':'4,USD','ram_payer':'keyusersolar'},'keyusersolar',contract='keyusdtoken1')
            self.register_token('keyusdtoken1','4,USD',quantity=f'{gross//10000}.{gross%10000:04d} AIC')
            self.check(self.asset_row('keyusdtoken1','4,USD')['ram_sponsor']=='keyusersolar',
                       'Permissionless sponsor registers full issuer, precision and symbol without recipient opening')
        def test_sender_ram_fees(self):
            def credit():return int(next(r['amount']for r in self.ktable('nativefees')if r['owner']=='keyusersolar'))
            def key_balance(key):
                w=next(w for w in self.ktable('wallets')if self.pub_equal(w['key'],key['Public key']))
                return int(next(b['amount']for b in self.ktable('balances',str(w['id']))if int(b['asset_id'])==1))
            before=credit();used=int(self.asset_row('eosio.token','4,AIC')['ram_used'])
            self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.receiver))
            first=before-credit()
            self.check(key_balance(self.receiver)==10000 and int(self.asset_row('eosio.token','4,AIC')['ram_used'])==used+1536,
                       'AIC deposit sender pays additional new-recipient fee; receiving key gets full principal and no account prerequisite')
            before=credit();self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.receiver));repeat=before-credit()
            self.check(first>=repeat>0 and key_balance(self.receiver)==20000 and int(self.asset_row('eosio.token','4,AIC')['ram_used'])==used+1536,
                       'Existing AIC recipient allocates no repeated RAM; the base floor may mask a low market storage charge')
            recipient=self.key();before=key_balance(self.owner);native_credit=credit()
            self.execute('ksend',self.owner,self.request(self.owner,to=recipient['Public key'],quantity='1.0000 AIC'))
            key_first=before-key_balance(self.owner)-10000
            before=key_balance(self.owner)
            self.execute('ksend',self.owner,self.request(self.owner,to=recipient['Public key'],quantity='1.0000 AIC'))
            key_repeat=before-key_balance(self.owner)-10000
            self.check(key_first>=key_repeat>0 and key_balance(recipient)==20000 and credit()==native_credit,
                       'Key sender pays fees in addition to full principal; repeated rows reuse RAM, and the base floor remains applicable')
            self.report['sender_paid_RAM_fees']={'native_first_fee_atoms':first,'native_repeat_fee_atoms':repeat,
                                               'key_first_fee_atoms':key_first,'key_repeat_fee_atoms':key_repeat,'recipient_principal_atoms':20000}
        def mock_ram(self,market):
            self.action('setram',{'supply':market['supply'],'ram':market['base']['balance'],'aic':market['quote']['balance'],
                'ram_weight':float(market['base']['weight']),'aic_weight':float(market['quote']['weight'])},'keysysmockup',contract='keysysmockup')
        def ram_market(self):
            account='keysysmockup'if args.fixture else'eosio'
            return self.table('rammarket',code=account,scope=account)[0]
        def ram_quote(self,bytes):
            if not bytes:return 0
            from decimal import Decimal
            m=self.ram_market();r=int(m['base']['balance'].split()[0]);q=int(Decimal(m['quote']['balance'].split()[0])*10000)
            net=(q*bytes+r-bytes-1)//(r-bytes)
            return (net*200+198)//199
        def market_expected(self,units,bytes,rate=3000):
            # These cases run before the separate real PowerUp reservation test.
            work=(10*units+0);storage=self.ram_quote(bytes);base=max(100,work+storage)
            premium=(base*rate+9999)//10000 if rate else 0
            return {'execution':work,'storage':storage,'base':base,'premium':premium,'total':base+premium}
        def sdk_quote(self,operation,request):
            result=subprocess.run(['node',str(source/'tests/quote.mjs')],input=json.dumps({'endpoint':self.url,'operation':operation,'request':request}),
                                  text=True,capture_output=True,timeout=40)
            assert result.returncode==0,self.redact(result.stderr)
            return json.loads(result.stdout)
        def test_market_ram(self):
            if args.fixture:return self.test_fixture_ram()
            from decimal import Decimal
            recipient=self.key();req=self.request(self.owner,to=recipient['Public key'],quantity='0.0001 AIC',memo='')
            low=self.sdk_quote('ksend',req)
            self.check(low['ram_pricing']=='market'and low['ram_bytes']==1536 and int(low['storage'])==self.ram_quote(1536),
                       'SDK prices a new key with the same 1536-byte integer curve quote as the production contract profile')
            signed=self.signed('ksend',req|{'max_fee':f"{int(low['total'])//10000}.{int(low['total'])%10000:04d} AIC"},self.owner)
            quota=int(self.rpc('get_account',{'account_name':'keyusersolar'})['ram_quota'])
            self.action('buyram',{'payer':'keyusersolar','receiver':'keyusersolar','quant':'500000.0000 AIC'},'keyusersolar')
            purchased=int(self.rpc('get_account',{'account_name':'keyusersolar'})['ram_quota'])-quota
            high=self.sdk_quote('ksend',req)
            self.check(int(high['storage'])>int(low['storage'])and int(high['total'])>int(low['total']),
                       'A real native RAM purchase raises the market-priced new-storage fee without changing fee policy')
            before=self.state();self.execute('ksend',self.owner,signed['prepared']['request'],signed,failure='fee exceeds signed maximum')
            self.check(self.state()==before,'RAM price increase above the owner-signed total cap atomically rejects principal, fees and row allocation')
            expected=self.market_expected(4,1536);old=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            used=int(self.asset_row('eosio.token','4,AIC')['ram_used']);self.execute('ksend',self.owner,req)
            now=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            self.check(int(now['network'])-int(old['network'])==expected['base']and int(now['relay_earned'])-int(old['relay_earned'])==expected['premium']
                and int(self.asset_row('eosio.token','4,AIC')['ram_used'])==used+1536,
                'Production WASM charges exact market storage plus CPU/NET work and relay premium, while consuming the original sponsored budget')
            # The receiver has AIC but no USD row: only a 512-byte balance budget.
            req512=self.request(self.owner,to=self.receiver['Public key'],token_contract='keyusdtoken1',quantity='0.0001 USD',memo='')
            quote512=self.sdk_quote('ksend',req512);old=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            used=int(self.asset_row('keyusdtoken1','4,USD')['ram_used']);self.execute('ksend',self.owner,req512)
            now=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            self.check(quote512['ram_bytes']==512 and int(now['network'])-int(old['network'])==int(quote512['base'])
                and int(self.asset_row('keyusdtoken1','4,USD')['ram_used'])==used+512,
                'Existing-key first USD receipt pays exactly the 512-byte market quote from the sender and its own token allocation')
            reused=self.sdk_quote('ksend',req512)
            self.check(reused['ram_bytes']==0 and int(reused['storage'])==0,'Existing USD balance quote has zero RAM charge at the same high market price')
            # Select a genuine existing native account without an AIC row.
            native_to=next(n for n in ['eosio.saving','eosio.wrap','eosio.msig']if 'AIC'not in self.token_balance(n))
            native_req=self.request(self.owner,'selfout',to=native_to,quantity='0.0001 AIC',memo='')
            native_quote=self.sdk_quote('selfout',native_req)
            self.check(native_quote['ram_bytes']==512 and int(native_quote['premium'])==0,'Native recipient opening uses a 512-byte RAM quote without a self-withdrawal relay premium')
            old=next(r for r in self.ktable('feebuckets')if r['owner']=='eosio.key')if any(r['owner']=='eosio.key'for r in self.ktable('feebuckets'))else {'network':0}
            self.execute('selfout',self.owner,native_req)
            now=next(r for r in self.ktable('feebuckets')if r['owner']=='eosio.key')
            self.check(int(now['network'])-int(old['network'])==int(native_quote['base'])and self.token_balance(native_to)['AIC']=='0.0001 AIC',
                       'Native recipient gets full principal and sender pays the exact RAM-priced self-withdrawal fee')
            self.action('sellram',{'account':'keyusersolar','bytes':purchased},'keyusersolar')
            after=self.sdk_quote('ksend',self.request(self.owner,to=self.key()['Public key'],quantity='0.0001 AIC',memo=''))
            self.check(int(after['storage'])<int(high['storage'])and int(after['total'])<int(high['total']),
                       'A real native RAM sale lowers the new-storage quote and total automatically, with the unchanged base floor')
            self.report['RAM_market_pricing']={'scope':'Real native buy/sell; production WASM, no RAM-market fixture','low':low,'high':high,'after_sale':after,
                'balance_only':quote512,'existing_balance':reused,'native_recipient':native_quote,'purchased_then_sold_bytes':purchased,'signed_cap_rollback':True}
        def test_fixture_ram(self):
            original=self.ram_market();market={'supply':'10000000000.0000 RAMCORE','base':{'balance':'1000000 RAM','weight':0.5},'quote':{'balance':'100.0000 AIC','weight':0.5}}
            measured_units=[]
            def native_fee(key,cap='100000'):
                before=int(next(r['amount']for r in self.ktable('nativefees')if r['owner']=='keyusersolar'))
                memo=self.memo(key,cap);measured_units.append(2+(len(memo.encode())+63)//64)
                self.transfer('keyusersolar','eosio.key','0.0001 AIC',memo)
                return before-int(next(r['amount']for r in self.ktable('nativefees')if r['owner']=='keyusersolar'))
            self.mock_ram(market);recipient=self.key();before=self.state()
            self.transfer('keyusersolar','eosio.key','0.0001 AIC',self.memo(recipient,'100'),failure='fee exceeds signed maximum')
            self.check(before==self.state(),'TEST FIXTURE: high RAM quote above native deposit cap rolls back every wallet table')
            a=native_fee(recipient)
            recipient2=self.key();self.mock_ram(market|{'quote':{'balance':'50.0000 AIC','weight':0.5}});b=native_fee(recipient2)
            self.check(a>b>0,'TEST FIXTURE: lowering only the RAM AIC reserve lowers the actual new-key deposit fee')
            self.mock_ram(market);self.action('setmarket',{'cpu':10000,'net':0},'keysysmockup',contract='keysysmockup')
            busy=native_fee(self.key())
            self.check(busy-a==10*measured_units[-1]*10-10*measured_units[0],
                       'TEST FIXTURE: a 10x CPU/NET multiplier increases only execution; identical RAM allocation price is unchanged')
            self.action('setmarket',{'cpu':0,'net':0},'keysysmockup',contract='keysysmockup')
            bad=[market|{'base':{'balance':'1536 RAM','weight':0.5}},market|{'quote':{'balance':'0.0000 AIC','weight':0.5}},
                 market|{'quote':{'balance':'100.0000 USD','weight':0.5}},market|{'base':{'balance':'1000000 RAM','weight':0.6}},
                 market|{'base':{'balance':'1537 RAM','weight':0.5},'quote':{'balance':'461168601842738.7903 AIC','weight':0.5}}]
            for i,m in enumerate(bad):
                self.mock_ram(m);before=self.state()
                self.transfer('keyusersolar','eosio.key','0.0001 AIC',self.memo(self.key()),failure='overflow'if i==4 else'invalid RAM market')
                self.check(self.state()==before,'TEST FIXTURE: malformed/depleted/overflow RAM market '+str(i)+' rejects atomically')
            self.action('dropram',{},'keysysmockup',contract='keysysmockup');before=self.state()
            self.transfer('keyusersolar','eosio.key','0.0001 AIC',self.memo(self.key()),failure='RAM market missing')
            self.check(self.state()==before,'TEST FIXTURE: missing RAM market cannot silently fall back to fixed fees')
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',memo=''))
            self.check(True,'TEST FIXTURE: existing recipient owner exit requires no RAM market')
            self.mock_ram(original)
        def test_ram_budget(self):
            def row():return self.asset_row('keyusdtoken1','4,USD')
            while int(row()['ram_purchased'])-int(row()['ram_used'])>=1536:
                self.execute('ksend',self.owner,self.request(self.owner,token_contract='keyusdtoken1',quantity='0.0001 USD',to=self.key()['Public key']))
            new=self.key();before=self.state();native=self.token_balance('keyusersolar','keyusdtoken1')
            self.transfer('keyusersolar','eosio.key','0.0001 USD',self.memo(new),token='keyusdtoken1',failure='token RAM budget exhausted')
            self.check(self.state()==before and self.token_balance('keyusersolar','keyusdtoken1')==native,
                       'Per-token RAM exhaustion rejects new key deposits without using AIC token budget or debiting sender')
            used=int(row()['ram_used']);before_ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            self.transfer('keyusersolar','eosio.key','0.0001 USD',self.memo(self.owner),token='keyusdtoken1')
            self.check(int(row()['ram_used'])==used and int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==before_ram,
                       'Existing recipient rows accept deposits at exhausted allocation budget without repeated RAM charges')
            self.register_token('keyusdtoken1','4,USD',payer='keyuserlunar')
            self.transfer('keyusersolar','eosio.key','0.0001 USD',self.memo(new),token='keyusdtoken1')
            self.check(int(row()['ram_used'])==used+1536 and row()['ram_sponsor']=='keyusersolar',
                       'Another native sponsor can top up; new key consumes its own token budget while retaining first-sponsor attribution')
        def test_relay_rates(self):
            import base64
            (self.root/'candidate.abi.bin').write_bytes(base64.b64decode(self.rpc('get_raw_abi',{'account_name':'eosio.key'})['abi']))
            def setrate(rate, actor='keyrelaymoon', failure=None):
                return self.action('setrate',{'bp':'keyrelaymoon','fee_bps':rate},actor,contract='eosio.key',failure=failure)
            for rate in [999,3001]:setrate(rate,failure='between 10% and 30%')
            setrate(1000,actor='keyusersolar',failure='missing authority')
            evidence=[]
            for rate in [1000,1501,3000]:
                setrate(rate)
                before=next(b for b in self.ktable('feebuckets')if b['owner']=='keyrelaymoon')
                self.execute('ksend',self.owner,self.request(self.owner,quantity='0.0001 AIC',relay_fee_bps=rate))
                after=next(b for b in self.ktable('feebuckets')if b['owner']=='keyrelaymoon')
                base=int(after['network'])-int(before['network']);premium=int(after['relay_earned'])-int(before['relay_earned'])
                self.check(base>0 and premium==max(1,(base*rate+9999)//10000),'On-chain relay rate '+str(rate)+'bp charges exact ceil premium and preserves base bucket')
                evidence.append({'bps':rate,'base_atoms':base,'service_atoms':premium})
            setrate(1000)
            old=self.request(self.owner,quantity='0.0001 AIC',relay_fee_bps=1000)
            signed=self.signed('ksend',old,self.owner)
            setrate(2000);before=self.state()
            self.execute('ksend',self.owner,old,sig=signed,failure='relay rate changed')
            self.check(self.state()==before,'Rate changes after owner signing reject atomically instead of increasing the signed rate')
            self.execute('ksend',self.owner,self.request(self.owner,quantity='0.0001 AIC'))
            self.check(True,'Legacy envelopes retain their signed total cap and accept rates below the original30% ceiling')
            setrate(1000)
            for operation in ['withdraw','cancel']:
                patch={'relay_fee_bps':1000}
                if operation=='withdraw':patch['quantity']='0.0001 AIC'
                self.execute(operation,self.owner,self.request(self.owner,operation,**patch))
            self.check(True,'Configurable signed rates work for both relayed withdrawal and cancellation')
            setrate(3000)
            self.action('regrelay',{'bp':'keyrelaymoon','permission':'keyrelay','endpoint':''},'keyrelaymoon',contract='eosio.key')
            row=next(r for r in self.ktable('relays')if r['bp']=='keyrelaymoon')
            self.check(row['endpoint']=='' and not row['enabled'],'Empty-endpoint registration is inactive metadata, without an HTTP service')
            self.action('setrelay',{'bp':'keyrelaymoon','enabled':True},'keyrelaymoon',contract='eosio.key',failure='endpoint not configured')
            setrate(1000);before=self.state()
            self.execute('ksend',self.owner,self.request(self.owner,relay_fee_bps=1000),failure='relay disabled')
            self.check(self.state()==before,'Display-only relay cannot execute owner transfers or consume fees')
            self.action('regrelay',{'bp':'keyrelaymoon','permission':'keyrelay','endpoint':'https://relay.example.invalid'},'keyrelaymoon',contract='eosio.key')
            self.check(next(r['fee_bps']for r in self.ktable('relayrates')if r['bp']=='keyrelaymoon')==1000,'Configuring an endpoint preserves the operator-owned rate')
            setrate(3000)
            self.report['relay_rates']={'range_bps':[1000,3000],'rounding':'ceil; min one atom','charges':evidence,'stale_quote_atomic_rejection':True,'legacy30_preserved':True}
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
            self.test_registration()
            self.transfer('keyusersolar','eosio.key','1000.0000 AIC',self.memo(self.owner))
            self.transfer('keyusersolar','eosio.key','100.0000 USD',self.memo(self.owner),token='keyusdtoken1')
            self.check(len(self.ktable('wallets'))==1 and len(self.ktable('assets'))==2,'Native deposits create one canonical wallet and distinct multi-token reserves')
            self.test_sender_ram_fees()
            self.test_relay_rates()
            read=subprocess.run(['node',str(source/'tests/read.mjs')],input=json.dumps({'endpoint':self.url,'key':self.owner['Public key'],
                'token':'keyusdtoken1','symbol':'4,USD'}),capture_output=True,text=True,timeout=30)
            assert read.returncode==0,self.redact(read.stderr)
            self.check(json.loads(read.stdout)['balance']=='1000000','JS SDK reads actual key/asset secondary indices and exact four-decimal USD balance')
            self.test_market_ram()
            self.test_ram_budget()
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
            req=self.request(self.owner,'withdraw',token_contract='keyusdtoken1',quantity='1.2345 USD')
            row_count=self.ktable('config')[0]['balance_count'];before=self.state()
            self.execute('withdraw',self.owner,req,failure='recipient must open')
            self.check(self.state()==before and not self.token_balance('keyuserlunar','keyusdtoken1'),
                       'Unopened native USD recipient rejects before fees, principal or custody RAM change')
            self.action('open',{'owner':'keyuserlunar','symbol':'4,USD','ram_payer':'keyuserlunar'},'keyuserlunar',contract='keyusdtoken1')
            ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            self.execute('withdraw',self.owner,req)
            self.check(self.ktable('config')[0]['balance_count']==row_count and int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram,
                       'USD withdrawal reuses the recipient-funded balance row without extra custody RAM or row-budget charge')
            self.check(self.token_balance('keyuserlunar','keyusdtoken1')['USD']=='1.2345 USD','Approved standard USD withdrawal delivers the full principal')
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
            self.test_claim_from_income()
            self.action('refundfee',{'owner':'keyusersolar','to':'keyusersolar','quantity':'1.0000 AIC','max_fee':'1.0000 AIC'},'keyusersolar',contract='eosio.key')
            self.check(all(not a['busy']for a in self.ktable('assets')),'All outgoing token interactions finish without outstanding busy locks')
            if (args.artifacts/'test-only-token/keybadtokenx.wasm').exists():self.test_incompatible_token()
            self.test_storage_and_nonce()
            if args.fixture:self.test_fixture_states()
            else:self.test_real_market()
            self.test_large_nonce()
            self.test_overspend()
            self.check_reserves()
            self.test_whitelist_governance()
            self.report['final_state']=self.state()
        def test_claim_from_income(self):
            from decimal import Decimal
            def quantity(atoms):return f'{atoms//10000}.{atoms%10000:04d} AIC'
            def balance(account='keyrelaymoon'):
                value=self.token_balance(account).get('AIC','0.0000 AIC')
                return int(Decimal(value.split()[0])*10000)
            def bucket():return next(b for b in self.ktable('feebuckets')if b['owner']=='keyrelaymoon')
            def claim(amount,**patch):
                return {'bp':'keyrelaymoon','to':'keyrelaymoon','quantity':quantity(amount),'max_fee':'0.0000 AIC'}|patch
            def rejected(data,message,actor='keyrelaymoon'):
                before=self.state();native=balance()
                self.action('claim',data,actor,contract='eosio.key',failure=message)
                self.check(self.state()==before and balance()==native,'Fee-free claim rejection is atomic: '+message)
            def receipt(result):
                def walk(trace):
                    yield trace
                    for child in trace.get('inline_traces',[]):yield from walk(child)
                return next(t['act']['data']for root in result['processed']['action_traces']for t in walk(root)
                            if t['act']['account']=='eosio.key'and t['act']['name']=='receipt'and t['act']['data']['kind']=='claim')
            self.check(not any(c['owner']=='keyrelaymoon'for c in self.ktable('nativefees')),
                       'Relay has earned income without ever opening or funding native fee credit')
            earned=int(bucket()['relay_earned']);assert earned>2
            rejected(claim(0),'invalid claim asset')
            rejected(claim(1,quantity='0.0001 USD'),'invalid claim asset')
            rejected(claim(1,max_fee='0.0000 USD'),'invalid AIC fee cap')
            rejected(claim(earned+1),'insufficient confirmed relay earnings')
            rejected(claim(1),'authority',actor='keyusersolar')
            rejected(claim(1),'authority',actor='keyrelaymoon@keyrelay')
            before=self.state();old=bucket();native=balance()
            ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            tx=self.action('claim',claim(1),'keyrelaymoon',contract='eosio.key')
            event=receipt(tx);now=bucket()
            self.check(balance()-native==1 and old['relay_earned']-now['relay_earned']==1 and
                       now['network']==old['network'] and self.ktable('nativefees')==before['nativefees'],
                       'One-atom relay income is delivered in full with max_fee zero, no prepaid row and no application fee')
            self.check(event['quantity']=='0.0001 AIC'and event['fee']=='0.0000 AIC'and
                       int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram,
                       'Fee-free claim receipt matches full payout and custody RAM does not grow')
            self.check_reserves()
            # Existing prepaid credits stay untouched as well.
            self.transfer('keyrelaymoon','eosio.key','0.0500 AIC','fee')
            rejected(claim(1,to='eosio.key'),'invalid withdrawal target')
            recipient='keyclaimdest';key=self.key()['Public key']
            auth={'threshold':1,'keys':[{'key':key,'weight':1}],'accounts':[],'waits':[]}
            def act(contract,name,data):
                return {'account':contract,'name':name,'authorization':[{'actor':'keyrelaymoon','permission':'active'}],'data':data}
            self.tx(['push','transaction',json.dumps({'actions':[
                act('eosio','newaccount',{'creator':'keyrelaymoon','name':recipient,'owner':auth,'active':auth}),
                act('eosio','buyram',{'payer':'keyrelaymoon','receiver':recipient,'quant':'1.0000 AIC'})]})])
            rejected(claim(1,to=recipient),'claim recipient must open AIC balance')
            opened=act('eosio.token','open',{'owner':recipient,'symbol':'4,AIC','ram_payer':'keyrelaymoon'})
            before=self.state();ram_bp=int(self.rpc('get_account',{'account_name':'keyrelaymoon'})['ram_usage'])
            self.tx(['push','transaction',json.dumps({'actions':[opened,
                act('eosio.key','claim',claim(int(bucket()['relay_earned'])+1,to=recipient))]})],failure='insufficient confirmed relay earnings')
            self.check(self.state()==before and not self.token_balance(recipient)and
                       int(self.rpc('get_account',{'account_name':'keyrelaymoon'})['ram_usage'])==ram_bp,
                       'Failed atomic claimant-paid open plus claim refunds the new recipient RAM and leaves income unchanged')
            old=bucket();ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            used=int(self.asset_row('eosio.token','4,AIC')['ram_used'])
            tx=self.tx(['push','transaction',json.dumps({'actions':[opened,act('eosio.key','claim',claim(1,to=recipient))]})])
            self.check(balance(recipient)==1 and int(bucket()['relay_earned'])==int(old['relay_earned'])-1 and
                       int(self.rpc('get_account',{'account_name':'keyrelaymoon'})['ram_usage'])>ram_bp and
                       int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram and
                       int(self.asset_row('eosio.token','4,AIC')['ram_used'])==used,
                       'Claimant authorizes and pays recipient token-row RAM; fee-free claim never allocates custody RAM or token budget')
            if args.fixture:
                self.action('setmarket',{'cpu':10000,'net':0},'keysysmockup',contract='keysysmockup')
                self.wait_lib(self.rpc('get_info')['head_block_num']+3)
            before=self.state();old=bucket();native=balance();amount=int(old['relay_earned'])
            tx=self.action('claim',claim(amount),'keyrelaymoon',contract='eosio.key')
            event=receipt(tx);now=bucket()
            self.check(now['relay_earned']==0 and balance()-native==amount and
                       now['network']==old['network'] and self.ktable('nativefees')==before['nativefees'],
                       'Revoked relay receives all remaining income with no fee or prepaid debit, even at maximum fixture market occupancy')
            self.check(event['quantity']==quantity(amount)and event['fee']=='0.0000 AIC',
                       'Final claim receipt records full income payout and zero fee')
            rejected(claim(1),'insufficient confirmed relay earnings')
            self.check_reserves()
            if args.fixture:self.action('setmarket',{'cpu':0,'net':0},'keysysmockup',contract='keysysmockup')
            self.report['fee_free_claim']={'quantity_semantics':'full earned income paid','application_fee_atoms':0,
                'no_native_fee_entry_first_claim':True,'existing_fee_credit_unchanged':True,
                'smallest_atom_claim_verified':True,'final_claim_atoms':amount,
                'claimant_paid_open_verified':True,'open_and_failed_claim_RAM_rollback':True,
                'custody_RAM_growth':0,'revoked_relay_claim_verified':True,
                'permission_and_overclaim_rejections':True,'max_fee_zero_supported':True}
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
            before=self.state();native=self.token_balance('keyusersolar');quota=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])
            self.register_token('keybadtokenx','4,XTK',failure='reviewed whitelist')
            self.check(self.state()==before and self.token_balance('keyusersolar')==native and int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_quota'])==quota,
                       'Non-whitelisted issuer registration atomically refunds its RAM purchase')
            before=self.state();native=self.token_balance('keyusersolar','keybadtokenx')
            self.transfer('keyusersolar','eosio.key','1.0000 XTK',self.memo(self.owner),token='keybadtokenx',failure='reviewed whitelist')
            self.check(self.state()==before and self.token_balance('keyusersolar','keybadtokenx')==native,
                       'Non-whitelisted deposits cannot allocate custody rows or consume fees')
            self.tx(['set','contract','keyusdtoken1',str(args.artifacts/'test-only-token'),'keybadtokenx.wasm','keybadtokenx.abi'])
            self.action('setmode',{'value':6},'keyusdtoken1',contract='keyusdtoken1')
            before=self.state();ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keyusdtoken1',quantity='1.0000 USD'),failure='approved whitelist hash')
            self.check(self.state()==before and int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram,
                       'Whitelisted USD upgraded to RAM attack is rejected before custody authority; fees, principal and RAM stay unchanged')
            self.transfer('keyusersolar','eosio.key','0.0001 USD',self.memo(self.owner),token='keyusdtoken1',failure='approved whitelist hash')
            self.check(self.state()==before,'Changed issuer code also rejects deposits atomically')
            self.tx(['set','contract','keyusdtoken1',str(self.contracts/'eosio.token')])
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',token_contract='keyusdtoken1',quantity='0.0001 USD'))
            self.check(True,'Restoring exactly the approved USD code restores owner withdrawal')
            self.action('create',{'issuer':'keyusdtoken1','maximum_supply':'1000.0000 AIC'},'keyusdtoken1',contract='keyusdtoken1')
            self.action('issue',{'to':'keyusdtoken1','quantity':'10.0000 AIC','memo':'same implementation, unlisted symbol'},'keyusdtoken1',contract='keyusdtoken1')
            self.transfer('keyusdtoken1','keyusersolar','10.0000 AIC','test funding',token='keyusdtoken1')
            before=self.state();native=self.token_balance('keyusersolar','keyusdtoken1')
            self.register_token('keyusdtoken1','4,AIC',failure='reviewed whitelist')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC','fee',token='keyusdtoken1',failure='reviewed whitelist')
            self.transfer('keyusersolar','eosio.key','1.0000 AIC',self.memo(self.owner),token='keyusdtoken1',failure='reviewed whitelist')
            self.check(self.state()==before and self.token_balance('keyusersolar','keyusdtoken1')==native,
                       'Approval of USD never approves another symbol on the same standard contract; fake AIC cannot fund fees')
            self.tx(['set','contract','keyramnotify',str(args.artifacts/'test-only-recipient'),'keyramattack.wasm','keyramattack.abi'])
            before=self.state();ram=int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage']);native=self.token_balance('keyramnotify')
            self.execute('selfout',self.owner,self.request(self.owner,'selfout',to='keyramnotify',quantity='0.0001 AIC'),failure='notify context')
            self.check(self.state()==before and int(self.rpc('get_account',{'account_name':'eosio.key'})['ram_usage'])==ram and self.token_balance('keyramnotify')==native,
                       'Native RAM_RESTRICTIONS rejects recipient notification billing; withdrawal fully rolls back')
            self.report['token_code_admission']={'native':'eosio.token::4,AIC','third_party':'keyusdtoken1::4,USD',
                'reviewed_hash':self.ktable('config')[0]['token_hash'],'unlisted_identity_rejected':True,
                'changed_code_withdrawal_rejected':True,'extra_RAM_bytes':0,'restored_code_exit_verified':True,
                'recipient_notification_RAM_rejected':True,'third_party_requires_preopened_native_recipient':True}
            self.report['public_test_deployment_qualified']=True
            self.report['release_qualified']=False
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
            self.action('setrate',{'bp':'keyrelaymoon','fee_bps':1000},'keyrelaymoon',contract='eosio.key')
            old=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            prepared=json.loads(self.run(['key','prepare',self.owner['Public key'],self.receiver['Public key'],'0.0001 AIC','--bp','keyrelaymoon','--max-fee','1.0000 AIC']))
            signed=json.loads(self.run(['key','sign',json.dumps(prepared)]))
            self.tx(['key','push',json.dumps(signed)])
            now=next(r for r in self.ktable('feebuckets')if r['owner']=='keyrelaymoon')
            base=int(now['network'])-int(old['network']);premium=int(now['relay_earned'])-int(old['relay_earned'])
            self.check('relay_fee_bps'not in prepared['request'] and premium==max(1,(base*1000+9999)//10000),'Unchanged native cleos completes legacy-cap transfers at a cheaper10% operator rate')
            self.action('setrate',{'bp':'keyrelaymoon','fee_bps':3000},'keyrelaymoon',contract='eosio.key')
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
