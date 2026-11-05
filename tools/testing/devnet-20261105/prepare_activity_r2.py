"""Prepare 90 funded ordinary voters and the qualified sparse business workload."""
from pathlib import Path
import base64
import hashlib
import itertools
import json
import os
import random
import shlex
import subprocess
import time
import traceback

from campaign_helpers import R, V, load_chain, ensure_bank, import_key, permission, action, transfer

os.umask(0o077)
f = None
assert not(R/'ACTIVITY_PREPARATION.json').exists(),'Reconcile previous preparation and canonical receipts before resuming'
report = {'status':'WAITING_FOR_24_BPs','TPS':'explicitly excluded'}


def save():
    (R/'ACTIVITY_PREPARATION.json').write_text(json.dumps(report,indent=2)+'\n')


try:
    save()
    deadline = time.monotonic()+1800
    while True:
        path = R/'JOIN_STAGE24.json'
        if path.exists() and json.loads(path.read_text())['status']=='REGISTERED_AND_SYNCED':
            break
        assert time.monotonic()<deadline,'Waiting for25BP enrollment timed out'
        time.sleep(2)
    f = load_chain()
    ensure_bank(f)
    bp_plan = json.loads((R/'PLAN.json').read_text())['BPs']
    bps = [row['name'] for row in bp_plan[:24]]
    chain_id = f.rpc('get_info')['chain_id']
    seed = hashlib.sha256((chain_id+'|ordinary90-votes-and-business|20261102-r1').encode()).hexdigest()
    rng = random.Random(int(seed,16))
    private_path = R/'private/ordinary-keys.json'
    public_path = R/'voting/PLAN.json'
    public_path.parent.mkdir(exist_ok=True)
    if private_path.exists():
        keys = json.loads(private_path.read_text())
        plan = json.loads(public_path.read_text())
    else:
        prefixes = ['silver','golden','gentle','silent','cosmic','autumn','bright','mellow','frozen','purple','velvet','cobalt','astral','arcane','summer','spring','winter','sunlit','serene']
        suffixes = ['willow','harbor','meadow','falcon','stream','garden','canyon','forest','valley','flower','breeze','aurora','comets']
        choices = [a+b for a,b in itertools.product(prefixes,suffixes) if a+b not in {row['name'] for row in bp_plan}]
        rng.shuffle(choices)
        actors = json.loads((R/'ORDINARY_NAMES.json').read_text())
        assert len(actors)==len(set(actors))==90 and all(len(name)==12 for name in actors)
        keys = {name:f.accounts[name] if name in f.accounts else f.key() for name in actors}
        shuffled_bps = bps.copy()
        rng.shuffle(shuffled_bps)
        assignments = shuffled_bps + [rng.choice(bps) for _ in range(90-len(bps))]
        rng.shuffle(assignments)
        plan = {'chain_id':chain_id,'seed':seed,'scope':'90 ordinary self-funded single-choice voters; key generation independent of seed',
            'voters':[{'name':name,'producer':producer,'CPU_AIC':rng.randint(100,900),'NET_AIC':10,'public_key':keys[name]['Public key']} for name,producer in zip(actors,assignments)]}
        private_path.write_text(json.dumps(keys,indent=2)+'\n')
        public_path.write_text(json.dumps(plan,indent=2)+'\n')
    for key in keys.values():
        import_key(f,key)
    report.update(status='PREPARING_90_ACCOUNTS',chain_id=chain_id,seed=seed)
    save()
    prepared_file = R/'ORDINARY_PREPARED_GROUPS.json'
    prepared = json.loads(prepared_file.read_text()) if prepared_file.exists() else []
    for start in range(0,90,5):
        if start in prepared:
            continue
        group = plan['voters'][start:start+5]
        create = []
        for row in group:
            name = row['name']
            try:
                f.rpc('get_account',{'account_name':name})
            except RuntimeError as error:
                if 'account' not in str(error).lower() and 'unknown key' not in str(error).lower():
                    raise
                auth = permission(keys[name]['Public key'])
                create += [action('newaccount','civicbankers',{'creator':'civicbankers','name':name,'owner':auth,'active':auth}),
                           action('buyram','civicbankers',{'payer':'civicbankers','receiver':name,'quant':'100.0000 AIC'})]
        if create:
            f.self_transact('create-ordinary-group-'+str(start),create)
        fund = []
        for row in group:
            name = row['name']
            fund += [transfer('civicbankers',name,'1500.0000 AIC','TEST ordinary initial allocation '+str(start)),
                     action('buyram',name,{'payer':name,'receiver':name,'quant':'100.0000 AIC'}),
                     action('delegatebw',name,{'from':name,'receiver':name,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':str(row['CPU_AIC'])+'.0000 AIC','transfer':False}),
                     action('voteproducer',name,{'voter':name,'proxy':'','producers':[row['producer']]})]
        tx = f.self_transact('fund-own-stake-and-random-votes-'+str(start),fund)
        prepared.append(start)
        prepared_file.write_text(json.dumps(prepared)+'\n')
        report.update(prepared_accounts=min(start+5,90),last_transaction=tx['transaction_id'])
        save()
    # Buy each user's own PowerUp after the complete stake population exists.
    powerup_file = R/'ORDINARY_POWERUP_GROUPS.json'
    rented = json.loads(powerup_file.read_text()) if powerup_file.exists() else []
    for start in range(0,90,5):
        if start in rented:
            continue
        acts = [transfer('civicbankers','gptgenesisbp','0.0001 AIC','TEST sponsor own PowerUp '+str(start))]
        for row in plan['voters'][start:start+5]:
            name = row['name']
            acts.append(action('powerup',name,{'payer':name,'receiver':name,'days':30,'net_frac':2000000000000,'cpu_frac':2000000000000,'max_payment':'100.0000 AIC'}))
        f.self_transact('ordinary-own-PowerUp-'+str(start),acts)
        rented.append(start)
        powerup_file.write_text(json.dumps(rented)+'\n')
    inputs = json.loads((R/'ACTIVITY_INPUTS.json').read_text())['files']
    A = R/'activity'
    (A/'contracts').mkdir(parents=True,exist_ok=True)
    for name,data in inputs.items():
        if name=='sender.py':
            continue
        raw = base64.b64decode(data['base64'])
        assert hashlib.sha256(raw).hexdigest()==data['SHA256']
        (A/name).write_bytes(raw)
    actor = 'civiccounter'
    contract_key_file = R/'private/contract-keys.json'
    if contract_key_file.exists():
        contract_key = json.loads(contract_key_file.read_text())
    else:
        contract_key = f.key()
        contract_key_file.write_text(json.dumps(contract_key,indent=2)+'\n')
    import_key(f,contract_key)
    try:
        f.rpc('get_account',{'account_name':actor})
    except RuntimeError:
        auth = permission(contract_key['Public key'])
        f.self_transact('create-business-contract',[action('newaccount','civicbankers',{'creator':'civicbankers','name':actor,'owner':auth,'active':auth}),action('buyram','civicbankers',{'payer':'civicbankers','receiver':actor,'quant':'100.0000 AIC'})])
        f.self_transact('fund-business-contract',[transfer('civicbankers',actor,'2000.0000 AIC','TEST existing qualified business contract'),action('buyram',actor,{'payer':actor,'receiver':actor,'quant':'100.0000 AIC'}),action('delegatebw',actor,{'from':actor,'receiver':actor,'stake_net_quantity':'10.0000 AIC','stake_cpu_quantity':'100.0000 AIC','transfer':False})])
    unsigned=json.loads(f.run(['set','contract',actor,str(A/'contracts'),'civiccounter.wasm','civiccounter.abi','-p',actor,'-d','-s','-j']))
    f.self_transact('deploy-qualified-business-contract',[
        transfer('civicbankers','gptgenesisbp','0.0001 AIC','TEST sponsored setcode; contract owner signs'),
        *unsigned['actions'],
    ])
    expected = inputs['contracts/civiccounter.wasm']['SHA256']
    assert f.rpc('get_code_hash',{'account_name':actor})['code_hash']==expected
    W = A/'wallet'
    W.mkdir(exist_ok=True)
    wallet_unit = 'pureeos-devnet-activity-wallet-20261105.service'
    args = [str(V/'bin/keosd'),'--data-dir',str(W),'--config-dir',str(W),'--http-server-address','']
    unit = '[Unit]\nDescription=PureEOS TEST ordinary-only activity wallet\n[Service]\nType=simple\nExecStart='+shlex.join(args)+'\nRestart=on-failure\nRestartSec=3\nStandardOutput=append:'+str(A/'wallet.log')+'\nStandardError=append:'+str(A/'wallet.log')+'\n[Install]\nWantedBy=multi-user.target\n'
    Path('/etc/systemd/system',wallet_unit).write_text(unit)
    subprocess.run(['systemctl','daemon-reload'],check=True)
    subprocess.run(['systemctl','enable','--now',wallet_unit],check=True,capture_output=True)
    deadline=time.monotonic()+20
    while not (W/'keosd.sock').exists():
        assert time.monotonic()<deadline
        time.sleep(.1)
    base=[str(V/'bin/cleos'),'--no-auto-keosd','--url',f.url,'--wallet-url','unix://'+str(W/'keosd.sock')]
    if not (W/'password.txt').exists():
        q=subprocess.run(base+['wallet','create','-n','activity-demo','--file',str(W/'password.txt')],capture_output=True,text=True)
        assert q.returncode==0,'Create ordinary activity wallet failed'
    password=(W/'password.txt').read_text().strip()
    f.secrets.append(password)
    subprocess.run(base+['wallet','unlock','-n','activity-demo'],input=password+'\n',capture_output=True,text=True)
    for key in keys.values():
        q=subprocess.run(base+['wallet','import','-n','activity-demo'],input=key['Private key']+'\n',capture_output=True,text=True)
        assert q.returncode==0 or 'already in wallet' in (q.stdout+q.stderr).lower()
    q=subprocess.run(base+['wallet','keys'],capture_output=True,text=True)
    assert q.returncode==0 and set(json.loads(q.stdout))=={key['Public key'] for key in keys.values()}
    f.action('tick',{'user':plan['voters'][0]['name'],'nonce':1,'delta':1},plan['voters'][0]['name'],contract=actor)
    f.action('note',{'user':plan['voters'][1]['name'],'nonce':1,'memo':'TEST ordinary contract qualification'},plan['voters'][1]['name'],contract=actor)
    sender=(R/'CURRENT_LOW_RATE_SENDER.py').read_text()
    sender=sender.replace('/opt/pureeos-keynet-20261103-r1',str(R)).replace('http://127.0.0.1:51003',f.url)
    sender=sender.replace("T=B/'pureeos-test-1.0.0-test.1/bin'","T=E/'native-client'")
    sender=sender.replace(json.loads((R/'PLAN.json').read_text())['old_chain_id'],chain_id)
    (A/'sender.py').write_text(sender)
    Q={'status':'PASS','signing_wallet_ordinary_keys_only':True,'ordinary_keys':90,'contract':actor,'WASM_SHA256':expected,'ABI_SHA256':inputs['contracts/civiccounter.abi']['SHA256'],'source_SHA256':inputs['civiccounter.cpp']['SHA256'],'sender_SHA256':hashlib.sha256(sender.encode()).hexdigest(),'behavior':'random1-3transactions/batch;30-60s minimum idle;90%transfers/10%tick-note;finalize/drain then random1-6actualemptyblocks;no TPS test'}
    (A/'QUALIFIED.json').write_text(json.dumps(Q,indent=2)+'\n')
    service='pureeos-devnet-ordinary-activity-20261105.service'
    unit='[Unit]\nDescription=PureEOS TEST ordinary sparse transactions and contract calls\nAfter='+wallet_unit+'\nRequires='+wallet_unit+'\n[Service]\nType=simple\nWorkingDirectory='+str(A)+'\nExecStart=/usr/bin/python3 -B '+str(A/'sender.py')+'\nRestart=on-failure\nRestartSec=10\nNice=10\nStandardOutput=append:'+str(A/'sender.log')+'\nStandardError=append:'+str(A/'sender.log')+'\n[Install]\nWantedBy=multi-user.target\n'
    Path('/etc/systemd/system',service).write_text(unit)
    subprocess.run(['systemctl','daemon-reload'],check=True)
    subprocess.run(['systemctl','enable','--now',service],check=True,capture_output=True)
    producers=f.table('producersv2')
    votes=[int(row['total_votes']) for row in producers]
    assert len(votes)==24 and len(set(votes))==24
    report.update(status='PASS',ordinary_accounts=90,distinct_BP_vote_totals=True,all24BPs_self_voted=True,sparse_activity_enabled=True,contract_WASM=expected)
    save()
    print('ORDINARY90_VOTING_AND_SPARSE_BUSINESS_STARTED',flush=True)
except BaseException:
    error=traceback.format_exc()
    if f:
        error=f.redact(error)
    report.update(status='FAIL',error=error)
    save()
    print(error,flush=True)
    raise
