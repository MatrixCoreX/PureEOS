"""Exact negative native governance test: cap2x proposal, owner vetoes, mature rejection.
No bypass, mint, direct cap setter or candidate public deployment.
"""
from pathlib import Path
import base64,datetime,hashlib,json,os,subprocess,sys
os.umask(0o077)
K=Path('/opt/pureeos-cap-veto-20261104-r1');R=Path('/opt/pureeos-keynet-20261103-r2');RAM=Path('/opt/pureeos-dev-proposals-20261104-r1')
action=sys.argv[1];assert action in ['propose','veto','attempt']
assert json.loads((RAM/'LIVE_SMOKE.json').read_text())['status']=='PASS'
assert json.loads((RAM/'browser-public-after/RESULT.json').read_text())['status']=='PASS'
assert json.loads((K/'QUALIFICATION_RESULT.json').read_text())['status']=='PASS'
sys.path.insert(0,str(R));from campaign_helpers import load_chain
f=load_chain();f.cleos=R/'native-client/cleos';f.url='http://127.0.0.1:8890';WRITER='http://127.0.0.1:8888'
CHAIN='116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
OLD='f8c5fbfb14bbab7da4f5bd3cb04a2a8967c5e0d39f5794eb12768a13e1375d0c'
NEW='bee8320e5e761703f469fb6b0f8c0744479b0bf329f709867bb613855808845a'
sha=lambda data:hashlib.sha256(data).hexdigest()
code=(K/'artifacts/eosio.token.wasm').read_bytes();assert sha(code)==NEW
assert f.rpc('get_info')['chain_id']==CHAIN
assert f.read_rpc(WRITER,'get_info')['chain_id']==CHAIN
assert f.rpc('get_code_hash',{'account_name':'eosio.key'})['code_hash']=='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1'
assert f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash']=='b591ca661e8256b7d1c7370fe11e5825427c98b4f84eceb3cdf735c06e5e8170'
raw=f.rpc('get_raw_abi',{'account_name':'eosio.token'});assert raw['code_hash']==OLD
abi=base64.b64decode(raw['abi']);assert sha(abi)==raw['abi_hash'];ABI=raw['abi_hash']
path=K/'artifacts/candidate.abi.bin'
if path.exists():assert path.read_bytes()==abi
else:path.write_bytes(abi)
state=f.governance();assert state['sealed']and state['review_seconds']==1800
stats=f.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC'];assert stats['max_supply']=='2100000000.0000 AIC'and stats['supply']=='100000000.0000 AIC'
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
        if progress['status']=='IRREVERSIBLE':return {'transaction_id':progress['transaction_id']},f.wait_transaction(progress['transaction_id'],progress['start_LIB'])
        if progress['status']not in ['SIGNED_IMMUTABLE','CPU_REJECTED_CANONICAL_ABSENT']:
            raise RuntimeError('Reconcile the saved immutable '+label+' transaction before another broadcast')
    before_gov=f.governance();before_hash=f.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']
    for attempt in range(1,4):
        before=f.rpc('get_info');progress['status']='BROADCASTING';save(progress_path,progress)
        p=subprocess.run([str(f.cleos),'--no-auto-keosd','--url',WRITER,'--wallet-url','unix://'+str(f.root/'wallet/keosd.sock'),
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
        assert f.governance()==before_gov and f.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']==before_hash
        save(K/(label+'-cpu-'+str(attempt)+'.json'),{'transaction_id':progress['transaction_id'],'same_packet':True,'canonical_absence':scanned,'governance_and_code_unchanged':True,'CPU_cap_us':150000})
        progress['status']='CPU_REJECTED_CANONICAL_ABSENT';save(progress_path,progress);assert attempt<3,'Repeated CPU refusal; keep the exact packet'

if action=='propose':
    assert not state['pending']and not(K/'PROPOSAL.json').exists()
    review={'title':'AIC maximum supply doubled: unanimous BP veto test','chain_id':CHAIN,'old_max_supply':'2100000000.0000 AIC','proposed_max_supply':'4200000000.0000 AIC','issued_supply_unchanged':'100000000.0000 AIC',
      'target':'eosio.token','old_code_hash':OLD,'new_code_hash':NEW,'old_abi_hash':ABI,'new_abi_hash':ABI,
      'source_archive_sha256':json.loads((K/'SOURCE_ARCHIVE.json').read_text())['sha256'],'source_CPP_sha256':sha((K/'source/src/eosio.token.cpp').read_bytes()),
      'qualification_report_sha256':sha((K/'QUALIFICATION_RESULT.json').read_bytes()),'private_functional_checks':8,
      'migration':'After an approved code upgrade, the first successful eosio.token::4,AIC transfer with issuer=eosio and exact old cap21b changes max_supply to42b. It does not mint or change ABI/tables. Failed transfers roll the change back; repeated and other-token transfers do not double again.',
      'campaign':'All effective frozen BP owners veto once. After full1800s native review, attempt this exact reviewed payload and require native veto rejection. Leave the vetoed proposal visible. No public activation intended.',
      'direct_cap_setter':False,'mint':False,'runtime_whitelist_change':False,'consensus_or_node_changes':False,'native_review_seconds':1800,'production_review_seconds_unchanged':604800}
    data=(json.dumps(review,ensure_ascii=False,sort_keys=True,indent=2)+'\n').encode();(K/'CAP_REVIEW.json').write_bytes(data)
    proposed={'target':'eosio.token','old_code_hash':OLD,'old_abi_hash':ABI,'new_code_hash':NEW,'new_abi_hash':ABI,'review_document_hash':sha(data)}
    auth=[{'actor':state['developer'],'permission':'upgrade'}]
    tx={'actions':[{'account':'eosio','name':name,'authorization':auth,'data':value}for name,value in [('refreshgov',{}),('devpropose',proposed)]]}
    result,receipt=send('propose',tx);after=f.governance()
    assert after['pending']and all(after[key]==value for key,value in proposed.items())and after['effective_voter_count']==24
    assert 'germanycrest'not in after['proposal_voters']
    assert(datetime.datetime.fromisoformat(after['execute_after'])-datetime.datetime.fromisoformat(after['proposed_at'])).total_seconds()==1800
    save(K/'PROPOSAL.json',{'status':'PROPOSED_IRREVERSIBLE','chain_id':CHAIN,'transaction_id':result['transaction_id'],'canonical_receipt':receipt,'proposal':proposed,'governance':after,'token_stats':stats})
    print(json.dumps({'status':'PROPOSED_IRREVERSIBLE','proposal_id':after['proposal_id'],'transaction_id':result['transaction_id'],'execute_after':after['execute_after']}))
elif action=='veto':
    proposal=json.loads((K/'PROPOSAL.json').read_text());assert state['pending']and state['proposal_id']==proposal['governance']['proposal_id']
    for key,value in proposal['proposal'].items():assert state[key]==value
    frozen=proposal['governance']['effective_proposal_voters'];assert len(frozen)==24
    directory=K/'vetoes';directory.mkdir(exist_ok=True)
    receipts=[]
    for bp in frozen:
        path=directory/(bp+'.json')
        if path.exists():
            entry=json.loads(path.read_text());assert bp in f.governance()['effective_vetoes'];receipts.append(entry);continue
        current=f.governance();assert datetime.datetime.fromisoformat(f.rpc('get_info')['head_block_time'])<datetime.datetime.fromisoformat(current['execute_after'])
        assert bp in current['effective_proposal_voters']
        if bp in current['effective_vetoes']:assert (K/('VETO-'+bp.upper()+'_PROGRESS.json')).exists(),'Reconcile an external veto before proceeding'
        tx={'actions':[{'account':'eosio','name':'devveto','authorization':[{'actor':bp,'permission':'active'}],'data':{'voter':bp,'proposal_id':current['proposal_id']}}]}
        result,receipt=send('veto-'+bp,tx);assert bp in f.governance()['effective_vetoes']
        entry={'voter':bp,'authorization':bp+'@active','transaction_id':result['transaction_id'],'canonical_receipt':receipt};save(path,entry);receipts.append(entry)
        print('IRREVERSIBLE VETO '+bp+' '+result['transaction_id'],flush=True)
    after=f.governance();assert set(after['effective_vetoes'])==set(after['effective_proposal_voters'])==set(frozen)
    assert after['effective_veto_count']==24 and after['required_vetoes']==16
    assert f.rpc('get_code_hash',{'account_name':'eosio.token'})['code_hash']==OLD
    assert f.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC']==stats
    save(K/'ALL_VETOES.json',{'status':'ALL_EFFECTIVE_BPS_VETOED_IRREVERSIBLY','proposal_id':after['proposal_id'],'receipts':receipts,'governance':after,'stats':stats,'excluded_existing_ban':'germanycrest'})
    print('ALL24 EFFECTIVE BP OWNER VETOES IRREVERSIBLE',flush=True)
else:
    proposal=json.loads((K/'PROPOSAL.json').read_text());votes=json.loads((K/'ALL_VETOES.json').read_text())
    assert votes['status']=='ALL_EFFECTIVE_BPS_VETOED_IRREVERSIBLY'and state['pending']and state['proposal_id']==proposal['governance']['proposal_id']
    for key,value in proposal['proposal'].items():assert state[key]==value
    assert set(state['effective_vetoes'])==set(state['effective_proposal_voters'])and state['effective_veto_count']>=state['required_vetoes']>0
    before=f.rpc('get_info');assert datetime.datetime.fromisoformat(before['head_block_time'])>=datetime.datetime.fromisoformat(state['execute_after'])
    signed_path=private/'attempt-signed.json';assert not signed_path.exists(),'Reconcile the saved attempt rather than preparing another one'
    tx={'actions':[{'account':'eosio','name':'devexec','authorization':[{'actor':state['developer'],'permission':'upgrade'}],'data':{'proposal_id':state['proposal_id'],'code':code.hex(),'abi':abi.hex()}}]}
    unsigned=private/'attempt-unsigned.json';save(unsigned,tx)
    signed=json.loads(f.run(['push','transaction',str(unsigned),'-j','-x','1800','--dont-broadcast','--compression','zlib']));save(signed_path,signed)
    packed=json.loads(f.run(['convert','pack_transaction',str(signed_path)]));transaction_id=sha(bytes.fromhex(packed['packed_trx']))
    save(K/'ATTEMPT_PROGRESS.json',{'status':'SIGNED_IMMUTABLE','transaction_id':transaction_id,'packet_sha256':sha(signed_path.read_bytes()),'start_LIB':before['last_irreversible_block_num']})
    p=subprocess.run([str(f.cleos),'--no-auto-keosd','--url',WRITER,'--wallet-url','unix://'+str(f.root/'wallet/keosd.sock'),'push','transaction',str(signed_path),'-s','-j','--compression','zlib'],capture_output=True,text=True,timeout=90)
    output=f.redact(p.stdout+p.stderr);(K/'DEVEXEC_REJECTION.log').write_text(output)
    assert p.returncode!=0 and 'upgrade vetoed by at least two thirds of the effective frozen electorate'in output,'Unexpected result: stop and reconcile exact packet/state'
    end=f.wait_lib(before['head_block_num']+2);scanned=[]
    for number in range(before['last_irreversible_block_num']+1,end['last_irreversible_block_num']+1):
        block=f.rpc('get_block',{'block_num_or_id':number});assert not any((r['trx']if isinstance(r['trx'],str)else r['trx']['id'])==transaction_id for r in block['transactions']);scanned.append({'block_num':number,'block_id':block['id']})
    after=f.governance();actual=f.rpc('get_raw_abi',{'account_name':'eosio.token'});after_stats=f.rpc('get_currency_stats',{'code':'eosio.token','symbol':'AIC'})['AIC']
    assert after==state and actual['code_hash']==OLD and actual['abi_hash']==ABI and after_stats==stats
    save(K/'VETO_REJECTION_RESULT.json',{'status':'PASS_NATIVE_VETO_REJECTION','chain_id':CHAIN,'proposal_id':state['proposal_id'],'attempted_transaction_id':transaction_id,'native_error':'upgrade vetoed by at least two thirds of the effective frozen electorate',
      'attempt_chain_time':before['head_block_time'],'execute_after':state['execute_after'],'review_elapsed_seconds':(datetime.datetime.fromisoformat(before['head_block_time'])-datetime.datetime.fromisoformat(state['proposed_at'])).total_seconds(),
      'effective_vetoes':state['effective_veto_count'],'effective_voters':state['effective_voter_count'],'threshold':state['required_vetoes'],'canonical_absence':scanned,'code_hash':OLD,'ABI_hash':ABI,'stats':after_stats,'proposal_remains_vetoed':True,'pending':after['pending']})
    print('PASS: exact mature payload rejected by native unanimous-veto guard; cap/supply/code unchanged',flush=True)
