from pathlib import Path
import json
R=Path('/opt/pureeos-testnet-bank-issuance-20261119-r1');p=R/'private/propose/wallet-error.log';x=json.loads(p.read_text())
error=x.get('error',{});r={'error':{k:error.get(k)for k in ['code','name','what']},'detail_formats':[d.get('message',d.get('format','')).split('\n')[0][:160]for d in error.get('details',[])],'exists':{n:(R/n).exists()for n in ['private/propose/intent.json','private/propose/signed.json','private/propose/packed.json','transactions/propose.json','PROPOSAL.json']},'signer_closed':json.loads((R/'SIGNER_propose_CLOSED.json').read_text())};print(json.dumps(r))
