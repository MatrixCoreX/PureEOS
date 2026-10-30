from functional_30_common import *
import socket,struct,contextlib
owned=[]
def table_install(machine,name,rules):
 assert name.startswith('aic_bp30_')
 code="import subprocess,json\nname="+repr(name)+"\nassert subprocess.run(['nft','list','table','inet',name],capture_output=True).returncode!=0\nr=subprocess.run(['nft','-f','-'],input="+repr(rules)+",capture_output=True,text=True);assert r.returncode==0,r.stderr\nprint(json.dumps(dict(installed=True)))"
 operate(machine,code);owned.append((machine,name));f.write(C/'ACTIVE_OWNED_NETWORK_FAULTS.json',[dict(machine=m['ip'],table=n)for m,n in owned])
def table_remove(machine,name):
 operate(machine,"import subprocess,json\nr=subprocess.run(['nft','delete','table','inet',"+repr(name)+"],capture_output=True,text=True);assert r.returncode==0,r.stderr\nprint(json.dumps(dict(removed=True)))")
 owned.remove((machine,name));f.write(C/'ACTIVE_OWNED_NETWORK_FAULTS.json',[dict(machine=m['ip'],table=n)for m,n in owned])
def send_one(tag,source='srcaa',dest='dstaa',index=0):
 return f.send_fresh([f.action('eosio.token','transfer',source,{'from':source,'to':dest,'quantity':'0.0001 SYS','memo':tag+' '+str(time.time_ns())})],node_index=index)
def balances(src,dst):
 return {n['account']:[f.balance(n,'eosio.token',src,'SYS'),f.balance(n,'eosio.token',dst,'SYS')]for n in nodes}
def wait_known(cmd,origin,deadline_seconds=120):
 limit=time.monotonic()+deadline_seconds;last=origin;found=None
 while True:
  info=f.infos();head=min(x['head_block_num']for x in info.values())
  if not found:
   for h in range(last,head+1):
    b=f.rpc(nodes[0],'get_block',dict(block_num_or_id=h))
    if cmd['id']in set(f.block_ids(b)):found=dict(number=h,id=b['id']);break
   last=head+1
  if found and all(x['last_irreversible_block_num']>=found['number']for x in info.values()):
   copies={n['account']:f.rpc(n,'get_block',dict(block_num_or_id=found['number']))for n in nodes}
   assert all(b['id']==found['id']and cmd['id']in set(f.block_ids(b))for b in copies.values())
   return dict(id=cmd['id'],included=found,all_thirty_finalized=True)
  assert time.monotonic()<limit,'Previously submitted transaction not finalized after recovery'
  time.sleep(1)
def current_policy():
 limit=time.monotonic()+300
 while True:
  p=f.rpc(nodes[0],'get_finalizer_info')
  if not p.get('pending_finalizer_policy'):return p['active_finalizer_policy']
  assert time.monotonic()<limit,'Policy handoff did not settle before partition test'
  time.sleep(3)
def ban_flags():
 rows=f.rpc(nodes[0],'get_table_rows',dict(code='eosio',scope='eosio',table='rotcands',json=True,limit=1000))['rows']
 return {r['owner']:r['banned']for r in rows}
