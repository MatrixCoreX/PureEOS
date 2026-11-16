import json,subprocess,urllib.request,datetime,os,hashlib,re
from pathlib import Path
R=Path('/opt/pureeos-testnet-stall-recovery-20261116-r1');assert not R.exists();R.mkdir(mode=0o700);(R/'private').mkdir(mode=0o700);os.umask(0o077)
def rpc(n,d=None):
 q=urllib.request.Request('http://127.0.0.1:8890/v1/chain/'+n,json.dumps(d or{}).encode(),{'Content-Type':'application/json'});return json.load(urllib.request.urlopen(q,timeout=10))
def save(n,d):(R/n).write_text(json.dumps(d,indent=2)+'\n')
info=rpc('get_info');assert info['chain_id']=='09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'
rows=rpc('get_table_rows',{'json':True,'code':'eosio','scope':'eosio','table':'rotcands','limit':1000});assert not rows['more'];banned={x['owner']for x in rows['rows']if x['banned']};assert len(banned)==21
# Merge only published topology metadata, never private key files.
O=Path('/opt/pureeos-testnet-resilience-20261110-r1');N=Path('/opt/pureeos-testnet-new-ban-20261111-r1');nodes=json.loads((O/'RESTORE_NODE_PATHS.json').read_text())
for p in [O/'REPLACEMENTS_PREPARED.json',N/'REPLACEMENTS_PREPARED.json']:
 for x in json.loads(p.read_text())['nodes']:nodes[x['name']]=x
# Native service ExecStart stays inside this process; only paths are disclosed.
units=subprocess.check_output(['systemctl','list-unit-files','--type=service','--no-legend','--plain','pureeos*'],text=True);allunits=[l.split()[0]for l in units.splitlines()if l.strip()]
configs=[];unitdata={}
for u in allunits:
 if not any(n in u for n in banned):continue
 text=subprocess.check_output(['systemctl','show',u,'-p','MainPID,ActiveState,UnitFileState,ControlGroup,ExecStart'],text=True);kv=dict(l.split('=',1)for l in text.splitlines()if'='in l);args=kv.pop('ExecStart','');paths={k:v for k,v in re.findall(r'--(config-dir|data-dir)(?:=|\s+)(/[^\s;]+)',args)};unitdata[u]={**kv,'paths':paths}
roots=[Path('/opt/pureeos-devnet-20261105-r1'),O,N]
for base in roots:
 for p in base.rglob('config.ini'):
  if p.is_symlink()or not p.is_file():continue
  raw=p.read_text();match=re.findall(r'^\s*producer-name\s*=\s*([a-z1-5.]+)\s*$',raw,re.M)
  if len(match)==1 and match[0]in banned:
   name=match[0];conf=p.parent;data=conf.parent/'data';configs.append({'name':name,'config_dir':str(conf),'data_dir':str(data),'config_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'data_exists':data.exists()})
for name,x in nodes.items():
 if name not in banned:continue
 cfg=x.get('config_dir');data=x.get('data_dir')
 if not cfg:
  root=(O/'nodes'/name)if name in{'berlintucomp','bostonmiteng','nvidiaailabs','tencentailab','zurichethnet'}else(N/'nodes'/name);cfg=str(root/'config');data=str(root/'data')
 for row in configs:
  if row['config_dir']==cfg:row['data_dir']=data;row['unit']=x['unit']
# Same native-policy safety:19nonbanned members remain, threshold15 retained.
f=rpc('get_finalizer_info');active={v['description']for v in f['active_finalizer_policy']['finalizers']};assert len(active-banned)>=f['active_finalizer_policy']['threshold']
plan={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain':info,'banned':sorted(banned),'active_banned':sorted(active&banned),'active_nonbanned_count':len(active-banned),'native_threshold':f['active_finalizer_policy']['threshold'],'all_known_nodes':nodes,'units_to_stop':unitdata,'exact_config_roots':configs,'scope':'STOP/DISABLEonly bannedidentity nativeunits; delete planlater afterprivatekey/safety preservation. No chainwrite/ordinaryfinancialcontroller.'};save('BANNED_STOP_PLAN.json',plan);print(json.dumps(plan))
