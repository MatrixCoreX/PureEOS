"""Owned maintenance only. New controller; no historical campaign is invoked."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import datetime,fcntl,hashlib,json,os,re,shutil,subprocess,sys,time,urllib.request,urllib.error
ROOT=Path('/opt/pureeos-system-signup-fix-20261105-r2');R=Path('/opt/pureeos-keynet-20261103-r2')
CHAIN='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
GERMANY='pureeos-keynet2-bp-germanycrest-20261103.service'
BUILD_PREFIX='pureeos-signup-fix-r2-'
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def save(name,value):
    p=ROOT/name;q=p.with_suffix('.tmp');q.write_text(json.dumps(value,indent=2)+'\n');q.replace(p)
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def rpc(method,data=None,port=8888):
    request=urllib.request.Request(f'http://127.0.0.1:{port}/v1/chain/{method}',data=json.dumps(data or {}).encode(),headers={'Content-Type':'application/json'})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request,timeout=12) as response:return json.load(response)
        except Exception as error:
            detail={'UTC':now(),'method':method,'port':port,'attempt':attempt,'type':type(error).__name__}
            if isinstance(error,urllib.error.HTTPError):
                body=error.read().decode(errors='replace');detail['http']=error.code;detail['body']=body[:3000]
                if error.code not in [500,502,503,504]:raise
            with (ROOT/'READ_RETRIES.jsonl').open('a') as log:log.write(json.dumps(detail)+'\n')
            if attempt==3:raise
            time.sleep(.25*(attempt+1))
def info(port=8888):
    value=rpc('get_info',port=port);assert value['chain_id']==CHAIN;return value
def systemctl(command,units):
    if units:subprocess.run(['systemctl',command,*units],check=True,timeout=180)
def properties(unit):
    result=subprocess.check_output(['systemctl','show',unit,'-p','ActiveState','-p','MainPID','-p','FragmentPath'],text=True)
    return dict(line.split('=',1) for line in result.splitlines() if '=' in line)
def tables():
    names={x['name'] for x in rpc('get_abi',{'account_name':'eosio'})['abi']['tables']};result={}
    for table in ['producers','producersv2','voters','votersv2','votepolicy','bpbonds','bpbondcfg','bpmaint','rotcands']:
        if table not in names:continue
        rows=[];lower='';seen=set()
        for page in range(128):
            value=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':table,'json':True,'limit':1000,'lower_bound':lower})
            rows.extend(value['rows'])
            if not value['more']:break
            next_key=str(value.get('next_key',''));assert next_key and next_key not in seen,'Incomplete table cursor '+table
            seen.add(next_key);lower=next_key
        else:raise AssertionError('Table pagination bound '+table)
        result[table]=rows
    return result
def invariant(value):
    result={'stats':value['stats'],'code_pins':value['code_pins'],
            'governance':{k:v for k,v in value['governance'].items() if not k.startswith('head_block')}}
    table=value['tables'];result['banned']={r['owner']:r['banned'] for r in table['rotcands']}
    for name in ['bpbonds','bpbondcfg','bpmaint','votepolicy']:
        if name in table:result[name]=table[name]
    for name in ['producers','producersv2','voters','votersv2']:
        if name in table:result[name]=[{k:r[k] for k in ['owner','total_votes','producer_authority','producer_key','is_active','staked','producers','producer','last_vote_weight','proxy','is_proxy'] if k in r} for r in table[name]]
    return result
def snapshot():
    i=info();pins={}
    for account in ['eosio','eosio.token','eosio.key']:
        row=rpc('get_raw_abi',{'account_name':account});pins[account]={k:row[k] for k in ['code_hash','abi_hash']}
    state={'UTC':now(),'info':i,'governance':rpc('get_upgrade_state'),'tables':tables(),
       'stats':rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'}),'code_pins':pins,
       'producer_schedule':rpc('get_producer_schedule'),'finalizers':rpc('get_finalizer_info')}
    assert state['governance']['proposal_id']==3 and state['governance']['pending'] and state['governance']['effective_veto_count']==24
    assert {x['owner'] for x in state['tables']['rotcands'] if x['banned']}=={'germanycrest'}
    return state
def balances():
    state=json.loads((R/'activity/STATE.json').read_text());expected=state['balances'];actual={}
    for account in expected:
        row=rpc('get_currency_balance',{'code':'eosio.token','account':account,'symbol':'AIC'})
        assert len(row)==1;asset=row[0].removesuffix(' AIC');whole,frac=asset.split('.');actual[account]=int(whole)*10000+int(frac)
    assert actual==expected,'Ordinary balances differ; never repeat ambiguous payments'
    return {'balances':actual,'pending':state.get('pending'),'completed_batches':state['completed_batches'],'business_transactions':state['business_transactions']}
def pause():
    approval=json.loads((ROOT/'APPROVAL.json').read_text());assert approval['approved'] and approval['max_window_seconds']==3600
    assert not (ROOT/'WINDOW.json').exists(),'Never rerun a completed maintenance window'
    previous=None
    discovery=json.loads((ROOT/'WINDOW_DISCOVERY.json').read_text());nodes=discovery['nodes'];assert len(nodes)==26
    node_units={x['unit'] for x in nodes};services=[x['unit'] for x in discovery['active_task_services'] if x['unit'] not in node_units]
    # These are this current chain's services only, never another application.
    assert all(x.startswith(('pureeos-keynet2-','pureeos-key-history-')) for x in services)
    for node in nodes:
        assert int(properties(node['unit'])['MainPID'])==node['pid']
        path=Path(node['config_dir'])/'config.ini';text=path.read_text()
        node['config_sha256']=sha(path);unitfile=properties(node['unit'])['FragmentPath'];node['unit_file']=unitfile;node['unit_sha256']=sha(unitfile)
        address=next(line.split('=',1)[1].strip() for line in text.splitlines() if line.strip().startswith('http-server-address'))
        node['port']=int(address.rsplit(':',1)[1]);node['producer']=next((line.split('=',1)[1].strip() for line in text.splitlines() if line.strip().startswith('producer-name')),None)
    original=R/'pureeos-test-1.0.0-test.1/libexec/nodeos'
    assert sha(original)=='0be5282d1d5a5eb72081e1d1374931d5e79cff139de728f8d230e29df50534e7'
    containers=[]
    for name in ['pureeos-keynet2-history-rabbitmq','pureeos-keynet2-history-redis','pureeos-keynet2-history-mongo','pureeos-keynet2-history-elasticsearch']:
        row=json.loads(subprocess.check_output(['docker','inspect',name],text=True))[0]
        assert row['State']['Running'];containers.append({'name':name,'id':row['Id'],'image':row['Image']})
    begin=previous['start_unix'] if previous else time.time();window={'status':'PREPARING','UTC':now(),'start_unix':begin,'stop_build_unix':begin+1800,'window_end_unix':begin+3600,
       'authorization':'User explicitly chose maintenance window for builds/validation only; no deployment/reset/governance transactions',
       'nodes':nodes,'services':services,'containers':containers,'original_node_sha256':sha(original),
       'keep_running_banned_unit':GERMANY,'mutated_units':[]}
    save('WINDOW.json',window)
    guardian='pureeos-system-signup-fix-window-guardian-r3-20261105'
    subprocess.run(['systemd-run','--no-block','--unit='+guardian,'--property=Type=simple','--property=MemoryMax=256M','--property=CPUQuota=50%','--property=RuntimeMaxSec=4000','--property=StandardOutput=append:'+str(ROOT/'guardian.log'),'--property=StandardError=append:'+str(ROOT/'guardian.log'),'/usr/bin/python3',str(ROOT/'window.py'),'guard'],check=True)
    try:
        # Stop admission first; any already submitted transaction can finalize.
        window['mutated_units'].append('pureeos-keynet2-relay-20261103.service');save('WINDOW.json',window);systemctl('stop',['pureeos-keynet2-relay-20261103.service'])
        deadline=time.time()+180
        while json.loads((R/'activity/STATE.json').read_text()).get('pending') is not None:
            assert time.time()<deadline,'Business did not drain; restore services without pausing chain';time.sleep(1)
        window['mutated_units'].append('pureeos-keynet2-ordinary-activity-20261103.service');save('WINDOW.json',window);systemctl('stop',['pureeos-keynet2-ordinary-activity-20261103.service'])
        activity=json.loads((R/'activity/STATE.json').read_text());assert activity.get('pending') is None,'Stop raced a new batch; preserve it and restore without chain pause'
        # Wait actual irreversible blocks after admission stops, then freeze baseline.
        head=info()['head_block_num'];deadline=time.time()+90
        while info()['last_irreversible_block_num']<head:
            assert time.time()<deadline;time.sleep(1)
        before=snapshot();save('BEFORE_CHAIN.json',before);save('BEFORE_ACTIVITY.json',balances())
        save('BEFORE_INVARIANTS.json',invariant(before))
        rest=[x for x in services if x not in window['mutated_units']]
        window['mutated_units']+=rest;save('WINDOW.json',window);systemctl('stop',rest)
        for c in containers:subprocess.run(['docker','stop','--time','40',c['id']],check=True,timeout=60)
        paused=[x['unit'] for x in nodes if x['unit']!=GERMANY]
        window['mutated_units']+=paused;save('WINDOW.json',window);systemctl('stop',paused)
        assert all(properties(u)['ActiveState']=='inactive' for u in paused)
        assert int(properties(GERMANY)['MainPID'])==next(x['pid'] for x in nodes if x['unit']==GERMANY)
        window['status']='PAUSED_FOR_BUILD_ONLY';window['paused_UTC']=now();window['MemAvailable']=next(x for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:'))
        save('WINDOW.json',window);print(json.dumps({k:window[k] for k in ['status','paused_UTC','stop_build_unix','MemAvailable']}),flush=True)
    except BaseException:
        window['status']='PAUSE_FAILED_RESTORING';save('WINDOW.json',window);restore();raise
def wait_history_database():
    import base64,ssl
    connections=R/'history-source/config/connections.json'
    cfg=json.loads(connections.read_text())['elasticsearch']
    host=cfg['host'];assert host in ['127.0.0.1:9200','localhost:9200'],'Never send service credentials to another host'
    protocol=cfg.get('protocol','http');deadline=time.time()+720
    while True:
        try:
            token=base64.b64encode((cfg['user']+':'+cfg['pass']).encode()).decode()
            request=urllib.request.Request(protocol+'://'+host+'/_cluster/health',headers={'Authorization':'Basic '+token})
            with urllib.request.urlopen(request,timeout=8,context=ssl._create_unverified_context() if protocol=='https' else None) as response:health=json.load(response)
            if health.get('status')=='green':
                save('DATABASE_READY.json',{'UTC':now(),'status':'green','credentials_exported':False});return
        except Exception:pass
        assert time.time()<deadline,'Original history database did not recover before bounded wait'
        time.sleep(3)
def restore():
    with (ROOT/'restore.lock').open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        window=json.loads((ROOT/'WINDOW.json').read_text())
        if window['status']=='RESTORED_VERIFIED':return
        window['status']='RESTORING';save('WINDOW.json',window)
        units=json.loads(subprocess.check_output(['systemctl','list-units','--all','--type=service','--output=json'],text=True))
        stop=[x['unit'] for x in units if x['unit'].startswith(BUILD_PREFIX) and x['active'] in ['active','activating']]
        systemctl('stop',stop)
        # Restore only the services this controller actually stopped. Never touch
        # Germany's preexisting banned process or archived stopped identities.
        node_units=[x['unit'] for x in window['nodes'] if x['unit'] in window['mutated_units']]
        systemctl('start',node_units)
        deadline=time.time()+240;base=None
        while time.time()<deadline:
            try:
                base=info();break
            except Exception:time.sleep(2)
        assert base,'Original RPC did not return after restart'
        initial_head=base['head_block_num'];normal_deadline=time.time()+35
        while time.time()<normal_deadline:
            current=info()
            if current['last_irreversible_block_num']>=initial_head+3:break
            time.sleep(1)
        else:
            healthy=[x for x in window['nodes'] if x['unit']!=GERMANY]
            deadline=time.time()+180
            while True:
                common=info();states=[]
                for node in healthy:states.append(info(node['port']))
                if all(s['head_block_id']==common['head_block_id'] and s['last_irreversible_block_id']==common['last_irreversible_block_id'] for s in states):break
                assert time.time()<deadline,'Healthy nodes did not reach a common durable head';time.sleep(2)
            names=[x['producer_name'] for x in rpc('get_producer_schedule')['active']['producers']]
            assert len(names)==21 and 'germanycrest' not in names
            selected=next(x for x in healthy if x['producer'] in names)
            config=Path(selected['config_dir'])/'config.ini';original=config.read_bytes()
            assert sha(config)==selected['config_sha256'] and b'enable-stale-production' not in original
            private=ROOT/'private-runtime-backups';private.mkdir(mode=0o700,exist_ok=True)
            backup=private/'selected-before-stale.ini';backup.write_bytes(original);backup.chmod(0o600)
            recovery={'unit':selected['unit'],'producer':selected['producer'],'initial_head':common['head_block_num'],'original_sha256':selected['config_sha256'],'config_restored':False,'UTC':now()}
            save('RECOVERY.json',recovery)
            try:
                target=names.index(selected['producer']);deadline=time.time()+150
                while True:
                    slot=int((time.time()-946684800)*2)+1
                    if (slot//12)%21==target and slot%12<=3:break
                    assert time.time()<deadline;time.sleep(.2)
                config.write_bytes(original+b'\nenable-stale-production = true\n')
                systemctl('restart',[selected['unit']]);deadline=time.time()+120
                while True:
                    current=info()
                    if current['last_irreversible_block_num']>=common['head_block_num']+3:break
                    assert time.time()<deadline,'Native quorum did not recover; never bypass safety';time.sleep(1)
                recovery['recovered_info']=current
            finally:
                config.write_bytes(original);assert sha(config)==selected['config_sha256'];recovery['config_restored']=True
                systemctl('restart',[selected['unit']]);save('RECOVERY.json',recovery)
        for node in window['nodes']:
            assert sha(Path(node['config_dir'])/'config.ini')==node['config_sha256'];assert sha(node['unit_file'])==node['unit_sha256']
        assert sha(R/'pureeos-test-1.0.0-test.1/libexec/nodeos')==window['original_node_sha256']
        assert int(properties(GERMANY)['MainPID'])==next(x['pid'] for x in window['nodes'] if x['unit']==GERMANY)
        # Verify economic/governance state and balances before admitting activity.
        after=snapshot();save('AFTER_CHAIN_BEFORE_ACTIVITY.json',after)
        if (ROOT/'BEFORE_INVARIANTS.json').exists():assert invariant(after)==json.loads((ROOT/'BEFORE_INVARIANTS.json').read_text()),'Frozen governance/economic state differs'
        if (ROOT/'BEFORE_ACTIVITY.json').exists():assert balances()==json.loads((ROOT/'BEFORE_ACTIVITY.json').read_text()),'Ordinary state differs before resume'
        for c in window['containers']:subprocess.run(['docker','start',c['id']],check=True,timeout=60)
        wait_history_database()
        apps=[x for x in window['services'] if x in window['mutated_units']]
        systemctl('start',apps)
        window['status']='RESTORED_VERIFIED';window['restored_UTC']=now();window['elapsed_seconds']=time.time()-window['start_unix'];window['after_head']=after['info']['head_block_num'];window['after_LIB']=after['info']['last_irreversible_block_num'];window['all_original_units_active']=all(properties(x['unit'])['ActiveState']=='active' for x in window['nodes']);save('WINDOW.json',window)
        print(json.dumps({k:window[k] for k in ['status','restored_UTC','elapsed_seconds','after_head','after_LIB','all_original_units_active']}),flush=True)
def guard():
    while True:
        window=json.loads((ROOT/'WINDOW.json').read_text())
        if window['status']=='RESTORED_VERIFIED':return
        if time.time()>=window['stop_build_unix'] or (ROOT/'RESTORE_REQUESTED').exists():restore();return
        time.sleep(5)
if __name__=='__main__':{'pause':pause,'restore':restore,'guard':guard}[sys.argv[1]]()
