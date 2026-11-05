"""Prepare concrete unsigned exact reviews. Publication must precede submission."""
from pathlib import Path
import argparse,datetime,hashlib,importlib.util,json,math,os,shutil,subprocess
from campaign_helpers import R,V,load_chain
C=Path('/opt/pureeos-dev-account-genesis-20261105-r1');K=Path('/opt/pureeos-key-atomic-ram-20261105-r1/attempt-14');P=R/'review'
assert json.loads((R/'FIRST_PHASES_AUDIT.json').read_text())['status']=='PASS'
assert not P.exists(),'Review inputs are immutable; reconcile existing staged review'
f=load_chain();info=f.rpc('get_info');state=f.rpc('get_system_account_state');assert state['latest_proposal_id']==0 and state['pending_count']==0
P.mkdir();art=P/'artifacts';art.mkdir()
spec=importlib.util.spec_from_file_location('bundle',C/'source/tools/governance/system_account_bundle.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
items=[('eosio.key',K/'artifacts','eosio.key',2),('usd',C/'real-contracts','custody.token',3),('cny',C/'real-contracts','custody.token',3),('eosio.signup',C/'real-contracts','eosio.signup',1)]
expected={'eosio.key':'e006128f25fcda3f2395302212247b436b550d8ffde0f7da3064b8cec624a442','custody.token':'ec9e8326c1fef0b561b2b5c45ad5c05628bd172c2afdedfd6f24fa34178354bb','eosio.signup':'996d520ce32c01da431334fc7bf1f03755f4c82f7ae73ddf8b9d931054ae35f3'}
market=f.table('rammarket')[0]
def atoms(text):
 whole,frac=text.split()[0].split('.');return int(whole)*10000+int(frac)
reserve=int(market['base']['balance'].split()[0]);quote=atoms(market['quote']['balance']);rows=[]
for i,(target,source,stem,version)in enumerate(items,1):
 wasm=art/(stem+'.wasm');abi=art/(stem+'.abi');raw=art/(stem+'.raw.abi')
 if not wasm.exists():
  shutil.copy2(source/(stem+'.wasm'),wasm);shutil.copy2(source/(stem+'.abi'),abi)
  assert hashlib.sha256(wasm.read_bytes()).hexdigest()==expected[stem]
  packed=json.loads(f.run(['convert','pack_hex','--type','abi_def',str(abi)]));assert isinstance(packed,str)
  raw.write_bytes(bytes.fromhex(packed))
  if stem!='eosio.key':assert raw.read_bytes()==(C/'real-contracts'/(stem+'.raw.abi')).read_bytes()
 initial=None
 if version==2:
  initial={'chain_id':info['chain_id'],'deployment':hashlib.sha256(os.urandom(32)).hexdigest(),'fee_contract':'eosio.token','fee_symbol':'4,AIC','system_hash':f.rpc('get_code_hash',{'account_name':'eosio'})['code_hash'],'policy':{'minimum_fee':100,'unit_fee':10,'new_row_fee':500,'maximum_multiplier_bps':100000,'epoch_seconds':1,'max_wallets':1000,'max_balances':4000,'max_assets':32,'max_native_payers':1000}}
 elif version==3:
  symbol=target.upper();initial={'recipient':'gptgenesisbp','maximum_supply':'1000000.0000 '+symbol,'initial_supply':'10000.0000 '+symbol}
 init_file=None
 if initial is not None:
  (P/(target+'.init.json')).write_text(json.dumps(initial,indent=2)+'\n')
  init_file=P/(target+'.init.bin');init_file.write_bytes(bytes.fromhex(json.loads(f.run(['convert','pack_hex','--type','init','--abi-file',str(abi),json.dumps(initial)]))))
 # Code multiplier plus ABI and64KiB init/scratch;20% RAM-price headroom.
 # No recipient pool: future missing records buy RAM in each atomic transfer.
 bytes_needed=wasm.stat().st_size*10+raw.stat().st_size+65536
 net=(quote*bytes_needed+reserve-bytes_needed-1)//(reserve-bytes_needed)+1
 quote_atoms=(net*200+198)//199;payment=(quote_atoms*120+99)//100+100
 assert 0<payment<=100000000
 metadata={'target':target,'chain_id':info['chain_id'],'expected_proposal_id':i,'authority_version':version,'RAM_payment_atoms':payment,'RAM_minimum_bytes_estimate':bytes_needed,'RAM_quote_atoms':quote_atoms,'RAM_market':market,'code_SHA256':hashlib.sha256(wasm.read_bytes()).hexdigest(),'JSON_ABI_SHA256':hashlib.sha256(abi.read_bytes()).hexdigest(),'raw_ABI_SHA256':hashlib.sha256(raw.read_bytes()).hexdigest(),'initializer':initial,'initializer_SHA256':hashlib.sha256(init_file.read_bytes()).hexdigest()if init_file else None,'review_seconds':1800,'sponsor':'gptgenesisbp','developer':'eosio.dev','native_SHA256':json.loads((R/'PLAN.json').read_text())['nodeos_SHA256']}
 ownership='Ordinary issuer: owner eosio.dev@owner; active eosio.dev@active OR own eosio.code; nonprivileged. Legitimate owner may manage only this ordinary account.'if version==3 else'Sealed system service: unsatisfiable owner and own-code active only; no external management key or BP/dev direct takeover.'
 review='''# 公开 TEST 创建/部署审核 / Public TEST creation/deployment review

此文件和附带的精确WASM、原始ABI及初始化是审核对象。公示完整1800链秒，BP按冻结有效选民的原生三分之二门槛否决；每项独立执行，不自动升级。
This review binds exact WASM, raw ABI and initialization. Each proposal waits1800chain seconds and independently checks the native two-thirds effective-BP veto threshold. No automatic execution.

旧链记录单独保留；本文件仅针对新的 Chain ID。创始人使用已发行余额赞助这次创建所需代码/ABI/少量运行RAM，不增发AIC，不预置大型收款池。预算为市场反算加20%价格余量及0.0100AIC；整个已审付款用于实际原生买RAM。随后KEY缺失收款行在每次转账内自动购RAM，并由发送人承担实际费用，收款本金全额。
Old history remains separate. Founder sponsors code/ABI/small runtime RAM from existing issued funds. The reviewed budget is the market quote plus20% price headroom and0.0100AIC; all of it buys actual native RAM. Future missing KEY records are funded by sender fees in each atomic transfer, preserving full principal.

所有权 / Ownership: '''+ownership+'''

权限边界：BP治理只否决；不新增强改普通用户权限或资产动作。dev不获通用特权。KEY为封印托管例外。USD/CNY是模拟第三方资产，无现实法币储备承诺，普通合法发行人保留其自身账号管理能力，不控制AIC或KEY。可见代码与BP否决是治理保障，不是任意代码良性的自动证明。
BP governance is veto-only. No forced ordinary-user authority or asset mutation action is added, and dev receives no universal privilege. KEY remains sealed custody. USD/CNY simulate third-party assets without a fiat reserve claim; their authorized holder manages only those issuer accounts. Visible code review and veto are governance safeguards, not a proof that arbitrary code is benign.

验证依据：native57、私有canary2、实际四项RPC；KEY候选237原生+26SDK。公开部署后仍须实测三方向/三币种、费用/RAM/回滚/历史/浏览器，不能用私有结果冒充公开结果。
Qualification: native57, private canary2 and real four-row RPC; KEY237native+26SDK. Actual public route/token/fee/RAM/rollback/history/browser checks remain required after deployment.

## 精确字段 / Exact fields

```json
'''+json.dumps(metadata,ensure_ascii=False,indent=2)+'\n```\n'
 review_file=P/(target+'.review.md');review_file.write_text(review)
 args=argparse.Namespace(target=target,payer='gptgenesisbp',developer='eosio.dev',ram_atoms=payment,proposal_id=i,wasm=wasm,raw_abi=raw,review=review_file,init_data=init_file,ordinary_issuer=version==3,chain_id=info['chain_id'],output=P/(target+'.bundle.json'))
 h.prepare(args);rows.append(metadata)
files={str(p.relative_to(P)):hashlib.sha256(p.read_bytes()).hexdigest()for p in P.rglob('*')if p.is_file()}
manifest={'status':'EXACT_UNSIGNED_REVIEWS_PREPARED_NOT_SUBMITTED','UTC':datetime.datetime.now(datetime.timezone.utc).isoformat(),'chain_id':info['chain_id'],'proposals':rows,'files':files,'scope':'Must publish exact review/artifacts and verify remote Git before submission; four full1800s reviews; separate executions'}
(P/'MANIFEST.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n');print('FOUR_EXACT_PUBLIC_REVIEWS_PREPARED',flush=True)
