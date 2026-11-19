"""Retire only mapped TEST BP identities with irreversible native blacklist proof."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,datetime

def retire_confirmed(g,nodes):
 ro=g.rows();done=[]
 for name,v in nodes.items():
  if not ro[name]['banned']or(g.R/('auto-retired/'+name+'.json')).exists():continue
  row=ro[name];i=g.rpc('get_info');reader=g.rpc('get_info',port=8890);assert row['zero_windows']==3 and 0<row['ban_final_block']<=min(i['last_irreversible_block_num'],reader['last_irreversible_block_num'])
  block=g.rpc('get_block',{'block_num_or_id':row['ban_final_block']});assert g.rpc('get_block',{'block_num_or_id':row['ban_final_block']},port=8890)['id']==block['id']
  cfg=Path(v['config_dir']);data=Path(v['data_dir']);assert cfg.resolve()==cfg and data.resolve()==data and not cfg.is_symlink()and not data.is_symlink()
  # The founder also serves shared full-genesis/writer roles. Stop a banned founder,
  # but require a reviewed role handoff before deleting its shared source database.
  if name=='gptgenesisbp':
   g.run(['systemctl','disable',v['unit']]);g.run(['systemctl','stop',v['unit']],65);g.save('auto-retired/'+name+'.json',{'status':'STOPPED_SHARED_CHAIN_SOURCE_PRESERVED_REQUIRES_ROLE_HANDOFF','utc':g.now(),'ban_block_id':block['id']});done.append(name);continue
  assert cfg.name=='config'and cfg.parent==data.parent and cfg.parent.name==name and data.name.startswith('data')
  previous=g.R/('auto-retire-events/'+name+'/STARTED.json');escrow=g.R/'private'/'automatic-retirement'/name
  if not(cfg/'config.ini').exists():
   assert previous.exists()and(escrow/'credentials.json').exists()and g.state(v['unit'])['MainPID']=='0'
   proof=json.loads(previous.read_text());assert proof['ban_block_id']==block['id']and proof['config_sha256']==v['config_sha256']
   assert not list(cfg.iterdir())and all(x.name=='finalizers'for x in data.iterdir())
   g.save('auto-retired/'+name+'.json',{'status':'PASS_RECONCILED_COMPLETED_RETIREMENT','utc':g.now(),'owner':name,'ban_block_id':block['id']});done.append(name);continue
  raw=(cfg/'config.ini').read_text();assert g.sha(cfg/'config.ini')==v['config_sha256'];assert re.findall(r'^\s*producer-name\s*=\s*([a-z1-5.]+)\s*$',raw,re.M)==[name]
  pid=g.state(v['unit'])['MainPID']
  if pid!='0':
   args=[x.decode()for x in(Path('/proc')/pid/'cmdline').read_bytes().split(b'\0')if x];assert str(cfg)==args[args.index('--config-dir')+1]and str(data)==args[args.index('--data-dir')+1]
  escrow=g.R/'private'/'automatic-retirement'/name;escrow.mkdir(parents=True,mode=0o700,exist_ok=True);(escrow/'credentials.json').write_text(json.dumps({'owner':name,'credential_lines':[l for l in raw.splitlines()if re.match(r'^\s*(signature-provider|peer-private-key)\s*=',l)]}));(escrow/'credentials.json').chmod(0o600)
  g.save('auto-retire-events/'+name+'/STARTED.json',{'utc':g.now(),'owner':name,'unit':v['unit'],'ban_block_id':block['id'],'ban_block_num':row['ban_final_block'],'config_sha256':v['config_sha256'],'scope':'Permanentban confirmed by both local canonical sources. No unban/re-enrollment.'})
  g.run(['systemctl','disable',v['unit']]);g.run(['systemctl','stop',v['unit']],65);assert g.state(v['unit'])['MainPID']=='0'
  if(data/'finalizers').exists():
   for f in(data/'finalizers').rglob('*'):
    if f.is_file():
     dest=escrow/'finalizers'/f.relative_to(data/'finalizers');dest.parent.mkdir(parents=True,mode=0o700,exist_ok=True);dest.write_bytes(f.read_bytes());assert g.sha(dest)==g.sha(f)
  # Refuse deletion if another process still maps/opens this node's roots.
  for proc in Path('/proc').iterdir():
   if not proc.name.isdigit():continue
   paths=[]
   try:
    for line in(proc/'maps').read_text().splitlines():
     parts=line.split(None,5)
     if len(parts)==6:paths.append(parts[5])
    for fd in(proc/'fd').iterdir():
     try:paths.append(os.readlink(fd))
     except OSError:pass
   except OSError:pass
   assert not any(p.startswith('/')and(any(Path(p)==r or r in Path(p).parents for r in [cfg,data]))for p in paths),'Live reference prevents banned data deletion'
  for root in [cfg,data]:
   assert all(not p.is_symlink()for p in root.rglob('*'))
   for child in list(root.iterdir()):
    if root==data and child.name=='finalizers':continue
    if child.is_dir():shutil.rmtree(child)
    else:child.unlink()
  result={'status':'PASS_PERMANENT_BAN_STOPPED_BLOCKS_CONFIG_DELETED','utc':g.now(),'owner':name,'ban_block_id':block['id'],'server_only_private_keys_and_safety_retained':True};g.save('auto-retired/'+name+'.json',result);done.append(name)
 return done
