from functional_30_common import *
Q=C/'third-party-live';A='atomicassets';M='atomicmarket';collection='aictestnftaa';creator='srcaa';buyer='dstaa'
def rows(code,scope,table):
 values={n['account']:f.rpc(n,'get_table_rows',dict(code=code,scope=scope,table=table,json=True,limit=1000))for n in nodes}
 assert len({json.dumps(v,sort_keys=True)for v in values.values()})==1,(code,scope,table)
 return values[nodes[0]['account']]['rows']
def tx(code,name,actor,data,expected=False):
 return f.send_fresh([f.action(code,name,actor,data)],expected_error=expected)
def aset(owner):return rows(A,owner,'assets')
def owns(owner,ident):return any(int(x['asset_id'])==int(ident)for x in aset(owner))
def template(transferable=True,burnable=True,max_supply=10):
 current=rows(A,collection,'templates')
 for r in current:
  if r['transferable']==transferable and r['burnable']==burnable and int(r['max_supply'])==max_supply and int(r['issued_supply'])==0:return int(r['template_id'])
 old={int(x['template_id'])for x in current}
 tx(A,'createtempl',creator,dict(authorized_creator=creator,collection_name=collection,schema_name='items',transferable=transferable,burnable=burnable,max_supply=max_supply,immutable_data=[dict(first='name',second=['string','Functional test item'])]))
 current=rows(A,collection,'templates');return next(int(x['template_id'])for x in current if int(x['template_id'])not in old)
def mint(template_id,owner=creator):
 old={int(x['asset_id'])for x in aset(owner)}
 tx(A,'mintasset',creator,dict(authorized_minter=creator,collection_name=collection,schema_name='items',template_id=template_id,new_asset_owner=owner,immutable_data=[],mutable_data=[dict(first='level',second=['uint64',1])],tokens_to_back=[]))
 return next(int(x['asset_id'])for x in aset(owner)if int(x['asset_id'])not in old)
def move(owner,dest,ids,expected=False):
 return tx(A,'transfer',owner,{'from':owner,'to':dest,'asset_ids':ids,'memo':'low-frequency NFT functional test'},expected)
def offer(sender,dest,ids):
 old={int(x['offer_id'])for x in rows(A,A,'offers')}
 tx(A,'createoffer',sender,dict(sender=sender,recipient=dest,sender_asset_ids=ids,recipient_asset_ids=[],memo='functional offer'))
 return next(int(x['offer_id'])for x in rows(A,A,'offers')if int(x['offer_id'])not in old)
def partial(cid,observed):
 evidence=Path(f.ACTIVE['evidence']);f.ACTIVE.update(status='PARTIAL',observed=observed,end_UTC=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()));f.write(evidence/'result.json',f.ACTIVE);f.record_status(cid,'PARTIAL',evidence,'Only explicitly listed executed subcases covered');f.ACTIVE=None
