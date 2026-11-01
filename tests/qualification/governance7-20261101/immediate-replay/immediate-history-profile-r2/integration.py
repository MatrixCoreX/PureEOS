import importlib.util,json,urllib.request,hashlib,pathlib,datetime
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
vpath='/opt/purehistory-producer-votes-20261101-r1/producer_votes.py';spath='/opt/pureeos-rotation-demo-20261031-r1/producer_summary.py'
votes=load('qualified_votes',vpath);summary=load('qualified_summary',spath)
def rpc(method,args={}):
 request=urllib.request.Request('http://127.0.0.1:15090/v1/chain/'+method,data=json.dumps(args).encode(),headers={'Content-Type':'application/json'})
 return json.load(urllib.request.urlopen(request,timeout=15))
rotation=rpc('get_table_rows',{'code':'eosio','scope':'eosio','table':'rotation','json':True,'limit':1})['rows'][0]
info=rpc('get_info');s=summary.summarize(rpc,rotation,rpc('get_producer_schedule'))
assert s['available'] and s['registered_bp_count']==212 and s['eligible_bp_count']==211 and s['permanent_blacklist_count']==1 and s['active_producer_count']==21,s
v=votes.snapshot(rpc,['replaynodeaa','replaynodeav','replaynodeic'],'25f7b6b3f91a2961a2f33d6914b309be5e6e1458dfa8bf602c68eeb2726b49dc')
assert v['available'] and v['system_code_hash']=='373b66b2994c3131ce6c9c3d4526fdf836c0ac2c36d8265b2239d61e75ca2ceb'
assert len(v['rows'])==3 and all(x['total_votes']==x['self_votes'] and x['other_votes']=='0' for x in v['rows'])
assert [x['owner']for x in v['rows']if x['permanently_blacklisted']]==['replaynodeav'],v
print(json.dumps({'status':'PASS','tested_at':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Actual deployed PureHistory Python modules reading unchanged production nodeos replay state; no live SHIP indexing in this isolated test','chain_id':info['chain_id'],'head_block_num':info['head_block_num'],'summary':s,'votes':v,'module_SHA256':{x:hashlib.sha256(pathlib.Path(x).read_bytes()).hexdigest()for x in [vpath,spath]}}))
