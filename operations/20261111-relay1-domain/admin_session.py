"""New nativeRAM functional-QA native signer. One frozen packet per label; no replay."""
from pathlib import Path
import http.client,json,os,socket,subprocess,time
import ops_common as o
class AdminSession:
 def __init__(self):
  self.P=o.R/'relay-admin-private';self.P.mkdir(mode=0o700,exist_ok=True);self.W=Path('/opt/rly1-admin-20261111');self.W.mkdir(mode=0o700,exist_ok=True)
  self.socket=self.W/'keosd.sock';self.cleos=Path('/opt/pureeos-keynet-20261103-r2/native-client/cleos');self.child=None
 def wallet(self,method,data):
  class UnixHTTP(http.client.HTTPConnection):
   def connect(c):c.sock=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);c.sock.settimeout(10);c.sock.connect(str(self.socket))
  c=UnixHTTP('localhost');c.request('POST','/v1/wallet/'+method,''if method in ['list_wallets','get_public_keys']else json.dumps(data),{'Content-Type':'application/json'});r=c.getresponse();raw=r.read();c.close();assert 200<=r.status<300,'Owned QA wallet refusal';return json.loads(raw)if raw else None
 def run(self,args):
  p=subprocess.run([str(self.cleos),'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(self.socket),*args],capture_output=True,text=True,timeout=90)
  if p.returncode:(self.P/'native-client-error.log').write_text(p.stdout+p.stderr)
  assert p.returncode==0,'Owned QA native refusal; preserve packet and reconcile privately';return p.stdout
 def __enter__(self):
  o.guard();o.require_codes(o.NEW);role=json.loads(Path('/opt/pureeos-devnet-20261105-r1/private/bootstrap-keys.json').read_text())['accounts']['gptgenesisbp']
  active=next(p['required_auth']for p in o.rpc('get_account',{'account_name':'gptgenesisbp'})['permissions']if p['perm_name']=='active')
  assert active['threshold']==1 and len(active['keys'])==1 and not active['accounts']and not active['waits']and active['keys'][0]['key']==role['Public key']
  self.log=(self.P/'keosd.log').open('a');self.child=subprocess.Popen(['/opt/pureeos-dev-account-genesis-20261105-r1/runtime-5/bin/keosd','--data-dir',str(self.W),'--config-dir',str(self.W),'--http-server-address',''],stdout=self.log,stderr=subprocess.STDOUT)
  deadline=time.monotonic()+30
  while True:
   assert self.child.poll()is None and time.monotonic()<deadline
   try:self.wallet('list_wallets',[]);break
   except(OSError,http.client.HTTPException):time.sleep(.1)
  password=self.W/'password.txt';assert not password.exists(),'New isolated signer only; do not replay'
  self.run(['wallet','create','-n','relay1admin','--file',str(password)]);self.wallet('import_key',['relay1admin',role['Private key']]);role=None
  assert len(self.wallet('get_public_keys',[]))==1
  return self
 def __exit__(self,*args):
  if self.child and self.child.poll()is None:self.child.terminate();self.child.wait(timeout=20)
  self.log.close()
 def key(self):
  raw=self.run(['create','key','--to-console']);pair=dict(line.split(': ',1)for line in raw.splitlines()if ': 'in line);assert set(pair)=={'Private key','Public key'};return pair
 def send(self,label,actions):
  path=self.P/(label+'-signed.json');progress=o.R/'admin-packets'/(label+'.json');assert not path.exists()and not progress.exists(),'Reconcile immutable original packet; never repeat'
  unsigned=self.P/(label+'-unsigned.json');o.save(unsigned,{'actions':actions});signed=json.loads(self.run(['push','transaction',str(unsigned),'-j','--expiration','300','--dont-broadcast','--compression','zlib']));o.save(path,signed)
  packed=json.loads(self.run(['convert','pack_transaction',str(path)]));assert packed['compression']in [0,'none'];tid=o.sha(bytes.fromhex(packed['packed_trx']));start=o.guard()['last_irreversible_block_num']
  record={'status':'SIGNED_IMMUTABLE','transaction_id':tid,'packet_SHA256':o.sha(path.read_bytes()),'start_LIB':start,'expiration':signed['expiration']};o.save(progress,record)
  p=subprocess.run([str(self.cleos),'--no-auto-keosd','--url','http://127.0.0.1:8888','--wallet-url','unix://'+str(self.socket),'push','transaction',str(path),'-s','-j','--compression','zlib'],capture_output=True,text=True,timeout=90)
  (self.P/(label+'-response.log')).write_text(p.stdout+p.stderr)
  # Even an ambiguous/CPU response can have canonical success. Never re-sign or
  # submit a second packet; inspect the original ID before deciding the result.
  receipt=o.receipt(tid,start,timeout=180);record.update(status='IRREVERSIBLE',canonical_receipt=receipt,response_exit=p.returncode);o.save(progress,record);return{'transaction_id':tid,'canonical_receipt':receipt,'execution':json.loads(p.stdout)if p.returncode==0 else None}
