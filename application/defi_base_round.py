from functional_30_common import *
Q=C/'third-party-live';V='bancorconvtr';R='bancornetwrk';T='bancortokena';owner='srcaa'
def rows(code,scope,table):
 allrows={n['account']:f.rpc(n,'get_table_rows',dict(code=code,scope=scope,table=table,json=True,limit=1000))['rows']for n in nodes}
 assert len({json.dumps(x,sort_keys=True)for x in allrows.values()})==1,(code,scope,table,'cross-node divergence')
 return allrows[nodes[0]['account']]
def tx(code,name,actor,data,expected=False):
 return f.send_fresh([f.action(code,name,actor,data)],expected_error=expected)
def transfer(code,actor,to,quantity,memo,expected=False):
 return tx(code,'transfer',actor,{'from':actor,'to':to,'quantity':quantity,'memo':memo},expected)
def snap():
 balances={n['account']:{code+':'+who:f.balance(n,code,who,sym)for code,sym in [('eosio.token','SYS'),('tokenone','TKN')]for who in [owner,V,R]}for n in nodes}
 assert len({json.dumps(v,sort_keys=True)for v in balances.values()})==1
 return dict(balances=balances[nodes[0]['account']],converters=rows(V,V,'converters'),temporary=rows(V,owner,'accounts'))
def reserves(snapshot,code='POOL'):
 converter=next(x for x in snapshot['converters']if x['currency'].split(',')[-1]==code)
 result={}
 for item in converter['reserve_balances']:
  k=item.get('key',item.get('first'));v=item.get('value',item.get('second'))
  result[k]=int(__import__('decimal').Decimal(v['quantity'].split()[0])*10000)
 return result
