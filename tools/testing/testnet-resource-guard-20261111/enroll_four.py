"""Four fresh Nov11 replacements and only their affected owned external ballots."""
from finance_common import *
import pwd
import shlex
import traceback

assert not (R/'REPLACEMENT_ENROLL_STARTED.json').exists(), 'Reconcile exact original receipts first'
baseline=read('REPLACEMENT_LIVE_BASELINE.json');assert baseline['status']=='PASS'
checkpoint=read('REPLACEMENT_CHECKPOINT.json');assert checkpoint['status']=='PASS'
assert hashlib.sha256(Path(checkpoint['snapshot']).read_bytes()).hexdigest()==checkpoint['sha256']
plan=read('REPLACEMENTS_PREPARED.json');keys=read('private/replacement-keys.json')
owner=pwd.getpwnam('pureeostest')
progress={'status':'STARTING','utc':now(),'completed':[],'vote_receipts':[]}
save('REPLACEMENT_ENROLL_STARTED.json',progress)
def report(status,**data):
    progress.update(status=status,utc=now(),**data);save('REPLACEMENT_ENROLL_PROGRESS.json',progress)
    print(json.dumps({'status':status,'current':progress.get('current'),'completed':len(progress['completed'])}),flush=True)
def guard():
    original_pids();i=rpc('get_info');assert i['chain_id']==CHAIN
    assert i['head_block_num']-i['last_irreversible_block_num']<25
    age=(datetime.datetime.now(datetime.timezone.utc)-datetime.datetime.fromisoformat(i['head_block_time']+'+00:00')).total_seconds()
    assert age<8,'Current blocks stale; halt additional enrollment'
    mem={l.split(':')[0]:int(l.split()[1])*1024 for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith(('MemAvailable:','SwapFree:'))}
    assert mem['SwapFree']>8*1024**3 and mem['MemAvailable']>512*1024**2,'Pressure guard; leave live chain running'
    assert {x['owner'] for x in table('rotcands') if x['banned']}==set(baseline['banned'])
    return i
def find_bank_key():
    a=rpc('get_account',{'account_name':BANK})
    active=next(x['required_auth'] for x in a['permissions'] if x['perm_name']=='active')
    assert active['threshold']==1 and len(active['keys'])==1 and not active['accounts'] and not active['waits']
    pub=active['keys'][0]['key'];found=[]
    def walk(x):
        if isinstance(x,dict):
            if x.get('Public key')==pub and isinstance(x.get('Private key'),str):found.append(x)
            for v in x.values():walk(v)
        elif isinstance(x,list):
            for v in x:walk(v)
    for p in [C/'private/bank-keys.json',C/'private/bootstrap-keys.json']:
        walk(json.loads(p.read_text()))
    assert found,'Owned bank key missing; no authority modification allowed'
    return found[0]
