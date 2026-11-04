"""Native proposal/execution for the exact previously qualified RAM candidate.
Run remotely only. A repeated command resumes the same signed packet, never
re-prepares an ambiguous payment or bypasses the native review/veto rules.
"""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,sys
os.umask(0o077)
K=Path('/opt/pureeos-dev-proposals-20261104-r1');Q=Path('/opt/pureeos-key-ram-market-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2')
sys.path.insert(0,str(R))
from campaign_helpers import load_chain
action=sys.argv[1];assert action in ['propose','execute']
for label in ['native','fixture']:assert json.loads((Q/('QUALIFICATION_'+label+'_r3.json')).read_text())['status']=='PASS'
assert json.loads((Q/'APPLICATION_REVIEW_r5.json').read_text())['status']=='PASS'
assert json.loads((K/'DEPLOYMENT.json').read_text())['status']=='PUBLISHED'
assert json.loads((K/'browser-public-before/RESULT.json').read_text())['status']=='PASS'
assert json.loads((K/'FRONTEND_RESULT.json').read_text())['status']=='PASS'
f=load_chain();f.cleos=R/'native-client/cleos'
CHAIN='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
OLD='1933629daab4d8fe3b64bfe0528b7d64b85dff97011b71301ba4122f590cf0f5'
NEW='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
ABI='3470a5e243680c4c93e7ef4b481bcd54cb4f626ed3d92331f414386bb5bb0043'
sha=lambda x:hashlib.sha256(x).hexdigest()
code=(Q/'artifacts/eosio.key.wasm').read_bytes();abi=(Q/'artifacts/candidate.abi.bin').read_bytes();assert sha(code)==NEW and sha(abi)==ABI
assert f.rpc('get_info')['chain_id']==CHAIN
state=f.governance();assert state['sealed']and state['review_seconds']==1800
assert f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash']=='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
assert f.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']=='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
private=K/'private-native';private.mkdir(mode=0o700,exist_ok=True)
def save(path,value):path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n')
def send(label,transaction):
    signed_path=private/(label+'-signed.json');progress_path=K/(label.upper()+'_PROGRESS.json')
    if not signed_path.exists():
        unsigned=private/(label+'-unsigned.json');save(unsigned,transaction)
        signed=json.loads(f.run(['push','transaction',str(unsigned),'-j','-x','1800','--dont-broadcast','--compression','zlib']))
        assert signed['signatures'];save(signed_path,signed)
        packed=json.loads(f.run(['convert','pack_transaction',str(signed_path)]));assert packed['compression']in ['none',0]
        progress={'status':'SIGNED_IMMUTABLE','transaction_id':sha(bytes.fromhex(packed['packed_trx'])),'packet_sha256':sha(signed_path.read_bytes()),
                  'start_LIB':f.rpc('get_info')['last_irreversible_block_num'],'CPU_cap_us':150000,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat()}
        save(progress_path,progress)
    else:
        progress=json.loads(progress_path.read_text());assert progress['packet_sha256']==sha(signed_path.read_bytes())
        # A previous accepted/ambiguous result must be reconciled, not sent again.
        if progress['status']not in ['SIGNED_IMMUTABLE','CPU_REJECTED_CANONICAL_ABSENT']:
            raise RuntimeError('Reconcile the saved immutable '+label+' transaction before another broadcast')
    before_gov=f.governance();before_hash=f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']
    for attempt in range(1,4):
        before=f.rpc('get_info');progress['status']='BROADCASTING';save(progress_path,progress)
        p=subprocess.run([str(f.cleos),'--no-auto-keosd','--url',f.url,'--wallet-url','unix://'+str(f.root/'wallet/keosd.sock'),
            'push','transaction',str(signed_path),'-s','-j','--compression','zlib'],capture_output=True,text=True,timeout=90)
        try:result=json.loads(p.stdout)
        except Exception:
            (K/(label+'-ambiguous.log')).write_text(f.redact(p.stdout+p.stderr));raise RuntimeError('Ambiguous native response; preserve and reconcile immutable packet')
        assert result['transaction_id']==progress['transaction_id']
        trace=result['processed']
        if p.returncode==0:
            assert trace['receipt']['status']=='executed';progress.update(status='ACCEPTED_AWAITING_IRREVERSIBILITY',block_hint=trace['block_num']);save(progress_path,progress)
            receipt=f.wait_transaction(progress['transaction_id'],before['last_irreversible_block_num']);progress['status']='IRREVERSIBLE';save(progress_path,progress)
            return result,receipt
        (K/(label+'-rejection-'+str(attempt)+'.log')).write_text(f.redact(p.stdout+p.stderr))
        assert trace.get('receipt')is None and trace['except']['name']=='tx_cpu_usage_exceeded','Non-CPU rejection: preserve immutable packet'
        end=f.wait_lib(trace['block_num']+2);scanned=[]
        for n in range(before['last_irreversible_block_num']+1,end['last_irreversible_block_num']+1):
            b=f.rpc('get_block',{'block_num_or_id':n});assert not any((t['trx']if isinstance(t['trx'],str)else t['trx']['id'])==progress['transaction_id']for t in b['transactions'])
            scanned.append({'block':n,'id':b['id']})
        assert f.governance()==before_gov and f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==before_hash
        save(K/(label+'-cpu-'+str(attempt)+'.json'),{'transaction_id':progress['transaction_id'],'same_packet':True,'canonical_absence':scanned,'governance_and_code_unchanged':True,'CPU_cap_us':150000})
        progress['status']='CPU_REJECTED_CANONICAL_ABSENT';save(progress_path,progress);assert attempt<3,'Repeated CPU refusal; keep the exact packet'
