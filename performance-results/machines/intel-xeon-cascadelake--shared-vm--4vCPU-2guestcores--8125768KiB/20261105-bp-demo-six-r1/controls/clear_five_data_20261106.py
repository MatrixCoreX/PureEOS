"""Delete only the five explicitly authorized stopped-node datasets; retain safety."""
from common import *
import stat,shutil,traceback

NEW=['tokyoaistlab','pariscnrslab','londonturing'];OLD=['tokyorikenai','singaporentu'];NAMES=OLD+NEW
def unit(n):return ('pureeos-devnet-bp-'if n in OLD else'pureeos-bp-demo-')+n+'-20261105.service'
def usage(root):
 total=logical=count=0
 for parent,dirs,files in os.walk(root,followlinks=False):
  total+=Path(parent).lstat().st_blocks*512
  for name in files:
   s=(Path(parent)/name).lstat();total+=s.st_blocks*512;logical+=s.st_size;count+=1
 return {'allocated_bytes':total,'logical_file_bytes':logical,'files':count}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
progress={'status':'PREPARING','UTC':now(),'only_nodes':NAMES,'completed_roots':[]}
try:
 assert not(R/'USER_FIVE_DATA_CLEANUP_RESULT_20261106.json').exists()
 assert not(R/'USER_FIVE_DATA_CLEANUP_PROGRESS_20261106.json').exists(),'Reconcile any partial deletion; do not blindly rerun'
 stop=read('USER_THREE_STOP_RECEIPT_20261106.json');assert stop['status']=='PASS'and stop['selected_new_three']==NEW and stop['already_banned_two']==OLD
 def stopped():
  values={n:state(unit(n))for n in NAMES};assert all(x['ActiveState']=='inactive'and x['MainPID']=='0'and x['UnitFileState']=='disabled'for x in values.values());return values
 stopped();before_info=rpc('get_info');assert before_info['chain_id']==CHAIN
 anchor=rpc('get_block',{'block_num_or_id':before_info['last_irreversible_block_num']});assert anchor['id']==before_info['last_irreversible_block_id']
 before_fs=os.statvfs(R);before_free=before_fs.f_bavail*before_fs.f_frsize;before_mem=subprocess.check_output(['free','-m'],text=True)
 pre=read('USER_THREE_STOP_FIVE_DATA_PREFLIGHT_20261106.json');expected={n:[Path(x['path'])for x in pre['data_roots'][n]]for n in NAMES};assert sum(map(len,expected.values()))==6
 safety_root=R/'private/retired-node-safety-20261106';assert not safety_root.exists();safety_root.mkdir(mode=0o700)
 configs={};records=[]
 for name in NAMES:
  base=C/'bps'/name if name in OLD else R/'nodes'/name
  config=base/'config/config.ini';assert config.is_file()and not config.is_symlink();configs[name]={'path':str(config),'sha256':sha(config)}
  for root in expected[name]:
   assert root.name in['data','data-checkpoint']and root.parent==base and root.resolve()==root and not root.is_symlink()and root.is_dir()
   original=root.stat();safety=[]
   for parent,dirs,files in os.walk(root,followlinks=False):
    assert not any(os.path.ismount(Path(parent)/x)for x in dirs),'Unexpected mount in selected dataset'
    for filename in files:
     file=Path(parent)/filename
     if 'safety'in filename.lower()or'finalizer'in filename.lower():
      assert file.relative_to(root)==Path('finalizers/safety.dat')and not file.is_symlink()and file.is_file()and file.stat().st_size<1024*1024,'Review unexpected safety-related file before deleting'
      fs=file.stat();dest=safety_root/name/root.name/file.relative_to(root);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,dest);dest.chmod(0o600)
      safety.append({'relative_path':str(file.relative_to(root)),'private_backup':str(dest),'sha256':sha(file),'bytes':fs.st_size,'uid':fs.st_uid,'gid':fs.st_gid,'mode':stat.S_IMODE(fs.st_mode)})
   records.append({'node':name,'path':str(root),'before':usage(root),'uid':original.st_uid,'gid':original.st_gid,'mode':stat.S_IMODE(original.st_mode),'safety':safety})
 assert sum(len(x['safety'])for x in records)==5
 public_plan=[{k:v for k,v in x.items()if k!='safety'}|{'safety':[{'relative_path':v['relative_path'],'sha256':v['sha256'],'bytes':v['bytes']}for v in x['safety']]}for x in records]
 progress.update(status='DELETING_ONLY_VERIFIED_STOPPED_DATA_ROOTS',plan=public_plan);save('USER_FIVE_DATA_CLEANUP_PROGRESS_20261106.json',progress)
 for row in records:
  stopped();root=Path(row['path']);assert root.resolve()==root and root.parent.name==row['node']
  progress.update(current_root=str(root),UTC=now());save('USER_FIVE_DATA_CLEANUP_PROGRESS_20261106.json',progress)
  shutil.rmtree(root);root.mkdir(mode=row['mode']);os.chown(root,row['uid'],row['gid'])
  for item in row['safety']:
   dest=root/item['relative_path'];dest.parent.mkdir(parents=True,exist_ok=True);os.chown(dest.parent,item['uid'],item['gid']);shutil.copy2(item['private_backup'],dest);os.chown(dest,item['uid'],item['gid']);dest.chmod(item['mode']);assert sha(dest)==item['sha256']
  remaining=[str(p.relative_to(root))for p in root.rglob('*')if p.is_file()or p.is_symlink()];assert sorted(remaining)==sorted(x['relative_path']for x in row['safety'])
  row['after']=usage(root);progress['completed_roots'].append(str(root));save('USER_FIVE_DATA_CLEANUP_PROGRESS_20261106.json',progress)
 assert all(sha(Path(v['path']))==v['sha256']for v in configs.values())
 after_info=rpc('get_info');assert after_info['chain_id']==CHAIN and rpc('get_block',{'block_num_or_id':anchor['block_num']})['id']==anchor['id']
 after_fs=os.statvfs(R);after_free=after_fs.f_bavail*after_fs.f_frsize
 result={'status':'PASS','UTC':now(),'chain_id':CHAIN,'only_nodes':NAMES,'new_stopped_three':NEW,'already_banned_two':OLD,'node_processes':stopped(),'datasets':[{k:v for k,v in row.items()if k!='safety'}|{'safety':[{'relative_path':s['relative_path'],'sha256':s['sha256'],'bytes':s['bytes']}for s in row['safety']]}for row in records],'dataset_allocated_bytes_before':sum(x['before']['allocated_bytes']for x in records),'dataset_allocated_bytes_after':sum(x['after']['allocated_bytes']for x in records),'filesystem_available_bytes_before':before_free,'filesystem_available_bytes_after':after_free,'filesystem_net_available_delta':after_free-before_free,'memory_before':before_mem,'memory_after':subprocess.check_output(['free','-m'],text=True),'unchanged_config_hashes':configs,'before_info':before_info,'after_info':after_info,'retained_canonical_anchor':{'block_num':anchor['block_num'],'block_id':anchor['id']},'scope':'User explicitly authorized removal of all block/state datasets of these5nodes. Six datasets cleared including Tokyo initial replay. Only five265-byte finalizer safety files retained/restored, plus tiny private safety copies; no raw block/state backup. Configuration/keys/chain registration/votes/bonds retained. Other nodes, shared bootstrap checkpoint, current chain and history were not deleted. New3bans await native rules; no unban, force ban, settlement or key/safety reset.'}
 save('USER_FIVE_DATA_CLEANUP_RESULT_20261106.json',result);progress.update(status='PASS',UTC=now());save('USER_FIVE_DATA_CLEANUP_PROGRESS_20261106.json',progress)
 print('FIVE_NODE_SIX_DATASETS_CLEARED',json.dumps({'allocated_removed_bytes':result['dataset_allocated_bytes_before']-result['dataset_allocated_bytes_after'],'disk_available_bytes':after_free,'filesystem_net_delta':after_free-before_free,'retained_safety_bytes':1325}),flush=True)
except BaseException:
 save('USER_FIVE_DATA_CLEANUP_FAILURE_20261106.json',{'status':'FAIL','UTC':now(),'progress':progress,'error':traceback.format_exc()});raise