def start_node(node,key):
    name=node['name'];root=R/'nodes'/name;assert not root.exists()
    (R/'nodes').mkdir(exist_ok=True,mode=0o750);os.chown(R/'nodes',0,owner.pw_gid);(R/'nodes').chmod(0o750)
    (R/'nodes').mkdir(exist_ok=True,mode=0o750);os.chown(R/'nodes',0,owner.pw_gid)
    (root/'config').mkdir(parents=True);data=root/'data';data.mkdir()
    source=(C/'bps/anthropiclab/config/config.ini').read_text()
    identity={'producer-name','signature-provider','peer-private-key','p2p-bp-gossip-endpoint',
        'p2p-peer-address','p2p-server-address','http-server-address','p2p-listen-endpoint','agent-name',
        'enable-stale-production','pause-on-startup','max-reversible-blocks'}
    public='\n'.join(l for l in source.splitlines() if '=' not in l or l.split('=',1)[0].strip() not in identity)
    assert 'validation-mode = full' in public and 'chain-state-db-size-mb = 512' in public
    extra=['http-server-address = 127.0.0.1:'+str(node['rpc_port']),
        'p2p-listen-endpoint = 127.0.0.1:'+str(node['p2p_port']),'producer-name = '+name,
        'p2p-bp-gossip-endpoint = '+name+',127.0.0.1:'+str(node['p2p_port'])+',127.0.0.1',
        'peer-private-key = '+json.dumps([key['peer']['Public key'],key['peer']['Private key']]),
        'pause-on-startup = false','enable-stale-production = false','max-reversible-blocks = 3600']
    extra+=['p2p-peer-address = 127.0.0.1:'+str(p) for p in [9876,9877,9951,9955]]
    extra+=['signature-provider = '+key[role]['Public key']+'=KEY:'+key[role]['Private key'] for role in ['production','bls']]
    cfg=root/'config/config.ini';cfg.write_text(public+'\n'+'\n'.join(extra)+'\n');cfg.chmod(0o600)
    (root/'config/logging.json').write_bytes((C/'bps/anthropiclab/config/logging.json').read_bytes())
    for p in [root,*root.rglob('*')]:os.chown(p,owner.pw_uid,owner.pw_gid)
    root.chmod(0o750)
    command=['/usr/bin/python3',str(R/'replacement_node_entry.py'),name]
    u=Path('/etc/systemd/system')/node['unit'];assert not u.exists()
    u.write_text('[Unit]\nDescription=PureEOS resilient replacement TEST BP '+name+'\nAfter=network-online.target\nStartLimitIntervalSec=600\nStartLimitBurst=5\n[Service]\nType=simple\nUser=pureeostest\nGroup=pureeostest\nWorkingDirectory='+str(root)+'\nExecStart='+shlex.join(command)+'\nSlice=pureeos-consensus.slice\nEnvironment=MALLOC_ARENA_MAX=2\nEnvironment=MALLOC_TRIM_THRESHOLD_=131072\nMemoryHigh=384M\nMemoryLow=64M\nMemoryMax=infinity\nMemorySwapMax=infinity\nCPUWeight=500\nIOWeight=500\nOOMScoreAdjust=-400\nRestart=on-failure\nRestartSec=30\nTimeoutStopSec=infinity\nLimitCORE=0\nLimitNOFILE=65536\nNoNewPrivileges=true\nStandardOutput=journal\nStandardError=journal\nLogRateLimitIntervalSec=30s\nLogRateLimitBurst=1000\n[Install]\nWantedBy=multi-user.target\n')
    subprocess.run(['systemctl','daemon-reload'],check=True,capture_output=True)
    subprocess.run(['systemctl','enable','--now',node['unit']],check=True,capture_output=True)
    before=guard();deadline=time.monotonic()+300;samples=[]
    while time.monotonic()<deadline:
        live=guard()
        try:
            i=rpc('get_info',port=node['rpc_port']);assert i['chain_id']==CHAIN
            if live['head_block_num']-i['head_block_num']<10 and i['last_irreversible_block_num']>=before['last_irreversible_block_num']:
                b=rpc('get_block',{'block_num_or_id':i['last_irreversible_block_num']})
                assert b['id']==i['last_irreversible_block_id'];samples.append({'utc':now(),'info':i})
                if len(samples)>=3:return {'PID':int(state(node['unit'])['MainPID']),'sync_samples':samples,'native_SHA256':BINARY,'profile_SHA256':hashlib.sha256(public.encode()).hexdigest()}
        except (OSError,urllib.error.URLError,RuntimeError):
            assert state(node['unit'])['ActiveState']=='active','New node failed; keep data/config/keys, no blind retries'
        time.sleep(2)
    raise TimeoutError('New node not synchronized; no registration without sync')