if action=='propose':
    assert not(K/'PROPOSAL.json').exists()and not state['pending']
    base=f.rpc('get_raw_abi',{'account_name':'eosio.key'});assert base['code_hash']==OLD and base['abi_hash']==ABI
    review={'change':'Market-priced RAM allocation fees','source_commit':'d5c176e7d9f14ad98129c5631f3bbdece8a3ba3e','source_kit_sha256':'6d1e987f8d5dbb3f65c763d348954ab7b8c002d302d76ef11f56754f8a7250a6',
      'new_code_hash':NEW,'new_abi_hash':ABI,'native_checks':114,'fixture_checks':115,'SDK_tests':21,'fee_UI_tests':241,'SDK_UI_comparisons':480,'fee_UI_Chrome_variants':8,
      'proposal_UI_source_sha256':json.loads((K/'FRONTEND_RESULT.json').read_text())['source_sha256'],
      'approved_bytes':{'existing_balance':0,'new_balance_or_native_AIC_recipient':512,'new_key_and_balance':1536},
      'RAM_formula':'ceil(ceil(AIC_reserve * bytes / (RAM_reserve - bytes)) * 200 / 199); bytes=0 costs0',
      'base_formula':'max(existing_minimum, ceil(unit_fee * work_units * CPU_NET_multiplier_bps / 10000) + RAM_quote)',
      'RAM_buy_fee_percent':'0.5','automatic_RAM_purchase':False,'exact_RAM_metering':False,'minimum_base_AIC':'0.0100','relay_bps':[1000,3000],
      'signed_total_cap':True,'fee_free_claim_preserved':True,'state_and_request_ABI_unchanged':True,'sponsored_budgets_and_AIC_USD_whitelist_preserved':True,
      'third_party_named_recipient_must_preopen':True,'only_founder_HTTP':True,'native_review_seconds':1800,'production_review_seconds_unchanged':604800,'reset':False,'node_or_system_change':False}
    review_bytes=(json.dumps(review,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode();(K/'RAM_REVIEW.json').write_bytes(review_bytes)
    proposal={'target':'eosio.key','old_code_hash':OLD,'old_abi_hash':ABI,'new_code_hash':NEW,'new_abi_hash':ABI,'review_document_hash':sha(review_bytes)}
    auth=[{'actor':state['developer'],'permission':'upgrade'}]
    tx={'actions':[{'account':'eosio','name':name,'authorization':auth,'data':data}for name,data in [('refreshgov',{}),('devpropose',proposal)]]}
    result,receipt=send('propose',tx);after=f.governance()
    assert after['pending']and after['target']=='eosio.key'and all(after[k]==v for k,v in proposal.items())
    assert(datetime.datetime.fromisoformat(after['execute_after'])-datetime.datetime.fromisoformat(after['proposed_at'])).total_seconds()==1800
    assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']==OLD
    save(K/'PROPOSAL.json',{'status':'PROPOSED_IRREVERSIBLE','chain_id':CHAIN,'transaction_id':result['transaction_id'],'canonical_receipt':receipt,'proposal':proposal,'governance':after,'review_seconds':1800})
    print(json.dumps({'status':'PROPOSED_IRREVERSIBLE','proposal_id':after['proposal_id'],'transaction_id':result['transaction_id'],'execute_after':after['execute_after']}))
else:
    proposal=json.loads((K/'PROPOSAL.json').read_text());assert proposal['status']=='PROPOSED_IRREVERSIBLE'
    assert state['pending']and state['proposal_id']==proposal['governance']['proposal_id']and state['target']=='eosio.key'
    for k,v in proposal['proposal'].items():assert state[k]==v
    assert state['effective_voter_count']>0 and not state['execution_blocked_no_electorate']and state['effective_veto_count']<state['required_vetoes']
    info=f.rpc('get_info');assert datetime.datetime.fromisoformat(info['head_block_time'])>=datetime.datetime.fromisoformat(state['execute_after'])
    old_raw=f.rpc('get_raw_abi',{'account_name':'eosio.key'});assert old_raw['code_hash']==OLD and old_raw['abi_hash']==ABI
    tx={'actions':[{'account':'eosio','name':'devexec','authorization':[{'actor':state['developer'],'permission':'upgrade'}],
        'data':{'proposal_id':state['proposal_id'],'code':code.hex(),'abi':abi.hex()}}]}
    result,receipt=send('execute',tx);actual=f.rpc('get_raw_abi',{'account_name':'eosio.key'});after=f.governance()
    assert actual['code_hash']==NEW and actual['abi_hash']==ABI and not after['pending']
    save(K/'EXECUTION.json',{'status':'UPGRADED_IRREVERSIBLE','chain_id':CHAIN,'transaction_id':result['transaction_id'],'canonical_receipt':receipt,'proposal_id':state['proposal_id'],
        'new_code_hash':NEW,'new_abi_hash':ABI,'review_seconds':1800,'before_governance':state,'after_governance':after,'UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'reset':False})
    print(json.dumps({'status':'UPGRADED_IRREVERSIBLE','transaction_id':result['transaction_id'],'new_code_hash':NEW}))
