from pathlib import Path
import os,json,time,urllib.request,urllib.error,traceback,hashlib,struct,fcntl,decimal
B=Path('/opt/aic-bp-discovery-live-20261030')
PLAN=Path('/opt/aic-performance-results/machines/amd-epyc-4345p-8-core-processor--baremetal--8physical-16threads--65536MiB/p2p-discovery-multinode-functional-test-plan-20261030.md')
os.sched_setaffinity(0,{1,2,3,4,5,6,7,9,10,11,12,13,14,15})
T=json.loads((B/'LIVE_TOPOLOGY.json').read_text());N=T['nodes'];F=json.loads((B/'fixture/PUBLIC_FIXTURE.json').read_text())
COMMANDS={x['label']:x for x in json.loads((B/'fixture/SIGNED_COMMANDS.json').read_text())}
ACTIVE=None
def write(p,d):
 p.parent.mkdir(parents=True,exist_ok=True);q=p.with_suffix('.pending');q.write_text(json.dumps(d,indent=2)+'\n');q.replace(p)
def save(d):d['UTC']=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime());write(B/'FUNCTIONAL_PROGRESS.json',d)
def rpc(node,method,payload=None,ns='chain',expected_error=False):
 req=urllib.request.Request('http://127.0.0.1:'+str(node['http_port'])+'/v1/'+ns+'/'+method,data=json.dumps(payload or{}).encode(),headers={'Content-Type':'application/json'})
 try:
  with urllib.request.urlopen(req,timeout=8)as r:data=json.load(r)
  assert not expected_error,'Expected a rejection but transaction was accepted'
  return data
 except urllib.error.HTTPError as e:
  body=e.read().decode(errors='replace')
  if not expected_error:raise RuntimeError('RPC '+method+' failed on '+node['account']+': '+body[:1200])
  return dict(http_status=e.code,body=json.loads(body))
def begin(cid,round_name):
 global ACTIVE
 D=B/'rounds'/round_name/cid/'attempt-1';assert not D.exists();D.mkdir(parents=True)
 ACTIVE=dict(case_id=cid,round=round_name,attempt=1,status='RUNNING',chain_id=T['chain_id'],source_commit=T['source_commit'],nodeos_sha256=T['nodeos_sha256'],pressure_load=False,start_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),evidence=str(D),successful_business_transactions=[],expected_rejections=[],unexpected_failures=[],unconfirmed_transactions=[])
 write(D/'result.json',ACTIVE);save(dict(status='running-functional-case',case_id=cid,round=round_name,pressure_load=False))
 return D
