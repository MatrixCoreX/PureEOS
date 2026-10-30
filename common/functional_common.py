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
  assert time.monotonic()<deadline,'Functional transaction not finalized on all participating nodes within the original 60s budget'
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
import uuid,socket,contextlib
def helper(request):
 ident=str(time.time_ns())+'-'+uuid.uuid4().hex[:8];p=B/'signing'/(ident+'.request');tmp=p.with_suffix('.pending')
 tmp.write_text(json.dumps(request));tmp.chmod(0o600);os.chown(tmp,1000,1000);tmp.replace(p)
 out=p.with_suffix('.response');limit=time.monotonic()+30
 while not out.exists():
  assert time.monotonic()<limit,'Signing/packet helper timed out'
  time.sleep(.04)
 response=json.loads(out.read_text());assert response['ok'],response.get('error')
 return response['result']
def action(code,name,actor,data):
 actors=actor if isinstance(actor,list)else [actor]
 return dict(account=code,name=name,authorization=[dict(actor=x,permission='active')for x in actors],data=data)
def signed(acts,signers=None):
 info=rpc(N[0],'get_info');abis={code:rpc(N[0],'get_abi',dict(account_name=code))['abi']for code in {a['account']for a in acts if 'hex'not in a}}
 if signers is None:signers=[dict(account=x)for x in sorted({p['actor']for a in acts for p in a['authorization']})]
 req=dict(mode='sign',chain_id=T['chain_id'],ref_block_id=info['last_irreversible_block_id'],expiration=time.strftime('%Y-%m-%dT%H:%M:%S',time.gmtime(time.time()+180)),actions=acts,abis=abis,signers=signers)
 return helper(req)
def send_fresh(acts,node_index=0,signers=None,expected_error=False):
 cmd=signed(acts,signers);label='fresh-'+str(time.time_ns());COMMANDS[label]=cmd
 if ACTIVE:write(Path(ACTIVE['evidence'])/('submitted-'+label+'.json'),cmd)
 result=send(label,node_index,expected_error)
 if ACTIVE:
  ACTIVE['expected_rejections'if expected_error else'successful_business_transactions'].append(result if expected_error else cmd['id'])
  write(Path(ACTIVE['evidence'])/'result.json',ACTIVE)
 return result
def record_status(cid,status,evidence,note=''):
 text=PLAN.read_text();lines=text.splitlines()
 for i,line in enumerate(lines):
  if line.startswith('| '+cid+' |'):
   parts=line.split('|');parts[4]=' '+status+' ';parts[5]=' '+str(evidence)+('；'+note if note else'')+' ';lines[i]='|'.join(parts)
 PLAN.write_text('\n'.join(lines)+'\n')
 p=B/'input/FUNCTIONAL_CASES.json';catalog=json.loads(p.read_text())
 for case in catalog['cases']:
  if case['id']==cid:
   case['status']=status
   if not any(x.get('evidence')==str(evidence)and x.get('status')==status for x in case['attempts']):case['attempts'].append(dict(evidence=str(evidence),status=status,note=note))
 write(p,catalog)
_base_begin=begin
def begin(cid,round_name):
 global ACTIVE
 parent=B/'rounds'/round_name/cid;attempt=1
 while(parent/('attempt-'+str(attempt))).exists():attempt+=1
 D=parent/('attempt-'+str(attempt));D.mkdir(parents=True)
 ACTIVE=dict(case_id=cid,round=round_name,attempt=attempt,status='RUNNING',chain_id=T['chain_id'],source_commit=T['source_commit'],nodeos_sha256=T['nodeos_sha256'],pressure_load=False,start_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),evidence=str(D),successful_business_transactions=[],expected_rejections=[],unexpected_failures=[],unconfirmed_transactions=[])
 write(D/'result.json',ACTIVE);record_status(cid,'RUNNING',D);save(dict(status='running-functional-case',case_id=cid,round=round_name,pressure_load=False))
 return D
_base_finish=finish
def finish(cid,observed):
 global ACTIVE
 evidence=ACTIVE['evidence'];_base_finish(cid,observed);record_status(cid,'PASS',evidence);save(dict(status='functional-case-completed',last_completed_case_id=cid,evidence=evidence,pressure_load=False))
