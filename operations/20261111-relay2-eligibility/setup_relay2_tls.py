"""New independent relay2 vhost/certificate only. Existing vhosts unchanged."""
from pathlib import Path
import subprocess,json,hashlib
R=Path('/opt/pureeos-relay-eligibility-20261111-r1');assert not(R/'TLS_STARTED.json').exists()
old=Path('/etc/nginx/sites-available/aic-test');before=hashlib.sha256(old.read_bytes()).hexdigest();v=Path('/etc/nginx/sites-available/pureeos-relay2');link=Path('/etc/nginx/sites-enabled/pureeos-relay2');assert not v.exists()and not link.exists()
root=Path('/var/www/relay2');root.mkdir(exist_ok=False);(R/'TLS_STARTED.json').write_text(json.dumps({'domain':'relay2.matrixai.one','existing_vhost_SHA256':before})+'\n')
v.write_text('server {\n listen 80;\n server_name relay2.matrixai.one;\n location /.well-known/acme-challenge/ { root /var/www/relay2; }\n location / { return 404; }\n}\n');link.symlink_to(v)
subprocess.run(['nginx','-t'],capture_output=True,check=True);subprocess.run(['systemctl','reload','nginx'],check=True)
p=subprocess.run(['certbot','certonly','--webroot','-w',str(root),'--non-interactive','--agree-tos','--register-unsafely-without-email','-d','relay2.matrixai.one'],capture_output=True,text=True,timeout=240);(R/'certbot.log').write_text(p.stdout+p.stderr);assert p.returncode==0,'New relay2 certificate failed; retain log/config; no old service touched'
limits=Path('/etc/nginx/conf.d/pureeos-relay2-limits.conf');assert not limits.exists();limits.write_text('limit_req_zone $binary_remote_addr zone=pureeos_relay2:1m rate=5r/s;\n')
v.write_text('''server {
 listen 80;
 server_name relay2.matrixai.one;
 location /.well-known/acme-challenge/ { root /var/www/relay2; }
 location / { return 301 https://$host$request_uri; }
}
server {
 listen 443 ssl;
 server_name relay2.matrixai.one;
 ssl_certificate /etc/letsencrypt/live/relay2.matrixai.one/fullchain.pem;
 ssl_certificate_key /etc/letsencrypt/live/relay2.matrixai.one/privkey.pem;
 include /etc/letsencrypt/options-ssl-nginx.conf;
 ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;
 client_max_body_size 8k;
 location / {
  limit_req zone=pureeos_relay2 burst=20 nodelay;
  limit_req_status 429;
  proxy_pass http://127.0.0.1:8788;
  proxy_http_version 1.1;
  proxy_set_header Host $proxy_host;
  proxy_read_timeout 30s;
  add_header Cache-Control "no-store" always;
  add_header Access-Control-Allow-Origin "*" always;
  add_header Access-Control-Allow-Methods "GET, POST, OPTIONS" always;
  add_header Access-Control-Allow-Headers "Content-Type" always;
  if ($request_method = OPTIONS) { return 204; }
 }
}
''')
subprocess.run(['nginx','-t'],capture_output=True,check=True);subprocess.run(['systemctl','reload','nginx'],check=True)
assert hashlib.sha256(old.read_bytes()).hexdigest()==before
(R/'TLS_RESULT.json').write_text(json.dumps({'status':'PASS','domain':'relay2.matrixai.one','certificate_validated':True,'origin_loopback_port':8788,'existing_vhost_unchanged':True},indent=2)+'\n');print('RELAY2_NEW_DOMAIN_TLS_READY')
