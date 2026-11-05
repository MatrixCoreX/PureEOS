from pathlib import Path
import ast,datetime,hashlib,json,os,subprocess,sys,time,traceback,decimal
os.umask(0o077)
R=Path(os.environ['PUREEOS_GENESIS_ROOT']);V=Path('/opt/pureeos-dev-account-genesis-20261105-r1/runtime-3');PRIVATE=os.environ.get('PUREEOS_PRIVATE_GENESIS')=='1'
sys.path.insert(0,str(R/'harness'))
from pureeos_single_choice_local_test import LocalTest
class TestChain(LocalTest):
    local_signers=('eosio','gptgenesisbp')
    def __init__(self):
        super().__init__(R/'tools-runtime',V/'contracts',resource_split=True)
        generated=self.root;generated.rename(R/"founder");self.root=R/"founder"
        if not PRIVATE:self.http_port=8888;self.p2p_port=9876
        self.url='http://127.0.0.1:'+str(self.http_port)
        self.cleos=R/'native-client/cleos'
        self.hooked=False;self.bootstrapping=True
    def tx(self,args,*,failure=None):
        # LocalTest.tx already passes an explicit, distinct -x lifetime.
        for attempt in range(1,6):
            try:return super().tx(args,failure=failure)
            except RuntimeError as error:
                if failure is not None or not getattr(self,'bootstrapping',False) or 'tx_cpu_usage_exceeded' not in str(error) or attempt==5:raise
                self.report.setdefault('atomic_cpu_retries',[]).append({'attempt':attempt,'operation':args[:3]})
                self.wait_lib(self.rpc('get_info')['head_block_num']+2,timeout=180)
    def run(self,args,**kwargs):
        args=[a.replace(' SYS',' AIC').replace('4,SYS','4,AIC')if isinstance(a,str)else a for a in args]
        return super().run(args,**kwargs)
    def start_node(self,phase,extra=()):
        if phase=='genesis':
            p=self.root/'genesis.json';g=json.loads(p.read_text());g['initial_configuration']=json.loads((V/'config/test-low-cost/chain-parameters.json').read_text());p.write_text(json.dumps(g))
            prepared=json.loads((R/'private/bp-keys.json').read_text())['gptgenesisbp'];self.production=prepared['production'];self.peer=prepared['peer'];self.secrets.extend([self.production['Private key'],self.peer['Private key']])
            p=self.root/'config/config.ini';lines=p.read_text().splitlines()
            preset={l.split('=',1)[0].strip():l.split('=',1)[1].strip()for l in (V/'config/test-low-cost/config.ini').read_text().splitlines()if '='in l and not l.lstrip().startswith('#')and not l.startswith('plugin')and not l.startswith('experimental-parallel-code-hash')}
            preset['http-server-address']='127.0.0.1:'+str(self.http_port);preset['p2p-listen-endpoint']='127.0.0.1:'+str(self.p2p_port)
            for l in lines:
                if '='in l:preset.pop(l.split('=',1)[0].strip(),None)
            # Override existing numeric values as well as adding absent knobs.
            desired={'chain-state-db-size-mb':'4096','chain-state-db-guard-size-mb':'128'}
            lines=[l.split('=',1)[0]+'= '+desired[l.split('=',1)[0].strip()]if '='in l and l.split('=',1)[0].strip()in desired else l for l in lines]
            lines += [k+' = '+v for k,v in preset.items()]
            lines += ['experimental-parallel-code-hash = '+hashlib.sha256(p.read_bytes()).hexdigest()for p in self.contracts.rglob('*.wasm')]
            lines += ['peer-private-key = '+json.dumps([self.peer['Public key'],self.peer['Private key']]),'p2p-bp-gossip-endpoint = gptgenesisbp,127.0.0.1:'+str(self.p2p_port)+',127.0.0.1']
            lines += ['signature-provider = '+self.production['Public key']+'=KEY:'+self.production['Private key']]
            p.write_text('\n'.join(lines)+'\n');p.chmod(0o600)
            (R/'private/bootstrap-keys.json').write_text(json.dumps({'accounts':self.accounts,'bls':self.bls,'peer':self.peer,'production':self.production},indent=2))
        return super().start_node(phase,extra)
    def action(self, name, data, actor, *, contract='eosio', failure=None):
            if contract=='eosio' and failure is None:validate_self_resource(name,data)
            if name == 'create' and contract == 'eosio.token':
                data = {**data, 'maximum_supply': '2100000000.0000 AIC'}
            if name == 'issue' and contract == 'eosio.token':
                data = {**data, 'quantity': '100000000.0000 AIC', 'memo': 'PureEOS explorer devnet initial issue'}
            if name == 'govinit' and not getattr(self,'seven_day_activated',False):
                feature=next(x for x in self.rpc('get_supported_protocol_features',{'exclude_disabled':True},namespace='producer') if any(y.get('value')=='PUREEOS_SEVEN_DAY_GOVERNANCE' for y in x['specification']))
                super().action('activate',{'feature_digest':feature['feature_digest']},'eosio')
                self.seven_day_activated=True
                feature=next(x for x in self.rpc('get_supported_protocol_features',{'exclude_disabled':True},namespace='producer') if any(y.get('value')=='PUREEOS_SYSTEM_ACCOUNT_GOVERNANCE' for y in x['specification']))
                super().action('activate',{'feature_digest':feature['feature_digest']},'eosio')
            if name == 'govinit' and failure is None and (not self.hooked):
                self.hooked = True
                self.tx(['set', 'contract', 'eosio.fees', str(self.contracts / 'eosio.fees')])
                for action, value in [('bondbp', {'producer':'gptgenesisbp'}), ('regproducer', {'producer': 'gptgenesisbp', 'producer_key': self.production['Public key'], 'url': '', 'location': 0}), ('regfinkey', {'finalizer_name': 'gptgenesisbp', 'finalizer_key': self.bls['gptgenesisbp']['Public key'], 'proof_of_possession': self.bls['gptgenesisbp']['Proof of Possession']}), ('regpeerkey', {'proposer_finalizer_name': 'gptgenesisbp', 'key': self.peer['Public key']})]:
                    super().action(action, value, 'gptgenesisbp')
                super().action('delegatebw', {'from': 'civicuserone', 'receiver': 'civicuserone', 'stake_net_quantity': '10.0000 AIC', 'stake_cpu_quantity': '100.0000 AIC', 'transfer': False}, 'civicuserone')
                super().action('voteproducer', {'voter': 'civicuserone', 'proxy': '', 'producers': ['gptgenesisbp']}, 'civicuserone')
                super().action('deposit', {'owner': 'civicuserone', 'amount': '100.0000 AIC'}, 'civicuserone')
                super().action('buyrex', {'from': 'civicuserone', 'amount': '100.0000 AIC'}, 'civicuserone')
            result = super().action(name, data, actor, contract=contract, failure=failure)
            if name == 'init' and contract == 'eosio':
                super().action('setramrate', {'bytes_per_block': 1024}, 'eosio')
            if name == 'activate' and (not getattr(self, 'deferred_activated', False)):
                self.deferred_activated = True
                activated = {f['feature_digest'] for f in self.rpc('get_activated_protocol_features', {'limit': 1000})['activated_protocol_features']}
                supported = self.rpc('get_supported_protocol_features', {'exclude_disabled': True}, namespace='producer')
                for item in supported:
                    labels = {x.get('value') for x in item['specification']}
                    if labels.intersection({'DISABLE_DEFERRED_TRXS_STAGE_1', 'DISABLE_DEFERRED_TRXS_STAGE_2'}) and item['feature_digest'] not in activated:
                        super().action('activate', {'feature_digest': item['feature_digest']}, 'eosio')
            return result

