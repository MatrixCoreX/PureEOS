"""Actual owned TEST registrations for canonical K1, legacy EOS and split K1/R1."""
from pathlib import Path
import datetime,json,subprocess,traceback
from campaign_helpers import R,load_chain,import_key,action,transfer
f=load_chain();assert json.loads((R/'CREATION_EXECUTION_RESULT_r2.json').read_text())['status']=='FOUR_CREATIONS_DEPLOYED_IRREVERSIBLE'
P=R/'PUBLIC_SIGNUP_PROGRESS.json';assert not P.exists(),'Reconcile native account existence and exact receipts before resuming'
report={'status':'RUNNING','chain_id':f.rpc('get_info')['chain_id'],'cases':[],'checks':[]}
def save():P.write_text(json.dumps(report,indent=2)+'\n')
def check(ok,label):assert ok,label;report['checks'].append(label);save();print('PASS '+label,flush=True)
def balance(name):
 row=f.rpc('get_currency_balance',{'code':'eosio.token','account':name,'symbol':'AIC'});return row[0]if row else'0.0000 AIC'
def absent(name):
 try:f.rpc('get_account',{'account_name':name})
 except RuntimeError as e:assert 'unknown key'in str(e).lower()or'account'in str(e).lower();return True
 return False
def r1():
 q=subprocess.run([str(R/'native-client/cleos'),'create','key','--r1','--to-console'],capture_output=True,text=True,check=True)
 k={a.strip():b.strip()for l in q.stdout.splitlines()if ':'in l for a,b in[l.split(':',1)]};f.secrets.append(k['Private key']);assert k['Public key'].startswith('PUB_R1_');return k
try:
 save();keys={'canonical':f.key(),'legacy':f.key(),'dual_owner':f.key(),'dual_active':r1()};(R/'private/public-signup-demo-keys.json').write_text(json.dumps(keys));(R/'private/public-signup-demo-keys.json').chmod(0o600)
 for k in keys.values():import_key(f,k)
 legacy=f.run(['convert','legacy_public_key',keys['legacy']['Public key']]).strip();assert legacy.startswith('EOS')
 cases=[('signupkoneai',keys['canonical'],keys['canonical'],'signupkoneai-'+keys['canonical']['Public key']),('signuplegacy',keys['legacy'],keys['legacy'],'signuplegacy '+legacy),('signupdualai',keys['dual_owner'],keys['dual_active'],'signupdualai-'+keys['dual_owner']['Public key']+'-'+keys['dual_active']['Public key'])]
 for name,owner,active,memo in cases:
  assert absent(name);before=balance('eosio.signup');result=f.tx(['transfer','gptgenesisbp','eosio.signup','0.2500 AIC',memo]);acct=f.rpc('get_account',{'account_name':name});perms={p['perm_name']:p['required_auth']for p in acct['permissions']}
  check(int(acct['ram_quota'])>=4096 and not acct['privileged'],'Actual registered account RAM and nonprivileged '+name)
  for role,k in [('owner',owner),('active',active)]:
   stored=perms[role];assert stored['threshold']==1 and not stored['accounts']and not stored['waits']and len(stored['keys'])==1
   expected=f.run(['convert','public_key',k['Public key']]).strip();actual=f.run(['convert','public_key',stored['keys'][0]['key']]).strip();check(expected==actual and stored['keys'][0]['weight']==1,'Exact registered '+role+' '+name)
  stake=f.table('delband',scope=name);check(len(stake)==1 and stake[0]['from']==stake[0]['to']==name and stake[0]['net_weight']=='0.1000 AIC'and stake[0]['cpu_weight']=='0.1000 AIC','New account owns both resource stakes '+name)
  check(balance('eosio.signup')==before,'Full registration payment consumed by owned stakes and actual RAM '+name)
  registration=f.report['confirmed_transactions'][-1]
  # Resource service is separately owner-authorized; do not describe signup stake as guaranteed transaction capacity.
  f.self_transact('owned-signup-self-PowerUp-'+name,[transfer('gptgenesisbp',name,'1.0000 AIC','Owned TEST signer/own PowerUp QA'),action('powerup',name,{'payer':name,'receiver':name,'days':30,'net_frac':2000000000000,'cpu_frac':2000000000000,'max_payment':'1.0000 AIC'})])
  signed=f.tx(['transfer',name,'gptgenesisbp','0.0001 AIC','Owned registered-account active signature QA']);packet=max((R/'founder/immutable-packets').glob('*.json'),key=lambda p:int(p.stem));signatures=json.loads(packet.read_text())['signatures']
  if active['Public key'].startswith('PUB_R1_'):check(any(s.startswith('SIG_R1_')for s in signatures),'Actual native R1 active signature accepted')
  report['cases'].append({'account':name,'owner_public_key':owner['Public key'],'active_public_key':active['Public key'],'registration':registration,'active_signature_transaction':f.report['confirmed_transactions'][-1],'active_signature_types':[s.split('_')[1]for s in signatures],'RAM_quota':acct['ram_quota'],'resource_stake':stake});save()
 for name,memo,quantity,error in [('signuplowram','signuplowram-'+keys['canonical']['Public key'],'0.2001 AIC','minimum RAM allocation'),('signupbadkey','signupbadkey-'+keys['canonical']['Public key'][:-1]+('1'if keys['canonical']['Public key'][-1]!='1'else'2'),'0.2500 AIC','checksum')]:
  before=balance('gptgenesisbp');vault=balance('eosio.signup');f.tx(['transfer','gptgenesisbp','eosio.signup',quantity,memo],failure=error);check(absent(name)and balance('gptgenesisbp')==before and balance('eosio.signup')==vault,'Native registration rollback '+name)
 before=balance('gptgenesisbp');f.tx(['transfer','gptgenesisbp','eosio.signup','0.2500 AIC',cases[0][3]],failure='already registered');check(balance('gptgenesisbp')==before,'Duplicate account rejects without charging')
 report.update(status='PASS',finished_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat(),formats=['PUB_K1','legacy EOS','split PUB_K1 owner / PUB_R1 active'],scope='Actual public owned TEST registrations/signatures; no post-quantum key claim, no external wallet compatibility claim')
 save();(R/'PUBLIC_SIGNUP_RESULT.json').write_text(json.dumps(report,indent=2)+'\n');print('PUBLIC_SIGNUP_K1_LEGACY_R1_PASS',flush=True)
except BaseException:
 report.update(status='NEEDS_CANONICAL_RECONCILIATION',error=f.redact(traceback.format_exc()));save();raise
