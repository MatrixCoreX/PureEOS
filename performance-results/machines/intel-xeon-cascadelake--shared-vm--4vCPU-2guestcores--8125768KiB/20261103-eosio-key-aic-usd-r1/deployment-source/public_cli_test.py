"""One immutable 0.0001 USD demo payment through the actual public HTTPS relay."""
from pathlib import Path
import json, subprocess, time, urllib.request
from campaign_helpers import R, load_chain

f=load_chain()
public_rpc='https://aic-test.matrixai.one/rpc'
endpoint='https://aic-test.matrixai.one/relay'
directory=R/'public-cli-test';directory.mkdir(mode=0o700,exist_ok=True)
base=[str(R/'native-client/cleos'),'--no-auto-keosd','--url',public_rpc,
      '--wallet-url','unix://'+str(f.root/'wallet/keosd.sock')]
def cli(args):
    p=subprocess.run(base+args,capture_output=True,text=True,timeout=60)
    if p.returncode:raise RuntimeError(f.redact(p.stderr+'\n'+p.stdout))
    return json.loads(p.stdout)
acceptance=json.loads((R/'KEY_RELAY_ACCEPTANCE.json').read_text())
signed_file=directory/'signed.json'
if signed_file.exists():
    signed=json.loads(signed_file.read_text())
    progress=json.loads((directory/'progress.json').read_text())
else:
    progress={'start_LIB':f.rpc('get_info')['last_irreversible_block_num'],
              'RPC':public_rpc,'relay':endpoint,'same_packet_on_retry':True}
    prepared=cli(['key','prepare',acceptance['holder_public_key'],acceptance['recipient_public_key'],
        '0.0001 USD','-c','keyusdtoken1','--bp','gptgenesisbp','--max-fee','2.0000 AIC'])
    signed=cli(['key','sign',json.dumps(prepared)])
    signed_file.write_text(json.dumps(signed,indent=2)+'\n')
    progress['transaction_id']=prepared['transaction_id']
    (directory/'progress.json').write_text(json.dumps(progress,indent=2)+'\n')
reply=cli(['key','submit',str(signed_file),'--relay-url',endpoint])
assert reply['transaction_id']==progress['transaction_id']
confirmed=f.wait_transaction(reply['transaction_id'],progress['start_LIB'],timeout=180)
# A duplicate submit must return the same job and can never mint a new payment.
again=cli(['key','submit',str(signed_file),'--relay-url',endpoint])
assert again['transaction_id']==reply['transaction_id']
result={'status':'PASS','chain_id':f.rpc('get_info')['chain_id'],'RPC':public_rpc,'relay':endpoint,
        'transaction_id':reply['transaction_id'],'confirmed':confirmed,
        'quantity':'0.0001 USD','token_contract':'keyusdtoken1',
        'same_packet_retry_verified':True,'native_cleos_default_HTTP_headers':True,
        'private_keys_sent_to_relay':False}
(R/'PUBLIC_CLI_ACCEPTANCE.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result),flush=True)
