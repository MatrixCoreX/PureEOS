"""Exact TEST upgrade only. Reconcile every immutable signed packet before resume."""
from pathlib import Path
import datetime,hashlib,json,os,subprocess,sys
os.umask(0o077)
K=Path('/opt/pureeos-relay-execution-income-20261105-r1');Q=K/'attempt-3';R=Path('/opt/pureeos-devnet-20261105-r1')
os.environ['PUREEOS_GENESIS_ROOT']=str(R);sys.path.insert(0,str(R))
action=sys.argv[1];assert action in ['propose','execute']
for p in [Q/'qualification/RESULT.json',K/'RUNTIME_TEST_RESULT.json',K/'FRONTEND_RESULT_r3.json']:
 assert json.loads(p.read_text())['status']=='PASS',str(p)
review_bytes=(K/'REVIEW.json').read_bytes();review=json.loads(review_bytes)
assert json.loads((K/'GIT_REVIEW_PUBLISHED.json').read_text())['status']=='PASS'
# The prior task intentionally stopped only this founder signing wallet.
# Wait for its existing Unix socket before load_chain unlocks it.
import socket,time
subprocess.run(['systemctl','start','pureeos-devnet-wallet-20261105.service'],check=True)
deadline=time.monotonic()+60
while True:
 sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
 try:
  sock.settimeout(1);sock.connect(str(R/'founder/wallet/keosd.sock'));break
 except OSError:
  assert time.monotonic()<deadline,'Founder wallet socket did not become ready';time.sleep(.25)
 finally:sock.close()
from campaign_helpers import load_chain
f=load_chain()
CHAIN='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
OLD='7523bbec364316d06f48283e9cc17cf81fa929bd60b4efd818eadd73e215c788'
NEW='1092b97ede0d06ab7cc7b54ac0fe5c6a47227e8356e3b7a44877d6a47bd6273d'
ABI='b6db52cd47fdf811bee83030e524de35d0b6f55aece386392b56d1a085df2f83'
sha=lambda x:hashlib.sha256(x).hexdigest()
code=(Q/'artifacts/eosio.key.wasm').read_bytes();abi=(K/'candidate.abi.bin').read_bytes()
assert sha(code)==NEW and sha(abi)==ABI
assert review['new_code_hash']==NEW and review['raw_ABI_hash']==ABI
assert f.rpc('get_info')['chain_id']==CHAIN
state=f.governance();assert state['sealed']and state['review_seconds']==1800 and state['developer']=='eosio.dev'
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
            receipt=f.wait_transaction(progress['transaction_id'],before['last_irreversible_block_num'],timeout=600);progress['status']='IRREVERSIBLE';save(progress_path,progress)
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