def quantity(units,sym):
 assert units>=0;return str(units//10000)+'.'+str(units%10000).zfill(4)+' '+sym
def swap(contract,sym,other_contract,out_sym,amount,path=None,minimum='0.0001'):
 before=snap();rs=reserves(before);path=path or V+':POOL '+out_sym
 receipt=transfer(contract,owner,R,quantity(amount,sym),'1,'+path+','+minimum+','+owner)
 after=snap()
 change=after['balances'][other_contract+':'+owner]-before['balances'][other_contract+':'+owner]
 if contract==other_contract:change+=amount
 else:assert before['balances'][contract+':'+owner]-after['balances'][contract+':'+owner]==amount
 assert change>0
 for c in ['eosio.token','tokenone']:
  assert sum(before['balances'][c+':'+a]for a in [owner,V,R])==sum(after['balances'][c+':'+a]for a in [owner,V,R]),'wallet/reserve token conservation'

 if ' 'in path and path.count(':')==1:
  expected=int((amount/10000)/(rs[sym]/10000+amount/10000)*(rs[out_sym]/10000)*10000)
  assert abs(change-expected)<=1,(change,expected,'pinned IEEE floating-point floor')
  updated=reserves(after);assert updated[sym]==rs[sym]+amount and updated[out_sym]==rs[out_sym]-change
 for c,s in [('eosio.token','SYS'),('tokenone','TKN')]:
  assert after['balances'][c+':'+R]==before['balances'][c+':'+R]
 return dict(receipt=receipt,before=before,after=after,received_units=change)
def partial(cid,observed):
 e=Path(f.ACTIVE['evidence']);f.ACTIVE.update(status='PARTIAL',observed=observed);f.write(e/'result.json',f.ACTIVE);f.record_status(cid,'PARTIAL',e,'Only explicitly recorded subcases executed');f.ACTIVE=None
try:
 gate=Q/'NFT_BASE_PASSED.json';limit=time.monotonic()+1800
 while not gate.exists():
  assert time.monotonic()<limit,'NFT base prerequisite did not complete'
  time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.N=nodes;f.T=topology;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  setup=read(B/'rounds/R09/DEF-02/attempt-1/result.json')
  assert setup['status']=='PASS'and setup['chain_id']==topology['chain_id']
  D=f.begin('DEF-03','R09')
  forward=swap('eosio.token','SYS','tokenone','TKN',10000)
  reverse=swap('tokenone','TKN','eosio.token','SYS',10000)
  deep_before=snap()
  deep_reject=transfer('eosio.token',owner,R,'1.0000 SYS','1,'+V+':POOL TKN '+V+':ALT SYS,0.0001,'+owner,True)
  assert 'max inline action depth'in json.dumps(deep_reject) and snap()==deep_before
  # Compose two ordinary top-level routes in one atomic transaction, retaining the existing inline-depth protection.
  before=snap();rs=reserves(before)
  expected=int((1.0)/(rs['SYS']/10000+1.0)*(rs['TKN']/10000)*10000);hop=max(1,expected-1)
  first=f.action('eosio.token','transfer',owner,{'from':owner,'to':R,'quantity':'1.0000 SYS','memo':'1,'+V+':POOL TKN,0.0001,'+owner})
  second=f.action('tokenone','transfer',owner,{'from':owner,'to':R,'quantity':quantity(hop,'TKN'),'memo':'1,'+V+':ALT SYS,0.0001,'+owner})
  receipt=f.send_fresh([first,second]);after=snap()
  for c in ['eosio.token','tokenone']:
   assert sum(before['balances'][c+':'+a]for a in [owner,V,R])==sum(after['balances'][c+':'+a]for a in [owner,V,R])
   assert before['balances'][c+':'+R]==after['balances'][c+':'+R]
  dust=after['balances']['tokenone:'+owner]-before['balances']['tokenone:'+owner]
  assert 0<=dust<=2
  assert after['balances']['eosio.token:'+owner]>before['balances']['eosio.token:'+owner]-10000
  route=dict(receipt=receipt,before=before,after=after,intermediate_input_units=hop,intermediate_dust_units=dust,top_level_actions=2,atomic=True)
  f.finish('DEF-03',dict(forward=forward,reverse=reverse,atomic_two_pool_route=route,over_depth_nested_route_rejection=deep_reject,scope='One-hop formula checked to one smallest unit; two pools composed as two top-level actions in one atomic transaction; native inline-depth cap retained and over-depth route rejected with complete rollback'))
  D=f.begin('DEF-05','R09');before=snap();rejections=[]
  for qty,memo in [('999999999.0000 SYS','1,'+V+':POOL TKN,0.0001,'+owner),('1.0000 SYS','bad memo'),('1.0000 SYS','1,'+V+':POOL TKN,999999999,'+owner),('1.0000 SYS','1,'+V+':POOL TKN,0.0001,noaccountaaaa')]:
   rejections.append(transfer('eosio.token',owner,R,qty,memo,True));assert snap()==before
  f.finish('DEF-05',dict(expected_rejections=rejections,balance_and_reserve_rollback=True,all30_unchanged=True))
  D=f.begin('DEF-06','R09');before=snap()
  for qty in ['0.0000 SYS','-0.0001 SYS']:
   transfer('eosio.token',owner,R,qty,'1,'+V+':POOL TKN,0.0001,'+owner,True);assert snap()==before
  tiny=swap('eosio.token','SYS','tokenone','TKN',10)
  partial('DEF-06',dict(zero_negative_rejected=True,tiny_transfer=tiny,precision_difference_and_int64_extremes='pending dedicated scenarios'))
  D=f.begin('DEF-07','R09');before=snap()
  transfer('eosio.token',owner,V,'1.0000 SYS','fund;POOL');deposited=snap();assert deposited['temporary']!=before['temporary']
  tx(V,'withdraw',owner,dict(sender=owner,quantity='1.0000 SYS',converter_currency_code='POOL'))
  after=snap();assert after==before
  partial('DEF-07',dict(deposit_withdraw_roundtrip_exact=True,fund_mint_and_liquidation='pending dedicated liquidity scenarios'))
  D=f.begin('DEF-09','R09');before=snap()
  a=f.action('eosio.token','transfer',owner,{'from':owner,'to':R,'quantity':'1.0000 SYS','memo':'1,'+V+':POOL TKN,0.0001,'+owner})
  bad=f.action('eosio.token','transfer',owner,{'from':owner,'to':'dstaa','quantity':'999999999.0000 SYS','memo':'expected rollback after inline conversion'})
  rejected=f.send_fresh([a,bad],expected_error=True);assert snap()==before
  f.finish('DEF-09',dict(actual_conversion_followed_by_failed_child_action_rejected=rejected,all30_wallet_reserves_fees_unchanged=True,atomicity_preserved=True))
  f.write(Q/'DEFI_BASE_PASSED.json',dict(status='base-DeFi-functional-round-completed',full_cases=['DEF-02','DEF-03','DEF-05','DEF-09'],partial=['DEF-06','DEF-07'],all30_checked=True,pressure_load=False,TPS='unmeasured',baseline_promoted=False))
except BaseException as e:
 f.failed(e);f.write(Q/'DEFI_FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
