from functional_30_common import *
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  fixture=read(C/'fixture/PUBLIC_FIXTURE.json')
  save(dict(status='six-low-frequency-transfer-finality-and-balance-checks-on-all-thirty',pressure_load=False,running_nodes=30))
  business=[]
  for i in range(6):
   contract='eosio.token'if i%2==0 else'tokenone';symbol='SYS'if i%2==0 else'TKN';source=fixture['business_accounts'][2*i];dest=fixture['business_accounts'][2*i+1]
   before={n['account']:[f.balance(n,contract,source,symbol),f.balance(n,contract,dest,symbol)]for n in nodes}
   assert len({tuple(v)for v in before.values()})==1
   tx=f.send_fresh([f.action(contract,'transfer',source,{'from':source,'to':dest,'quantity':'0.0001 '+symbol,'memo':'thirty-node-functional-'+str(i)+'-'+str(time.time_ns())})],node_index=i)
   after={n['account']:[f.balance(n,contract,source,symbol),f.balance(n,contract,dest,symbol)]for n in nodes}
   assert len({tuple(v)for v in after.values()})==1
   assert all(after[k]==[v[0]-1,v[1]+1]for k,v in before.items())
   business.append(dict(transaction=tx,before=before,after=after,contract=contract));f.write(C/'SIX_LOW_FREQUENCY_TRANSFERS.json',business)
  f.write(C/'ACTIVATED.json',dict(status='thirty-independent-parallel-node-functional-discovery-active',source_commit=topology['source_commit'],nodeos_sha256=topology['nodeos_sha256'],chain_id=topology['chain_id'],stages=[3,10,21,30],low_frequency_transfers=6,all_thirty_finalized_and_balances_match=True,pressure_load=False,TPS='unmeasured',full_suite_complete=False,baseline_promoted=False))
  f.PLAN.write_text(f.PLAN.read_text()+'\n30实例低频业务补充：6笔SYS/TKN转账逐笔在全部30实例最终确认，所有源/目标余额增减逐笔一致。恢复控制器曾遗漏fixture变量，保留其NameError；仅修复测试控制器引用，节点源码、预算和断言未改。证据：'+str(C/'SIX_LOW_FREQUENCY_TRANSFERS.json')+'。后续继续R04角色/滚动重启、网络故障、黑名单与第三方合约功能。\n')
  save(dict(status='thirty-node-discovery-and-low-frequency-business-cases-passed-next-dropout-blacklist-and-third-party-rounds-pending',running_nodes=30,pressure_load=False,baseline_promoted=False))
except BaseException as e:
 f.failed(e);save(dict(status='thirty-node-business-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),pressure_load=False,baseline_promoted=False));raise
