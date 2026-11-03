"""Expose the separate relay and native transaction RPC on the existing TLS site."""
from pathlib import Path
import hashlib, json, shutil, subprocess

R=Path('/opt/pureeos-keynet-20261103-r1')
config=Path('/etc/nginx/sites-available/aic-test')
backup=R/'archive/nginx-aic-test-before-keynet.conf'
assert not backup.exists(),'Endpoint publication is already recorded; reconcile before repeating'
shutil.copy2(config,backup)
limits=Path('/etc/nginx/conf.d/pureeos-keynet-limits.conf')
assert not limits.exists()
limits.write_text('limit_req_zone $binary_remote_addr zone=keynet_rpc:10m rate=10r/s;\n'
                  'limit_req_zone $binary_remote_addr zone=keynet_relay:10m rate=5r/s;\n')
text=config.read_text()
anchor='  location /api/v1/chain/ {'
assert text.count(anchor)==1 and 'location /relay/'not in text and 'location /rpc/'not in text
addition='''  location /relay/ {
    client_max_body_size 8k;
    limit_req zone=keynet_relay burst=20 nodelay;
    limit_req_status 429;
    proxy_pass http://127.0.0.1:8787/;
    proxy_http_version 1.1;
    proxy_set_header Host $proxy_host;
    proxy_read_timeout 30s;
    add_header Cache-Control "no-store" always;
  }
  location /rpc/v1/chain/ {
    client_max_body_size 8m;
    limit_req zone=keynet_rpc burst=40 nodelay;
    limit_req_status 429;
    limit_except POST { deny all; }
    proxy_pass http://127.0.0.1:8888/v1/chain/;
    proxy_http_version 1.1;
    proxy_set_header Host $proxy_host;
    proxy_set_header X-Forwarded-Proto https;
    proxy_read_timeout 30s;
    add_header Cache-Control "no-store" always;
  }
  location /rpc/ { return 404; }
'''
config.write_text(text.replace(anchor,addition+anchor))
check=subprocess.run(['nginx','-t'],capture_output=True,text=True)
if check.returncode:
    shutil.copy2(backup,config);limits.unlink()
    raise RuntimeError(check.stderr)
subprocess.run(['systemctl','reload','nginx'],check=True)
receipt={'status':'PUBLISHED','read_only_RPC':'https://aic-test.matrixai.one/api',
         'transaction_RPC':'https://aic-test.matrixai.one/rpc','relay':'https://aic-test.matrixai.one/relay',
         'history':'https://aic-test.matrixai.one/history','old_config_SHA256':hashlib.sha256(backup.read_bytes()).hexdigest(),
         'new_config_SHA256':hashlib.sha256(config.read_bytes()).hexdigest(),'nginx_validation':check.stderr,
         'nodeos_HTTP_plugin_modified':False,'signing_wallet_exposed':False}
(R/'ENDPOINTS.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps(receipt),flush=True)