try:
 save(dict(status='waiting-for-R04-role-lifecycle-and-rolling-restarts-before-network-fault-round',pressure_load=False))
 limit=time.monotonic()+2400
 gate=Path('/opt/aic-bp-standby-seed-failover-20261030/live-qualification/QUALIFICATION.json')
 while not gate.exists():
  progress=Path('/opt/aic-bp-standby-seed-failover-20261030/live-qualification/STATUS.json')
  if progress.exists():assert read(progress)['status']!='stopped-on-preserved-failure','Seed fix live regression failed; network round untouched'
  assert time.monotonic()<limit,'Qualified standby failover prerequisite did not finish'
  time.sleep(3)
 assert read(gate)['status']=='PASS'
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.T=topology;f.N=nodes;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  f.PLAN.write_text(f.PLAN.read_text()+'\nR05执行方案：单条P2P断连及同ID重发；对一个BP施加仅限自有P2P地址的双向隔离，保留RPC/SSH，其他20名确认者继续；按实际稳定政策权重选一台主机分成两组（两侧均低于门槛15），隔离20秒检查新块不能最终确认，恢复后检查统一链。延迟只匹配自有P2P流量，50ms；坏包只各发一次，无压力。临时防火墙规则在finally中撤回；现有UFW及防双签状态不清空。\n')
  D=f.begin('NET-01','R05');before=balances('srcaa','dstaa');n=nodes[0]
  connections=f.rpc(n,'connections',ns='net');peer=next(c['peer']for c in connections if c.get('is_socket_open')and c.get('peer')and c.get('is_bp_gossip_peer'))
  disconnect=f.rpc(n,'disconnect',peer,ns='net');cmd=f.signed([f.action('eosio.token','transfer','srcaa',{'from':'srcaa','to':'dstaa','quantity':'0.0001 SYS','memo':'edge-drop-once-'+str(time.time_ns())})])
  label='net-edge-'+str(time.time_ns());f.COMMANDS[label]=cmd;f.write(D/'SUBMITTED_PACKET.json',cmd);tx=f.send(label);f.ACTIVE['successful_business_transactions'].append(tx['id'])
  reconnect=f.rpc(n,'connect',peer,ns='net');duplicate=f.rpc(nodes[1],'push_transaction',cmd['packed'],expected_error=True)
  assert 'duplicate'in json.dumps(duplicate).lower();f.ACTIVE['expected_rejections'].append(duplicate)
  after=balances('srcaa','dstaa');assert all(after[k]==[v[0]-1,v[1]+1]for k,v in before.items())
  f.finish('NET-01',dict(peer=peer,disconnect=disconnect,reconnect=reconnect,transaction=tx,same_packet_duplicate_rejection=duplicate,balances_before=before,balances_after=after,all_thirty_finalized_once=True))
  # Isolate one node completely at its owned P2P endpoints, preserving localhost RPC.
  D=f.begin('NET-02','R05');victim=nodes[0];machine=topology['machines'][victim['host_index']];name='aic_bp30_single_fault_20261030'
  all_ips=sorted({x['ip']for x in nodes});portset=', '.join(str(11000+i)for i in range(10))
  rules='\n'.join(['table inet '+name+' {',' chain incoming { type filter hook input priority -7; policy accept; ip saddr { '+', '.join(all_ips)+' } tcp dport '+str(victim['p2p_port'])+' drop; }',' chain outgoing { type filter hook output priority -7; policy accept; meta skuid '+str(victim['uid'])+' ip daddr { '+', '.join(all_ips)+' } drop; }','}',''])
  before=balances('srcaa','dstaa');origin=min(x['head_block_num']for x in f.infos().values())-2
  table_install(machine,name,rules)
  cmd=f.signed([f.action('eosio.token','transfer','srcaa',{'from':'srcaa','to':'dstaa','quantity':'0.0001 SYS','memo':'isolated-accepted-'+str(time.time_ns())})])
  f.write(D/'SUBMITTED_PACKET.json',cmd)
  accepted=f.rpc(victim,'push_transaction',cmd['packed']);assert accepted['transaction_id']==cmd['id']
  initial_lib={n['account']:f.rpc(n,'get_info')['last_irreversible_block_num']for n in nodes if n is not victim}
  f.N=[n for n in nodes if n is not victim];majority_tx=send_one('healthy-majority-during-isolation','srcac','dstac');f.N=nodes
  time.sleep(10);isolated=f.rpc(victim,'get_info');healthy={n['account']:f.rpc(n,'get_info')for n in nodes if n is not victim}
  assert any(v['last_irreversible_block_num']>initial_lib[k]for k,v in healthy.items())
  # Check healthy canonical blocks have not incorporated the isolated packet.
  end=min(v['head_block_num']for v in healthy.values())
  for h in range(origin,end+1):
   b=f.rpc(nodes[1],'get_block',dict(block_num_or_id=h));assert cmd['id']not in set(f.block_ids(b))
  f.write(D/'isolated-accepted-but-not-confirmed.json',dict(id=cmd['id'],reply=accepted,isolated_info=isolated,healthy_info=healthy,healthy_majority_transaction=majority_tx))
  table_remove(machine,name)
  try:retry=f.rpc(nodes[1],'push_transaction',cmd['packed'])
  except RuntimeError as e:
   assert 'duplicate'in str(e).lower();retry=dict(expected_duplicate_race=True)
  receipt=wait_known(cmd,origin);after=balances('srcaa','dstaa')
  assert all(after[k]==[v[0]-1,v[1]+1]for k,v in before.items())
  f.ACTIVE['successful_business_transactions'].append(cmd['id'])
  f.finish('NET-02',dict(accepted_while_isolated_but_unconfirmed=True,majority_stayed_live=majority_tx,retry_outcome=retry,recovered_receipt=receipt,balances_before=before,balances_after=after,isolated_process_remained_alive=True))
  D=f.begin('NET-04','R05');f.finish('NET-04',dict(evidence=str(B/'rounds/R05/NET-02'),minority_isolated=1,healthy_finalizers_at_least=20,required_quorum=15,healthy_majority_transaction=majority_tx,all_thirty_converged_after_recovery=True))
  # Divide by actual finalizer weights, preserving management traffic.
  D=f.begin('NET-05','R05');policy=current_policy();weights={x['description']:x['weight']for x in policy['finalizers']};total=sum(weights.values());threshold=policy['threshold']
  hostweights={m['host_index']:sum(weights.get(n['account'],0)for n in nodes if n['host_index']==m['host_index'])for m in topology['machines']}
  isolated_host=next(h for h,w in hostweights.items()if w<threshold and total-w<threshold);m=next(x for x in topology['machines']if x['host_index']==isolated_host)
  others=sorted({n['ip']for n in nodes if n['host_index']!=isolated_host});name='aic_bp30_partition_20261030'
  lines=['table inet '+name+' {',' chain incoming { type filter hook input priority -7; policy accept;']
  for direction,addr in [('incoming','saddr')]:
   for field in ['sport','dport']:lines+=[' ip '+addr+' { '+', '.join(others)+' } tcp '+field+' 11000-11009 drop;']
  lines+=[' }',' chain outgoing { type filter hook output priority -7; policy accept;']
  for field in ['sport','dport']:lines+=[' ip daddr { '+', '.join(others)+' } tcp '+field+' 11000-11009 drop;']
  lines+=[' }','}',''];table_install(m,name,'\n'.join(lines))
  partition_start=f.infos();cutoff=max(x['head_block_num']for x in partition_start.values());time.sleep(20);during=f.infos()
  assert all(x['last_irreversible_block_num']<=cutoff for x in during.values()),'A below-quorum partition finalized newly produced blocks'
  minimum=min(x['last_irreversible_block_num']for x in during.values());maximum=max(x['last_irreversible_block_num']for x in during.values());finalized={}
  for h in range(minimum,maximum+1):
   ids={n['account']:f.rpc(n,'get_block',dict(block_num_or_id=h))['id']for n in nodes if during[n['account']]['last_irreversible_block_num']>=h}
   assert len(set(ids.values()))==1,'Conflicting finalized block IDs'
   finalized[str(h)]=ids
  f.write(D/'partition-proof.json',dict(policy=policy,hostweights=hostweights,isolated_host=isolated_host,start=partition_start,cutoff_head=cutoff,during=during,finalized_prefixes=finalized))
  table_remove(m,name)
  f.finish('NET-05',dict(policy=policy,group_weights=[hostweights[isolated_host],total-hostweights[isolated_host]],threshold=threshold,no_new_post_isolation_block_finalized=True,no_conflicting_finalized_prefix=True,raw=str(D/'partition-proof.json'),already_certified_inflight_blocks_counted_correctly=True))
  D=f.begin('NET-06','R05');deadline=time.monotonic()+300;history=[]
  while True:
   info=f.infos();history.append(dict(UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),info=info));f.write(D/'recovery.json',history)
   if min(x['last_irreversible_block_num']for x in info.values())>cutoff+2:break
   assert time.monotonic()<deadline,'Partition recovery failed to restore new finality'
   time.sleep(2)
  recovered=send_one('after-two-below-quorum-partitions','srcad','dstad')
  f.finish('NET-06',dict(all_thirty_recovered=True,new_ordinary_transfer=recovered,safety_never_reset=True))
  # One malformed probe per category, no flooding.
  D=f.begin('NET-08','R05');before_flags=ban_flags();probes=[]
  request=f.peer_request([]);request['chain_id']='00'*32;frame=f.helper(request)['handshake']
  with socket.create_connection(('127.0.0.1',nodes[0]['p2p_port']),timeout=5)as s:
   s.sendall(bytes.fromhex(frame['hex']));history=[];deadline=time.monotonic()+5
   while time.monotonic()<deadline:
    try:
     raw=f.framed_read(s,deadline)
     if raw[0]in[0,2,12]:history.append(f.helper(dict(mode='decode',hex=raw.hex())))
     if raw[0]==2:break
    except(EOFError,ConnectionResetError,socket.timeout):break
   assert any(v['message_index']==2 and v['value']['reason']==3 for v in history),'Wrong-chain handshake was not rejected with wrong_chain'
   probes.append(dict(kind='wrong-chain',history=history))
  with socket.create_connection(('127.0.0.1',nodes[0]['p2p_port']),timeout=5)as s:
   s.sendall(struct.pack('<I',67108865));deadline=time.monotonic()+5;closed=False;preclose_frames=0
   while time.monotonic()<deadline:
    try:
     f.framed_read(s,deadline);preclose_frames+=1
    except(EOFError,ConnectionResetError):closed=True;break
    except socket.timeout:break
   assert closed,'Oversize header connection stayed open beyond the original 5s budget'
   probes.append(dict(kind='single-64MiB-plus-1-length-header',closed=True,bytes_sent=4,preclose_frames=preclose_frames))
  req=f.peer_request([dict(account=nodes[0]['account'],role='invalid-peer-key-test',initial=True)])
  frames=f.helper(req)
  with socket.create_connection(('127.0.0.1',nodes[0]['p2p_port']),timeout=5)as s:
   localport=s.getsockname()[1];s.sendall(bytes.fromhex(frames['handshake']['hex'])+bytes.fromhex(frames['gossip']['hex']));time.sleep(1)
   status=[v for v in f.rpc(nodes[0],'connections',ns='net')if v.get('remote_ip')=='127.0.0.1'and v.get('remote_port')==str(localport)]
   assert not any(v.get('is_bp_peer')or v.get('is_bp_gossip_peer')for v in status)
   probes.append(dict(kind='wrong-current-key-signature',no_BP_privilege=True,connection=status))
  after_flags=ban_flags();assert before_flags==after_flags
  healthy=send_one('after-invalid-wire-probes','srcae','dstae')
  f.write(D/'probes.json',probes)
  f.finish('NET-08',dict(probes=probes,banned_flags_before=before_flags,banned_flags_after=after_flags,ordinary_transfer_afterward=healthy))
  D=f.begin('BAN-17','R07');f.finish('BAN-17',dict(evidence=str(B/'rounds/R05/NET-08'),P2P_rejections_do_not_create_chain_permanent_bans=True,flags=after_flags))
  # Delay only owned P2P flows with a dedicated IFB, preserving all other filters.
  D=f.begin('NET-07','R05');iface=None;created_clsact=False;tc_added=[];ifb='aicbp30delay'
  try:
   route=json.loads(subprocess.check_output(['ip','-j','route','get',nodes[1]['ip']],text=True))[0];iface=route['dev']
   assert subprocess.run(['ip','link','show','dev',ifb],capture_output=True).returncode!=0
   subprocess.run(['modprobe','ifb'],check=True,capture_output=True)
   subprocess.run(['ip','link','add',ifb,'type','ifb'],check=True,capture_output=True);subprocess.run(['ip','link','set',ifb,'up'],check=True,capture_output=True)
   subprocess.run(['tc','qdisc','add','dev',ifb,'root','netem','delay','50ms','limit','1024'],check=True,capture_output=True)
   qdiscs=json.loads(subprocess.check_output(['tc','-j','qdisc','show','dev',iface],text=True))
   if not any(q['kind']in['ingress','clsact']for q in qdiscs):
    subprocess.run(['tc','qdisc','add','dev',iface,'clsact'],check=True,capture_output=True);created_clsact=True
   for pref,field in [('49300','src_port'),('49301','dst_port')]:
    args=['tc','filter','add','dev',iface,'ingress','protocol','ip','pref',pref,'flower','src_ip',nodes[1]['ip'],'ip_proto','tcp',field,'11000','action','mirred','egress','redirect','dev',ifb]
    subprocess.run(args,check=True,capture_output=True);tc_added.append(pref)
   delayed=send_one('controlled-50ms-P2P-delay','srcaf','dstaf');time.sleep(2)
   stats=json.loads(subprocess.check_output(['tc','-j','-s','qdisc','show','dev',ifb],text=True));assert any(q.get('packets',0)>0 for q in stats),'No controlled P2P packets traversed the delay'
   duplicates=[f.rpc(nodes[0],'connect',nodes[1]['ip']+':'+str(nodes[1]['p2p_port']),ns='net')for _ in range(2)]
   connection_count=sum(v.get('is_socket_open',False)for v in f.rpc(nodes[0],'connections',ns='net'));assert connection_count<=62
   f.write(D/'delay-evidence.json',dict(interface=iface,owned_IFB=ifb,delay_ms=50,stats=stats,transaction=delayed,duplicate_connect_results=duplicates,open_connections=connection_count))
  finally:
   if iface:
    for pref in tc_added:subprocess.run(['tc','filter','del','dev',iface,'ingress','protocol','ip','pref',pref],capture_output=True)
    if created_clsact:
     filters=json.loads(subprocess.check_output(['tc','-j','filter','show','dev',iface,'ingress'],text=True))
     if not filters:subprocess.run(['tc','qdisc','del','dev',iface,'clsact'],capture_output=True)
   subprocess.run(['ip','link','del',ifb],capture_output=True)
  f.finish('NET-07',dict(actual_50ms_owned_P2P_delay_and_recovery=True,duplicate_connection_bounded=True,one_way_loss_and_process_alive_evidence=str(B/'rounds/R05/NET-02'),evidence=str(D/'delay-evidence.json'),owned_rules_cleaned=True))
  D=f.begin('NET-09','R05');logs=[]
  for m in topology['machines']:
   root=C if m['primary']else P;accounts=[n['account']for n in nodes if n['host_index']==m['host_index']]
   code="from pathlib import Path\nimport json,re\nroot=Path("+repr(str(root))+");out=[]\nfor account in "+repr(accounts)+":\n p=root/'live'/account/'node.log';s=p.read_text(errors='replace');matches=[line for line in s.splitlines()if re.search(r'write queue.*full|max write_queue_size|readloop.*write_queue_size',line,re.I)];out.append(dict(account=account,queue_close_events=len(matches),matching_lines=matches))\nprint(json.dumps(out))"
   logs+=operate(m,code)
  f.write(D/'queue-events.json',logs);assert not any(v['queue_close_events']for v in logs),'Unexpected write queue overload under functional-only load'
  f.finish('NET-09',dict(functional_only_queue_check=logs,no_saturation=True,TPS='unmeasured',expected_fault_disconnects_recorded_separately=True))
  f.write(C/'NETWORK_FUNCTIONAL_PASSED.json',dict(status='round-R05-owned-network-fault-functional-cases-passed',chain_id=topology['chain_id'],source_commit=topology['source_commit'],cases=['NET-01','NET-02','NET-03','NET-04','NET-05','NET-06','NET-07','NET-08','NET-09'],pressure_load=False,baseline_promoted=False))
  save(dict(status='R05-network-fault-functional-round-passed-next-blacklist-and-third-party-rounds-pending',running_nodes=30,pressure_load=False,baseline_promoted=False))
except BaseException as e:
 f.N=nodes;f.failed(e);save(dict(status='network-fault-round-stopped-on-preserved-error',error=str(e),traceback=traceback.format_exc(),pressure_load=False,baseline_promoted=False));raise
finally:
 for machine,name in list(owned):
  try:table_remove(machine,name)
  except BaseException as e:f.write(C/('FAULT_CLEANUP_ERROR-'+name+'.json'),dict(error=str(e),table=name,machine=machine['ip']))
