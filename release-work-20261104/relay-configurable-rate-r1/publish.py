"""Publish the verified static explorer while retaining its rollback release."""
from pathlib import Path
import datetime, hashlib, json, shutil, urllib.request

K = Path('/opt/pureeos-key-rate-20261104-r1')
F = Path('/opt/pureeos-explorer-key-rate-20261104-r3')
build = json.loads((F / 'BUILD_RESULT.json').read_text())
assert build['status'] == 'PASS'
preview = json.loads((F / 'browser-preview/RESULT.json').read_text())
assert preview['status'] == 'PASS' and preview['build_files'] == build['files']
integration = json.loads((F / 'INTEGRATION_RESULT.json').read_text())
assert integration['status'] == 'PASS' and integration['SDK_comparisons'] == 60
registration = json.loads((K / 'DISPLAY_RELAYS.json').read_text())
assert registration['status'] == 'DISPLAY_RELAYS_CONFIGURED'
assert registration['new_HTTP_services'] == 0 and len(registration['registered']) == 5
assert json.loads((K / 'EXECUTION.json').read_text())['status'] == 'UPGRADED_IRREVERSIBLE'
digest = lambda root: {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in root.rglob('*') if p.is_file()}
root = Path('/var/www/aic-test')
current = root / 'current'
old = current.resolve()
assert str(old) == '/var/www/aic-test/releases/20261103T095548Z-fee-estimate'
assert digest(old) == json.loads(Path('/opt/pureeos-explorer-fee-estimate-20261103-r1/BUILD_RESULT.json').read_text())['files']
request = urllib.request.Request('http://127.0.0.1:8890/v1/chain/get_info', data=b'{}', headers={'Content-Type': 'application/json'})
info = json.load(urllib.request.urlopen(request, timeout=20))
assert info['chain_id'] == '116ea0c3c556951ba0a9244cbfbfe13f98972f3353c38ff38c35122d0f61c5a0'
source = F / 'source/dist'
assert digest(source) == build['files']
release = root / 'releases' / (datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-relay-rates')
shutil.copytree(source, release)
for p in [release, *release.rglob('*')]: p.chmod(0o755 if p.is_dir() else 0o644)
link = root / 'current-relay-rates-next'
assert not link.exists() and not link.is_symlink()
link.symlink_to(release)
link.replace(current)
result = {'status': 'DEPLOYED', 'UTC': datetime.datetime.now(datetime.timezone.utc).isoformat(),
          'release': str(release), 'rollback': str(old), 'chain_id': info['chain_id'],
          'files': build['files'], 'scope': 'Public-key account introduction, real fee-rate selector and formulas; four inactive comparison entries, sole founder HTTP'}
(F / 'DEPLOYMENT.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({'status': result['status'], 'release': str(release)}))