try:
    guard()
    with Wallet() as w:
        w.import_key(find_bank_key())
        for node in plan['nodes']:
            name=node['name'];key=keys[name];guard();report('CREATING_NEW_OWNED_REPLACEMENT',current=name)
            w.import_key(key['account']);auth=permission(key['account']['Public key'])
            create=w.send('replacement-create-'+name,[sysact('newaccount',BANK,{'creator':BANK,'name':name,'owner':auth,'active':auth}),
                sysact('buyram',BANK,{'payer':BANK,'receiver':name,'quant':'1.0000 AIC'})])
            report('FUNDING_NEW_SELF_BOND_AND_RESOURCES')
            fund=w.send('replacement-fund-'+name,[transfer(BANK,name,'16000.0000 AIC','Owned TEST replacement '+name),
                sysact('delegatebw',name,{'from':name,'receiver':name,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':node['CPU_stake_AIC']+' AIC','transfer':False}),
                sysact('powerup',name,{'payer':name,'receiver':name,'days':30,'net_frac':5000000000000,'cpu_frac':5000000000000,'max_payment':'100.0000 AIC'}),
                sysact('bondbp',name,{'producer':name})])
            report('SYNCHRONIZING_NEW_REPLACEMENT');runtime=start_node(node,key)
            report('REGISTERING_NEW_KEYS_AND_SELF_VOTE')
            actions=[sysact('regproducer',name,{'producer':name,'producer_key':key['production']['Public key'],'url':'https://aic-test.matrixai.one','location':0}),
                sysact('regfinkey',name,{'finalizer_name':name,'finalizer_key':key['bls']['Public key'],'proof_of_possession':key['bls']['Proof of Possession']}),
                sysact('regpeerkey',name,{'proposer_finalizer_name':name,'key':key['peer']['Public key']}),
                sysact('voteproducer',name,{'voter':name,'proxy':'','producers':[name]})]
            actions[0]['authorization'].insert(0,{'actor':BANK,'permission':'active'})
            reg=w.send('replacement-register-'+name,actions)
            progress['completed'].append({'name':name,'create':create,'fund':fund,'registration':reg,'runtime':runtime})
            report('REPLACEMENT_REGISTERED_RUNNING')
        assert len(progress['completed'])==4
        owned=read('VOTE_SIGNER_OWNERSHIP.json')['accounts'];before={x['owner']:x for x in baseline['affected_external']}
        for x in owned:
            s=x['key_source'];v=json.loads(Path(s['source']).read_text())
            for k in s['json_path']:v=v[k]
            w.import_key({'Private key':v[s['private_field']],'Public key':v[s['public_field']]})
        intents=read('AUTHORIZED_VOTE_INTENTS.json')['intents'];names=sorted(intents)
        for start in range(0,len(names),5):
            guard();current={x['owner']:x for x in table('votersv2')};batch=names[start:start+5]
            for name in batch:assert current[name]==before[name],'Ballot changed since preparation; do not overwrite new choice'
            actions=[sysact('voteproducer',name,{'voter':name,'proxy':'','producers':intents[name]}) for name in batch]
            actions[0]['authorization'].insert(0,{'actor':BANK,'permission':'active'})
            report('REDIRECTING_ONLY_EXTERNAL_BLACKLIST_BALLOTS',current=','.join(batch))
            receipt=w.send('redirect-external-'+str(start//5+1),actions)
            progress['vote_receipts'].append({'voters':batch,'receipt':receipt});report('EXTERNAL_BALLOT_BATCH_IRREVERSIBLE')
    voters={x['owner']:x for x in table('votersv2')};new={x['name'] for x in plan['nodes']}
    rex_now={x['owner']:x for x in table('rexbal')};refreshes=[]
    def units(v):return int(v.split()[0].replace('.',''))
    for old in baseline['voters']:
        after=voters[old['owner']]
        if after['staked']!=old['staked']:
            n=old['owner'];assert n in intents and n in baseline.get('rex_owned_baseline',{})
            before_rex=baseline['rex_owned_baseline'][n]['rexbal'];r=rex_now[n]
            delta=units(r['vote_stake'])-units(before_rex['vote_stake'])
            assert after['staked']-old['staked']==delta and delta>=0
            assert {k:v for k,v in r.items()if k!='vote_stake'}=={k:v for k,v in before_rex.items()if k!='vote_stake'}
            assert table('delband','eosio',n)==baseline['rex_owned_baseline'][n]['self_delegated']
            refreshes.append({'owner':n,'native_cached_REX_vote_units_delta':delta,'principal_and_shares_unchanged':True})
        if old['owner'] in intents:
            assert after['producers']==intents[old['owner']] and not set(after['producers']).intersection(baseline['banned'])
        else:assert after==old,'Unrelated vote changed'
    save('REX_VOTE_REFRESH_RECONCILIATION.json',{'utc':now(),'refreshes':refreshes})
    for name in new:assert voters[name]['producers']==[name]
    bonds={x['owner']:x for x in table('bpbonds')}
    for old in baseline['bonds']:assert bonds[old['owner']]==old
    for name in new:assert int(bonds[name]['balance'])==100000000 and not int(bonds[name]['forfeited'])
    cfg=table('bpbondcfg')[0];oldcfg=baseline['bond_config'][0]
    assert int(cfg['locked'])==int(oldcfg['locked'])+400000000
    for field in ['slash_pending','slash_settled']:assert cfg[field]==oldcfg[field]
    for code,symbol in [('eosio.token','AIC'),('usd','USD'),('cny','CNY')]:assert rpc('get_currency_stats',{'code':code,'symbol':symbol})==baseline['token_stats'][code]
    for a,pin in baseline['code_ABI_pins'].items():
        v=rpc('get_raw_abi',{'account_name':a});assert all(v[k]==h for k,h in pin.items())
    assert table('relays','eosio.key','eosio.key')==baseline['relays']
    prods=table('producersv2');assert len(prods)==41 and len({int(x['total_votes']) for x in prods})==41
    guard();report('PASS_4_REPLACEMENTS_AND_OWNED_EXTERNAL_VOTE_REDIRECTIONS',registered=41,physical_BP_target=25,
        old15_bans_preserved=True,external_redirected=len(intents),banned15_self_ballots_unchanged=True,
        supplies_caps_contracts_and_old_bonds_unchanged=True,all37_vote_totals_distinct=True)
    save('REPLACEMENT_ENROLL_RESULT.json',progress)
except BaseException:
    report('NEEDS_CANONICAL_RECONCILIATION',error=traceback.format_exc());save('REPLACEMENT_ENROLL_FAILURE.json',progress);raise
