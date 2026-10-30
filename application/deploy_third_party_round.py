from functional_30_common import *
Q=C/'third-party-live';Q.mkdir(exist_ok=True)
def same_rows(code,scope,table):
 out={n['account']:f.rpc(n,'get_table_rows',dict(code=code,scope=scope,table=table,json=True,limit=1000))for n in nodes}
 assert len({json.dumps(v,sort_keys=True)for v in out.values()})==1,(code,scope,table,'table divergence')
 return out[nodes[0]['account']]['rows']
def tx(code,name,actor,data,expected=False):
 return f.send_fresh([f.action(code,name,actor,data)],expected_error=expected)
def owned_permission(account,key,extra=None):
 return dict(threshold=1,keys=[dict(key=key,weight=1)],accounts=extra or[],waits=[])
try:
 gate=C/'NETWORK_FUNCTIONAL_PASSED.json';limit=time.monotonic()+1800
 while not gate.exists():
  assert time.monotonic()<limit,'Network round did not finish'
  if(C/'ACTIVATION_STATUS.json').exists():assert read(C/'ACTIVATION_STATUS.json')['status']!='network-fault-round-stopped-on-preserved-error','Network round failed; deployment untouched'
  time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.N=nodes;f.T=topology;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  D=f.begin('DEF-01','R09')
  accounts=['atomicassets','atomicmarket','bancortokena','bancorconvtr','bancornetwrk']
  keys=f.helper(dict(mode='keys',accounts=[dict(account=a,role=r)for a in accounts for r in ['owner','active']]))
  keys={(x['account'],x['role']):x['public_key']for x in keys}
  deployments=[];build=read(B/'third-party-build/STATUS.json');assert build['status']=='all-pinned-contracts-built'
  base=Path(build['evidence'].replace('/campaign',str(B),1))
  specs=[('atomicassets','atomicassets'),('atomicmarket','atomicmarket'),('bancortokena','Token'),('bancorconvtr','BancorConverter'),('bancornetwrk','BancorNetwork')]
  for account,contract in specs:
   try:exists=f.rpc(nodes[0],'get_account',dict(account_name=account))
   except RuntimeError:exists=None
   if not exists:
    create=f.action('eosio','newaccount','developer',dict(creator='developer',name=account,owner=owned_permission(account,keys[account,'owner']),active=owned_permission(account,keys[account,'active'])))
    ram=f.action('eosio','buyram','developer',dict(payer='developer',receiver=account,quant='100.0000 SYS'))
    f.send_fresh([create,ram])
    # Account-owned stake and vote, ordinarily authorized; setup EOSIO sponsorship is explicit.
    acts=[
     f.action('eosio.token','open',['developer',account],dict(owner=account,symbol='4,SYS',ram_payer=account)),
     f.action('eosio.token','transfer','developer',{'from':'developer','to':account,'quantity':'1000.0000 SYS','memo':'third-party normal test setup'}),
     f.action('eosio','delegatebw',['developer',account],{'from':account,'receiver':account,'stake_net_quantity':'100.0000 SYS','stake_cpu_quantity':'100.0000 SYS','transfer':False}),
     f.action('eosio','voteproducer',['developer',account],dict(voter=account,proxy='',producers=[nodes[0]['account']]))]
    f.send_fresh(acts)
  # Every authority target exists before adding cross-contract eosio.code grants.
  for account,contract in specs:
   code_permissions=[dict(permission=dict(actor=account,permission='eosio.code'),weight=1)]
   if account=='bancortokena':code_permissions.append(dict(permission=dict(actor='bancorconvtr',permission='eosio.code'),weight=1))
   code_permissions.sort(key=lambda x:(x['permission']['actor'],x['permission']['permission']))
   required=owned_permission(account,keys[account,'active'],code_permissions)
   current=next(x['required_auth']for x in f.rpc(nodes[0],'get_account',dict(account_name=account))['permissions']if x['perm_name']=='active')
   if current!=required:
    update=f.action('eosio','updateauth',account,dict(account=account,permission='active',parent='owner',auth=required))
    update['authorization']=[dict(actor=account,permission='owner')]
    f.send_fresh([update],signers=[dict(account=account,role='owner')])
  for account,contract in specs:
   spec=next(x for x in build['contracts']if x['contract']==contract)
   wasm=base/contract/(contract+'.wasm');abi=base/contract/(contract+'.abi')
   assert hashlib.sha256(wasm.read_bytes()).hexdigest()==spec['wasm_sha256']
   packed=f.helper(dict(mode='pack_abi',abi=read(abi)))['hex']
   setcode=f.action('eosio','setcode',account,dict(account=account,vmtype=0,vmversion=0,code=wasm.read_bytes().hex()))
   setabi=f.action('eosio','setabi',account,dict(account=account,abi=packed))
   prior=f.rpc(nodes[0],'get_code_hash',dict(account_name=account))['code_hash']
   receipt=f.send_fresh([setcode,setabi])if prior!=spec['wasm_sha256']else dict(reused_initial_deployment=True,prior_case_evidence=str(B/'rounds/R09/DEF-01/attempt-2'))

   hashes={n['account']:f.rpc(n,'get_code_hash',dict(account_name=account))['code_hash']for n in nodes}
   assert set(hashes.values())=={spec['wasm_sha256']}
   deployed_abis={n['account']:f.rpc(n,'get_abi',dict(account_name=account))['abi']for n in nodes}
   assert len({json.dumps(v,sort_keys=True)for v in deployed_abis.values()})==1
   deployments.append(dict(account=account,contract=contract,wasm_sha256=spec['wasm_sha256'],abi_sha256=spec['abi_sha256'],receipt=receipt,all30_code_hashes=hashes))
   f.write(Q/'DEPLOYMENTS.json',dict(status='deploying-pinned-third-party-contracts',contracts=deployments,chain_id=topology['chain_id']))
  for account in ['atomicassets','atomicmarket']:
   if not same_rows(account,account,'config'):tx(account,'init',account,{})

  f.finish('DEF-01',dict(deployments=deployments,provenance=read(B/'third-party-sources/PROVENANCE.json'),real_pinned_contracts=True,setup_outside_timing=True,scope='Deploy and initialize five real pinned contracts on full21/30-node chain; DeFi pool business and NFT use cases pending'))
  f.write(Q/'DEPLOYED.json',dict(status='PASS',chain_id=topology['chain_id'],contracts=deployments,all30_verified=True,pressure_load=False))
except BaseException as e:
 f.failed(e);f.write(Q/'FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
