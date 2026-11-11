from pathlib import Path
import json
source=Path(__file__).with_name('publish_git.py').read_text()
exec(compile(source[:source.index('entry=')],'<publication_helpers>','exec'),globals())
assert not(D/'OPS_PUBLICATION.json').exists(),'Reconcile existing publication receipt'
out={'status':'PASS','utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
out['ops']=publish(O,['AGENTS.md','operations/20261111-relay1-domain'],'Move founder relay to independent relay1 domain with irreversible endpoint registration')
(D/'OPS_PUBLICATION.json').write_text(json.dumps(out['ops'],indent=2)+'\n')
exec(compile(source[source.index("base='https://github.com/"):],'<wallet_publication>','exec'),globals())