def failed(e):
 global ACTIVE
 if ACTIVE:
  ACTIVE.update(status='FAIL',unexpected_failures=[dict(error=str(e),traceback=traceback.format_exc())])
  write(Path(ACTIVE['evidence'])/'result.json',ACTIVE);record_status(ACTIVE['case_id'],'FAIL',ACTIVE['evidence'],str(e)[:200])
 save(dict(status='functional-case-stopped-on-preserved-error',case_id=ACTIVE['case_id']if ACTIVE else None,error=str(e),traceback=traceback.format_exc(),pressure_load=False))
def peer_request(ports,index=0):
 info=rpc(N[index],'get_info');connections=rpc(N[index],'connections',ns='net');version=max(x.get('last_handshake',{}).get('network_version',0)for x in connections)
 return dict(mode='wire',network_version=version,chain_id=T['chain_id'],nonce=uuid.uuid4().hex,root_num=info['last_irreversible_block_num'],root_id=info['last_irreversible_block_id'],head_num=info['head_block_num'],head_id=info['head_block_id'],peers=ports)
def framed_read(sock,deadline):
 def exact(n):
  out=b''
  while len(out)<n:
   sock.settimeout(max(.05,deadline-time.monotonic()));v=sock.recv(n-len(out))
   if not v:raise EOFError()
   out+=v
  return out
 size=struct.unpack('<I',exact(4))[0];assert size<=67108864
 return exact(size)
def peer_record(account='bpbbb',**kw):
 d=dict(account=account,role='peer-v1',endpoint=next(n['ip']for n in N if n['account']==account)+':9876',outbound_ip=next(n['ip']for n in N if n['account']==account),expiration=time.strftime('%Y-%m-%dT%H:%M:%S',time.gmtime(time.time()+3600)))
 d.update(kw);return d
def wire_probe(peers,expect_valid,index=0,gossip_override=None):
 request=peer_request(peers,index);frames=helper(request)
 if gossip_override is not None:frames['gossip']=gossip_override
 history=[];src=None;states=[]
 with socket.create_connection(('127.0.0.1',9876),timeout=5)as s:
  src=s.getsockname()[1];s.sendall(bytes.fromhex(frames['handshake']['hex']))
  # Observe the unauthenticated initial and drain it before sending proof.
  until=time.monotonic()+.8
  while time.monotonic()<until:
   try:
    msg=framed_read(s,until)
    if msg[0]in[0,2,12]:history.append(dict(phase='before-proof',decoded=helper(dict(mode='decode',hex=msg.hex()))))
   except socket.timeout:break
  s.sendall(bytes.fromhex(frames['gossip']['hex']))
  until=time.monotonic()+1.8;closed=False
  while time.monotonic()<until:
   try:
    msg=framed_read(s,until)
    if msg[0]in[0,2,12]:history.append(dict(phase='after-proof',decoded=helper(dict(mode='decode',hex=msg.hex()))))
   except socket.timeout:break
   except EOFError:closed=True;break
  conns=rpc(N[0],'connections',ns='net')
  own=[x for x in conns if x.get('remote_ip')in ['127.0.0.1','::ffff:127.0.0.1']and int(x.get('remote_port',0))==src]
  privileged=any(x.get('is_bp_peer')or x.get('is_bp_gossip_peer')for x in own)
  full=[x for x in history if x['phase']=='after-proof'and x['decoded']['message_index']==12 and any(p.get('bp_peer_info')for p in x['decoded']['value'].get('peers',[]))]
  if expect_valid:assert privileged,'Valid gossip must confer verified gossip peer status'
  else:assert not privileged,'Invalid/revoked proof conferred BP privilege'
 result=dict(request=request,frames=frames,source_port=src,history=history,connection_snapshot=own,privileged=privileged,full_responses=len(full),closed=closed,gossip_after=rpc(N[0],'bp_gossip_peers',ns='net'))
 if ACTIVE:write(Path(ACTIVE['evidence'])/('wire-'+str(time.time_ns())+'.json'),result)
 return result