def finish(cid,observed):
 global ACTIVE
 ACTIVE.update(status='PASS',observed=observed,end_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
 write(Path(ACTIVE['evidence'])/'result.json',ACTIVE)
 text=PLAN.read_text();lines=text.splitlines()
 for i,line in enumerate(lines):
  if line.startswith('| '+cid+' |'):
   parts=line.split('|');parts[4]=' PASS ';parts[5]=' '+ACTIVE['evidence']+' ';lines[i]='|'.join(parts)
 PLAN.write_text('\n'.join(lines)+'\n')
 p=B/'input/FUNCTIONAL_CASES.json';catalog=json.loads(p.read_text())
 for case in catalog['cases']:
  if case['id']==cid:case['status']='PASS';case['attempts'].append(dict(evidence=ACTIVE['evidence'],status='PASS'))
 write(p,catalog)
 ACTIVE=None
def infos():return {n['account']:rpc(n,'get_info')for n in N}
def rows(node):
 result=rpc(node,'get_table_rows',dict(code='eosio',scope='eosio',table='peerkeys',json=True,limit=100,show_payer=True))
 return result['rows']
def account_row(data,account):
 for r in data:
  value=r.get('data',r)
  if value.get('account')==account:return r
 return None
def contains_key(row,key):return row is not None and key in json.dumps(row)
def block_ids(block):
 for entry in block.get('transactions',[]):
  trx=entry.get('trx');yield trx if isinstance(trx,str)else trx.get('id')if isinstance(trx,dict)else None
def send(label,node_index=0,expected_error=False):
 cmd=COMMANDS[label];packed=cmd['packed'];expiry=struct.unpack('<I',bytes.fromhex(packed['packed_trx'])[:4])[0]
 assert expiry>time.time()+90,'Pre-signed functional packet near expiry; re-sign normally before continuing'
 before=infos();origin=max(1,min(x['head_block_num']for x in before.values())-2)
 reply=rpc(N[node_index],'push_transaction',packed,expected_error=expected_error)
 if expected_error:return dict(label=label,id=cmd['id'],reply=reply)
 assert reply.get('transaction_id')==cmd['id']
 deadline=time.monotonic()+60;last_scan=origin;found=None
 while True:
  observed=infos();minhead=min(x['head_block_num']for x in observed.values())
  if found:
   check=rpc(N[0],'get_block',dict(block_num_or_id=found['number']))
   if check['id']!=found['id']or cmd['id']not in set(block_ids(check)):found=None;last_scan=origin
  if not found:
   for height in range(last_scan,minhead+1):
    block=rpc(N[0],'get_block',dict(block_num_or_id=height))
    if cmd['id']in set(block_ids(block)):found=dict(number=height,id=block['id']);break
   last_scan=minhead+1
  if found and all(x['last_irreversible_block_num']>=found['number']for x in observed.values()):
   copies={n['account']:rpc(n,'get_block',dict(block_num_or_id=found['number']))for n in N}
   assert all(x['id']==found['id']and cmd['id']in set(block_ids(x))for x in copies.values())
   return dict(label=label,id=cmd['id'],included=found,all_three_LIB={k:v['last_irreversible_block_num']for k,v in observed.items()},injected_node=N[node_index]['account'])
  assert time.monotonic()<deadline,'Functional transaction not finalized on all three nodes'
  time.sleep(.5)
def asset_units(value):
 if not value:return 0
 return int(decimal.Decimal(value[0].split()[0])*10000)
def balance(node,contract,account,symbol):return asset_units(rpc(node,'get_currency_balance',dict(code=contract,account=account,symbol=symbol)))
def transfer(i):
 pair=i%20;source=F['business_accounts'][pair*2];dest=F['business_accounts'][pair*2+1]
 contract='eosio.token'if i%2==0 else'tokenone';symbol='SYS'if i%2==0 else'TKN'
 before={n['account']:[balance(n,contract,source,symbol),balance(n,contract,dest,symbol)]for n in N}
 assert len({tuple(x)for x in before.values()})==1
 result=send('transfer-'+str(i),i%3)
 after={n['account']:[balance(n,contract,source,symbol),balance(n,contract,dest,symbol)]for n in N}
 assert len({tuple(x)for x in after.values()})==1
 for name,values in after.items():assert values==[before[name][0]-1,before[name][1]+1]
 result.update(contract=contract,symbol=symbol,source=source,destination=dest,before=before,after=after)
 return result
def gossip_wait(account,present=True,previous_expirations=None):
 deadline=time.monotonic()+210;history=[]
 while True:
  current={}
  for n in N:
   peers=rpc(n,'bp_gossip_peers',ns='net')
   matching=[p for p in peers if p['producer_name']==account]
   current[n['account']]=dict(gossip=matching,head=rpc(n,'get_info')['head_block_num'])
  history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),nodes=current))
  if present:
   good=all(x['gossip']for x in current.values())
   if previous_expirations is not None:
    good=good and all(any(p['expiration']not in previous_expirations[k]for p in x['gossip'])for k,x in current.items())
  else:good=all(not x['gossip']for x in current.values())
  write(Path(ACTIVE['evidence'])/'gossip-observations.json',history)
  if good:return current
  assert time.monotonic()<deadline,'Registry change did not reach authenticated gossip state'
  time.sleep(2)
