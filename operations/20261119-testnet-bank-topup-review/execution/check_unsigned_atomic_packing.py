from pathlib import Path
import sys,json,subprocess,hashlib
R=Path('/opt/pureeos-testnet-bank-issuance-20261119-r1');sys.path.insert(0,str(R));import bank_upgrade as b
p=R/'private/unsigned-packing-check';assert not p.exists();p.mkdir(mode=0o700)
g=b.rpc('get_upgrade_state');b.verify_proposal(g,b.OLD,b.NEW,6)
before={'token':b.token_pin(),'stats':b.stats(),'bank':b.bank(),'proposal_id':g['proposal_id'],'pending':g['pending']}
mint={'account':'eosio.token','name':'testtopup','authorization':[{'actor':'eosio.dev','permission':'active'},{'actor':'civicbankers','permission':'active'}],'data':''}
actions=[b.execute_action(6,'candidate'),mint,*b.proposal_actions(b.NEW,b.OLD)];(p/'intent.json').write_text(json.dumps({'actions':actions}))
cmd=[str(b.CLEOS),'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(p/'not-a-wallet.sock'),'push','transaction',str(p/'intent.json'),'-j','-x','300','--dont-broadcast','--skip-sign','--compression','zlib']
q=subprocess.run(cmd,capture_output=True,text=True,timeout=45);(p/'stdout.json').write_text(q.stdout);(p/'stderr.log').write_text(q.stderr);assert q.returncode==0,'Unsigned packing refusal: private diagnostics retained'
u=json.loads(q.stdout);assert not u.get('signatures');assert[x['name']for x in u['actions']]==['devexec','testtopup','refreshgov','devpropose'];assert u['actions'][1]['data']in['',{}]and u['actions'][1].get('hex_data','')==''
g2=b.rpc('get_upgrade_state');after={'token':b.token_pin(),'stats':b.stats(),'bank':b.bank(),'proposal_id':g2['proposal_id'],'pending':g2['pending']};assert before==after
r={'status':'PASS_COLD_ORIGINAL_ABI_UNSIGNED_ATOMIC_PACKING','utc':b.now(),'action_names':[x['name']for x in u['actions']],'mint_action_empty_wire_data':True,'unsigned_output_sha256':hashlib.sha256(q.stdout.encode()).hexdigest(),'signatures':0,'broadcasts':0,'wallet_socket_deliberately_nonexistent':True,'state_before_after_exact':before};b.save('UNSIGNED_ATOMIC_PACKING_RESULT.json',r);print(json.dumps(r))