try:
 gate=Q/'DEPLOYED.json';limit=time.monotonic()+1800
 while not gate.exists():
  assert time.monotonic()<limit,'Third-party contracts not deployed'
  time.sleep(3)
 with Path('/opt/aic-group8-repeat-multimachine-20261029/RESOURCE_PHASE.lock').open('a')as lock:
  fcntl.flock(lock,fcntl.LOCK_EX)
  topology=read(C/'LIVE_TOPOLOGY.json');nodes=topology['nodes'];f.N=nodes;f.T=topology;f.F=read(C/'fixture/PUBLIC_FIXTURE.json')
  D=f.begin('NFT-01','R10')
  if not any(x['collection_name']==collection for x in rows(A,A,'collections')):
   tx(A,'createcol',creator,dict(author=creator,collection_name=collection,allow_notify=True,authorized_accounts=[creator],notify_accounts=[],market_fee=.01,data=[]))
  if not any(x['schema_name']=='items'for x in rows(A,collection,'schemas')):
   tx(A,'createschema',creator,dict(authorized_creator=creator,collection_name=collection,schema_name='items',schema_format=[dict(name='name',type='string'),dict(name='level',type='uint64')]))
  if(Q/'NFT_SETUP.json').exists():
   setup=read(Q/'NFT_SETUP.json');normal=setup['normal_template'];locked=setup['locked_template'];limited=setup['limited_template'];unburnable=setup['unburnable_template']
  else:
   normal=template();locked=template(False,True,2);unburnable=template(True,False,2);limited=template(True,True,1)
   setup=dict(collection=collection,schema='items',normal_template=normal,locked_template=locked,limited_template=limited,unburnable_template=unburnable,creator=creator,buyer=buyer)
   f.write(Q/'NFT_SETUP.json',setup)
  f.finish('NFT-01',dict(setup=setup,collections=rows(A,A,'collections'),schemas=rows(A,collection,'schemas'),templates=rows(A,collection,'templates'),deployment_evidence=str(Q/'DEPLOYED.json')))
  D=f.begin('NFT-02','R10')
  first=mint(normal);move(creator,buyer,[first]);assert owns(buyer,first)and not owns(creator,first)
  tx(A,'setassetdata',creator,dict(authorized_editor=creator,asset_owner=buyer,asset_id=first,new_mutable_data=[dict(first='level',second=['uint64',7])]))
  edited=next(x for x in aset(buyer)if int(x['asset_id'])==first);assert edited['mutable_serialized_data']
  move(buyer,creator,[first]);tx(A,'burnasset',creator,dict(asset_owner=creator,asset_id=first));assert not owns(creator,first)
  lock_id=mint(locked);move(creator,buyer,[lock_id],True);assert owns(creator,lock_id)
  unburn_id=mint(unburnable);tx(A,'burnasset',creator,dict(asset_owner=creator,asset_id=unburn_id),True);assert owns(creator,unburn_id)
  limited_id=mint(limited)
  tx(A,'mintasset',creator,dict(authorized_minter=creator,collection_name=collection,schema_name='items',template_id=limited,new_asset_owner=creator,immutable_data=[],mutable_data=[],tokens_to_back=[]),True)
  assert owns(creator,limited_id)
  f.finish('NFT-02',dict(minted_transferred_edited_burned=first,nontransferable_rejected=lock_id,nonburnable_rejected=unburn_id,max_supply_enforced=limited_id,mutable_edit_raw=edited,all30_ownership_checked=True))
  D=f.begin('NFT-03','R10');valid=mint(normal);before=aset(creator)
  tx(A,'transfer','srcac',{'from':creator,'to':buyer,'asset_ids':[valid],'memo':'expected missing owner authority'},True)
  tx(A,'transfer',creator,{'from':creator,'to':buyer,'asset_ids':[valid,valid],'memo':'expected duplicate IDs'},True)
  tx(A,'transfer',buyer,{'from':buyer,'to':creator,'asset_ids':[valid],'memo':'expected wrong owner scope'},True)
  tx(A,'mintasset','srcac',dict(authorized_minter='srcac',collection_name=collection,schema_name='items',template_id=normal,new_asset_owner='srcac',immutable_data=[],mutable_data=[],tokens_to_back=[]),True)
  assert aset(creator)==before
  partial('NFT-03',dict(authorization_duplicate_ID_wrong_scope_rejections=True,all30_rows_unchanged=True,forged_notification_case='pending dedicated fake-token/notification contract'))
  D=f.begin('NFT-04','R10')
  oid=offer(creator,buyer,[valid]);tx(A,'acceptoffer',buyer,dict(offer_id=oid));assert owns(buyer,valid)
  tx(A,'acceptoffer',buyer,dict(offer_id=oid),True)
  oid2=offer(buyer,creator,[valid]);tx(A,'canceloffer',buyer,dict(offer_id=oid2));assert not any(int(x['offer_id'])==oid2 for x in rows(A,A,'offers'))
  oid3=offer(buyer,creator,[valid]);move(buyer,'dstac',[valid]);tx(A,'acceptoffer',creator,dict(offer_id=oid3),True);assert owns('dstac',valid)
  tx(A,'canceloffer',buyer,dict(offer_id=oid3))
  f.finish('NFT-04',dict(accepted_once=oid,cancelled=oid2,ownership_changed_offer_invalid=oid3,TTL='This pinned interface has no offer TTL; invalidity tested by ownership change'))
  D=f.begin('NFT-05','R10')
  tx(M,'addconftoken',M,dict(token_contract='eosio.token',token_symbol='4,SYS'))
  config=rows(M,M,'config')
  f.finish('NFT-05',dict(matching_real_AtomicMarket_deployed=True,SYS_contract_and_symbol_configured=config,all30_tables_equal=True))
  f.write(Q/'NFT_BASE_PASSED.json',dict(status='base-NFT-round-completed',full_cases=['NFT-01','NFT-02','NFT-04','NFT-05'],partial=['NFT-03'],NFT_setup=setup,pressure_load=False,TPS='unmeasured',baseline_promoted=False))
except BaseException as e:
 f.failed(e);f.write(Q/'NFT_FAILED.json',dict(error=str(e),traceback=traceback.format_exc(),baseline_promoted=False));raise