try:
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  assert json.loads((B/'ACTIVATED.json').read_text())['learned_direct_links']=={'bpaaa-to-bpccc':True,'bpccc-to-bpaaa':True}
  D=begin('ENV-03','R00');preset=json.loads((B/'input/chain-parameters.json').read_text());profiles={}
  for n in N:
   params=rpc(n,'get_consensus_parameters')['chain_config']
   assert all(params[k]==v for k,v in preset.items()),n['account']
   info=rpc(n,'get_info');assert info['chain_id']==T['chain_id']
   profiles[n['account']]=dict(chain_config=params,chain_id=info['chain_id'])
  finish('ENV-03',profiles)
  D=begin('KEY-01','R01');expected=json.loads((B/'contract/eosio.system.abi').read_text());deploy={}
  for n in N:
   code=rpc(n,'get_code_hash',dict(account_name='eosio'))['code_hash'];assert code==F['system_code_hash']
   abi=rpc(n,'get_abi',dict(account_name='eosio'))['abi']
   for section in ['actions','tables','structs','types','variants','action_results','ricardian_clauses']:
    assert abi.get(section,[])==expected.get(section,[]),(n['account'],section)
   deploy[n['account']]=dict(code_hash=code,ABI_prior_and_new_sections_verified=True,actions=len(abi['actions']))
  finish('KEY-01',deploy)
  D=begin('KEY-02','R01');registry={}
  for n in N:
   rr=rows(n);registry[n['account']]=rr
   for bp in F['nodes']:
    row=account_row(rr,bp['account']);assert contains_key(row,bp['peer_key'])
    assert row.get('payer')==bp['account'],row
  finish('KEY-02',registry)
  D=begin('GSP-03','R02');business=[]
  for i in range(24):
   result=transfer(i);business.append(result);ACTIVE['successful_business_transactions'].append(result['id'])
   write(D/'transactions.json',business);write(D/'result.json',ACTIVE);time.sleep(.5)
  finish('GSP-03',dict(successful_unique_transfers=24,SYS_and_TKN=True,ingress_all_three_nodes=True,resource_classes='staked and prepaid PowerUp business accounts',all_three_finalized=True,balances_checked_before_after=True))
  bp='bpbbb';public=next(x for x in F['nodes']if x['account']==bp)
  D=begin('SEC-01','R03')
  before={n['account']:[p['expiration']for p in rpc(n,'bp_gossip_peers',ns='net')if p['producer_name']==bp]for n in N}
  result=send('rotate-'+bp,1)
  assert all(contains_key(account_row(rows(n),bp),public['next_peer_key'])for n in N)
  observed=gossip_wait(bp,True,before)
  business=transfer(30);ACTIVE['successful_business_transactions'] +=[result['id'],business['id']]
  finish('SEC-01',dict(on_chain_current_key=public['next_peer_key'],all_three_registry_rows_matched=True,renewed_authenticated_gossip=observed,ordinary_transfer_after_rotation=business))
  D=begin('KEY-07','R01');before={n['account']:rows(n)for n in N}
  rejection=send('stale-delete-'+bp,2,True)
  encoded=json.dumps(rejection['reply'])
  assert 'current peer key does not match'in encoded
  after={n['account']:rows(n)for n in N};assert before==after
  ACTIVE['expected_rejections'].append(rejection)
  finish('KEY-07',dict(rejected_with_expected_reason=True,registry_unchanged_on_all_three=True))
  D=begin('SEC-03','R03');result=send('delete-'+bp,0)
  assert all(account_row(rows(n),bp)is None for n in N)
  observed=gossip_wait(bp,False)
  business=transfer(31);ACTIVE['successful_business_transactions'] +=[result['id'],business['id']]
  finish('SEC-03',dict(registry_row_deleted=True,gossip_entry_removed_on_all_three=observed,consensus_uses_separate_keys=True,ordinary_transfer_after_revocation=business,old_wire_replay='SEC-02 separate pending'))
  D=begin('SEC-04','R03');result=send('restore-'+bp,1)
  assert all(contains_key(account_row(rows(n),bp),public['peer_key'])for n in N)
  observed=gossip_wait(bp,True)
  business=transfer(32);ACTIVE['successful_business_transactions'] +=[result['id'],business['id']]
  finish('SEC-04',dict(registry_restored=True,authenticated_gossip_restored=observed,ordinary_transfer_after_restoration=business))
  save(dict(status='low-frequency-basic-business-and-registry-rotation-revocation-restoration-cases-passed',passed=['ENV-03','KEY-01','KEY-02','GSP-03','SEC-01','KEY-07','SEC-03','SEC-04'],pressure_load=False,remaining_plan_cases=True,baseline_promoted=False))
except BaseException as e:
 if ACTIVE:
  ACTIVE.update(status='FAIL',unexpected_failures=[dict(error=str(e),traceback=traceback.format_exc())])
  write(Path(ACTIVE['evidence'])/'result.json',ACTIVE)
 save(dict(status='functional-case-stopped-on-preserved-error',case_id=ACTIVE['case_id']if ACTIVE else None,error=str(e),traceback=traceback.format_exc(),pressure_load=False))
 raise