def validate_self_resource(name,data):
    if name=='delegatebw':
        assert data['from']==data['receiver'],'positive test stake must be self-owned'
    if name=='powerup':
        assert data['payer']==data['receiver'],'positive test PowerUp must be self-paid'

def self_transact(self,label,actions):
    for a in actions:
        if a['account']=='eosio':validate_self_resource(a['name'],a['data'])
    info=self.rpc('get_info');bid=bytes.fromhex(info['last_irreversible_block_id'])
    tx={'expiration':(datetime.datetime.now(datetime.timezone.utc)+datetime.timedelta(seconds=90)).strftime('%Y-%m-%dT%H:%M:%S'),'ref_block_num':info['last_irreversible_block_num']&65535,'ref_block_prefix':int.from_bytes(bid[8:12],'little'),'max_net_usage_words':0,'max_cpu_usage_ms':0,'delay_sec':0,'context_free_actions':[],'actions':actions,'transaction_extensions':[]}
    return self.tx(['push','transaction',json.dumps(tx)])

def resource_action(name,actor,data,contract='eosio'):
    return {'account':contract,'name':name,'authorization':[{'actor':actor,'permission':'active'}],'data':data}

def self_stake(self,account,net,cpu):
    # The treasury's first funding action supplies the bootstrap transaction's
    # execution resources; the account signs and owns/pays its own stake.
    marker=resource_action('transfer','eosio',{'from':'eosio','to':account,'quantity':'0.0001 AIC','memo':'self-owned bootstrap resources'},'eosio.token')
    stake=resource_action('delegatebw',account,{'from':account,'receiver':account,'stake_net_quantity':net,'stake_cpu_quantity':cpu,'transfer':False})
    return self.self_transact('self-stake-'+account,[marker,stake])

def setup_twelve_character_accounts(self, register_candidates=True):
    self.accounts = {n: self.key() for n in ('eosio', 'civicgovlead', 'civicgovauth', 'civicgovcode', 'civicuserone', 'civicusertwo', 'civicrenters', 'gptgenesisbp', 'deepseekcore', 'eosio.dev')}
    self.bls = {n: self.key(True) for n in ('eosio', 'gptgenesisbp', 'deepseekcore')}
    prepared=json.loads((R/'private/bp-keys.json').read_text())
    for name in ['gptgenesisbp','deepseekcore']:
        self.accounts[name]=prepared[name]['account'];self.bls[name]=prepared[name]['bls'];self.secrets.extend([self.accounts[name]['Private key'],self.bls[name]['Private key']])
    (self.root / 'config').mkdir()
    (self.root / 'wallet').mkdir()
    genesis = {'initial_timestamp': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000'), 'initial_key': self.accounts['eosio']['Public key'], 'initial_savanna': {'version': 2, 'finalizer_key': self.bls['eosio']['Public key'], 'proof_of_possession': self.bls['eosio']['Proof of Possession']}}
    (self.root / 'genesis.json').write_text(json.dumps(genesis, indent=2))
    (self.root / 'config/config.ini').write_text('\n'.join(['plugin = eosio::producer_plugin', 'plugin = eosio::net_api_plugin', 'plugin = eosio::chain_api_plugin', 'plugin = eosio::producer_api_plugin', 'producer-name = eosio', 'enable-stale-production = true', 'chain-state-db-size-mb = 128', 'chain-state-db-guard-size-mb = 4', 'http-server-address = 127.0.0.1:' + str(self.http_port), 'p2p-listen-endpoint = 127.0.0.1:' + str(self.p2p_port), 'max-body-size = 8388608', 'verbose-http-errors = true', 'signature-provider = ' + self.accounts['eosio']['Public key'] + '=KEY:' + self.accounts['eosio']['Private key'], 'signature-provider = ' + self.bls['eosio']['Public key'] + '=KEY:' + self.bls['eosio']['Private key'], '']))
    with (self.root / 'config/config.ini').open('a') as output:
        for name in self.local_signers:
            if name == 'eosio':
                continue
            output.write('producer-name = ' + name + '\n')
            for key in (self.accounts[name], self.bls[name]):
                output.write('signature-provider = ' + key['Public key'] + '=KEY:' + key['Private key'] + '\n')
    self.wallet = self.spawn([str(self.build / 'programs/keosd/keosd'), '--data-dir', str(self.root / 'wallet'), '--config-dir', str(self.root / 'wallet'), '--http-server-address', ''], 'wallet.log')
    for _ in range(100):
        if (self.root / 'wallet/keosd.sock').exists():
            break
        if self.wallet.poll() is not None:
            raise RuntimeError('keosd exited during startup')
        time.sleep(0.1)
    self.run(['wallet', 'create', '-n', 'localtest', '--file', str(self.root / 'wallet/password.txt')])
    for key in self.accounts.values():
        self.run(['wallet', 'import', '-n', 'localtest'], input_text=key['Private key'] + '\n')
    self.start_node('genesis', ['--genesis-json', str(self.root / 'genesis.json')])
    info = self.wait_lib(17)
    self.report['chain_id'] = info['chain_id']
    self.check(self.rpc('get_block', {'block_num_or_id': 2})['schedule_version'] & 1 << 31, 'Native Savanna genesis, head and LIB advancing')
    supported = self.rpc('get_supported_protocol_features', {'exclude_disabled': True}, namespace='producer')
    names = {s['value'] for feature in supported for s in feature['specification'] if s['name'] == 'builtin_feature_codename'}
    enabled = {spec['builtin_feature_codename'] for path in (self.root / 'config/protocol_features').glob('*.json') if (spec := json.loads(path.read_text()))['subjective_restrictions']['enabled']}
    self.check(names == enabled and len(names) == len(supported), 'Protocol-feature API lists every enabled feature exactly once')
    seen = set()
    for feature in supported:
        if not set(feature['dependencies']).issubset(seen):
            raise AssertionError('Protocol-feature API dependencies are not in topological order')
        seen.add(feature['feature_digest'])
    self.report['protocol_feature_names'] = sorted(names)
    system = ('eosio.token', 'eosio.stake', 'eosio.rex', 'eosio.ram', 'eosio.ramfee', 'eosio.bpay', 'eosio.vpay', 'eosio.saving', 'eosio.fees', 'eosio.msig', 'eosio.wrap')
    if self.resource_split:
        system += ('eosio.reserv',)
    ordinary = ('eosio.dev', 'civicgovauth', 'civicgovcode', 'civicgovlead', 'civicuserone', 'civicusertwo', 'civicrenters', 'gptgenesisbp', 'deepseekcore')
    for account in (*system, *ordinary):
        owner = self.accounts[account if account in ordinary else 'eosio']['Public key']
        active = self.accounts['civicgovauth']['Public key'] if account == 'civicgovlead' else owner
        self.tx(['create', 'account', 'eosio', account, owner, active])
    self.tx(['set', 'account', 'permission', 'civicgovlead', 'upgrade', self.accounts['civicgovcode']['Public key'], 'active'])
    self.tx(['set', 'contract', 'eosio.token', str(self.contracts / 'eosio.token')])
    self.action('create', {'issuer': 'eosio', 'maximum_supply': '1000000000.0000 SYS'}, 'eosio.token', contract='eosio.token')
    self.action('issue', {'to': 'eosio', 'quantity': '1000000000.0000 SYS', 'memo': 'isolated test only'}, 'eosio', contract='eosio.token')
    for account in ordinary:
        self.tx(['transfer', 'eosio', account, '1000000.0000 SYS' if account=='gptgenesisbp' else '100000.0000 SYS', 'new-chain initial allocation; no extra issuance'])
    print('Bootstrap accounts and token funding complete; deploying system contracts', flush=True)
    if self.rotation_context:
        feature = next((f for f in supported if any((s.get('name') == 'builtin_feature_codename' and s.get('value') == 'PUREEOS_ROTATION_CONTEXT' for s in f['specification']))))
        self.tx(['set', 'contract', 'eosio', str(self.contracts / 'eosio.bios')])
        self.action('activate', {'feature_digest': feature['feature_digest']}, 'eosio')
        self.report['rotation_context_feature'] = feature['feature_digest']
    feature_map={f['feature_digest']:f for f in supported}
    active={f['feature_digest']for f in self.rpc('get_activated_protocol_features',{'limit':1000})['activated_protocol_features']}
    def activate_required(digest):
        if digest in active:return
        for dependency in feature_map[digest]['dependencies']:activate_required(dependency)
        self.action('activate',{'feature_digest':digest},'eosio');active.add(digest)
    required={'GET_CODE_HASH','CRYPTO_PRIMITIVES','RAM_RESTRICTIONS'}
    for feature in supported:
        if {x['value']for x in feature['specification']}&required:activate_required(feature['feature_digest'])
    self.tx(['set', 'contract', 'eosio.bpay', str(self.contracts / 'eosio.bpay')])
    self.tx(['set', 'contract', 'eosio.rex', str(self.contracts / 'eosio.system/.rex'), 'rex.results.wasm', 'rex.results.abi'])
    if self.resource_split:
        self.tx(['set', 'contract', 'eosio.reserv', str(self.contracts / 'eosio.system/.powerup'), 'powup.results.wasm', 'powup.results.abi'])
    self.deploy_system()
    self.action('init', {'version': 0, 'core': '4,SYS'}, 'eosio')
    if self.resource_split:
        self.action('buyram', {'payer': 'eosio', 'receiver': 'eosio.reserv', 'quant': '100.0000 SYS'}, 'eosio')
    for account in ordinary:
        self.action('buyram', {'payer': 'eosio', 'receiver': account, 'quant': '100.0000 SYS'}, 'eosio')
        self.self_stake(account,'10.0000 SYS','100.0000 SYS')
    for perm,parent,member,memberperm in [('upgrade','active','civicgovauth','active'),('active','owner','civicgovlead','active'),('owner','','civicgovlead','owner')]:
        auth={'threshold':1,'keys':[],'accounts':[{'permission':{'actor':member,'permission':memberperm},'weight':1}],'waits':[]}
        self.tx(['set','account','permission','eosio.dev',perm,json.dumps(auth),parent,'-p','eosio.dev@owner'])
    if self.resource_split:
        self.action('govinit', {'developer': 'eosio.dev', 'review_seconds': 1800, 'protected_accounts': []}, 'eosio@owner', failure='configure PowerUp before governance sealing')
        self.check(not self.governance()['sealed'], 'Missing PowerUp configuration rolls back governance sealing')
        resource = {'current_weight_ratio': None, 'target_weight_ratio': None, 'assumed_stake_weight': None, 'target_timestamp': None, 'exponent': 2.0, 'decay_secs': 86400, 'min_price': '1.0000 SYS', 'max_price': '100.0000 SYS'}
        self.action('cfgpowerup', {'args': {'net': resource, 'cpu': resource, 'powerup_days': 30, 'min_powerup_fee': '0.0001 SYS'}}, 'eosio')
        self.check_resource_split('PowerUp initialized before sealing')
    self.action('govinit', {'developer': 'eosio.dev', 'review_seconds': 1800, 'protected_accounts': []}, 'eosio@owner')
    state = self.governance()
    self.check(state['sealed'] and state['review_seconds'] == 1800, 'Real contract deployment and 30-minute TEST governance sealing')
    for node in ('gptgenesisbp', 'deepseekcore') if register_candidates else ():
        if not any(x['owner']==node for x in self.table('bpbonds')):self.action('bondbp',{'producer':node},node)
        self.action('regproducer', {'producer': node, 'producer_key': self.accounts[node]['Public key'], 'url': '', 'location': 0}, node)
        self.action('regfinkey', {'finalizer_name': node, 'finalizer_key': self.bls[node]['Public key'], 'proof_of_possession': self.bls[node]['Proof of Possession']}, node)
TestChain.setup=setup_twelve_character_accounts
TestChain.self_transact=self_transact;TestChain.self_stake=self_stake


import pwd,shlex
class ServiceHandle:
    def __init__(self,unit):self.unit=unit
    def poll(self):
        return None if subprocess.run(['systemctl','is-active','--quiet',self.unit]).returncode==0 else 0
    @property
    def returncode(self):return self.poll()
    def terminate(self):subprocess.run(['systemctl','stop',self.unit],check=True,timeout=120)
    def kill(self):subprocess.run(['systemctl','kill',self.unit],check=True)
    def wait(self,timeout=None):return self.poll()
def managed_spawn(self,args,logname):
    executable=str(args[0]).rsplit('/',1)[-1]
    if executable not in ['nodeos','keosd']:return LocalTest.spawn(self,args,logname)
    account=pwd.getpwnam('pureeostest');os.chown(self.root,account.pw_uid,account.pw_gid);self.root.chmod(0o750)
    for p in self.root.rglob('*'):
        os.chown(p,account.pw_uid,account.pw_gid)
    if PRIVATE:return LocalTest.spawn(self,args,logname)
    unit='pureeos-devnet-founder-20261105.service'if executable=='nodeos'else'pureeos-devnet-wallet-20261105.service'
    path=Path('/etc/systemd/system')/unit
    text='[Unit]\nDescription=PureEOS TEST45 '+executable+'\nAfter=network-online.target\nWants=network-online.target\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory='+str(self.root)+'\nExecStart='+shlex.join(list(map(str,args)))+'\nRestart=on-failure\nRestartSec=3\nTimeoutStopSec=120\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=append:'+str(self.root/(executable+'.log'))+'\nStandardError=append:'+str(self.root/(executable+'.log'))+'\n[Install]\nWantedBy=multi-user.target\n'
    path.write_text(text);subprocess.run(['systemctl','daemon-reload'],check=True)
    subprocess.run(['systemctl','enable','--now',unit],check=True,capture_output=True)
    return ServiceHandle(unit)
TestChain.spawn=managed_spawn
TestChain.setup=setup_twelve_character_accounts
TestChain.self_transact=self_transact;TestChain.self_stake=self_stake


def run(self,args,*,input_text=None,failure=None):
    args=[a.replace(' SYS',' AIC').replace('4,SYS','4,AIC')if isinstance(a,str)else a for a in args]
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
TestChain.run=run

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
TestChain.tx=tx

def audit_state(self):
    return {'governance':self.governance(),'token_stats':self.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})}
TestChain.audit_state=audit_state
