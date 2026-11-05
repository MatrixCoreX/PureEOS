"""Local-only static preview with fixed read-only current-chain forwarding."""
from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import urllib.request,urllib.error
ROOT=Path('/opt/pureeos-bp-demo-six-20261105-r1/frontend-final/dist')
READ={'get_info','get_block','get_account','get_abi','get_code_hash','get_upgrade_state','get_system_account_state','get_table_rows','get_producers','get_producer_schedule','get_consensus_parameters','get_transaction_status','get_activated_protocol_features','get_table_by_scope','get_currency_balance','get_currency_stats','get_finalizer_info'}
class Handler(SimpleHTTPRequestHandler):
 def __init__(self,*args,**kw):super().__init__(*args,directory=str(ROOT),**kw)
 def relay(self,url,body=None):
  q=urllib.request.Request(url,data=body,headers={'Content-Type':'application/json'})
  try:
   with urllib.request.urlopen(q,timeout=30)as r:status=r.status;raw=r.read();kind=r.headers.get('Content-Type','application/json')
  except urllib.error.HTTPError as e:status=e.code;raw=e.read();kind='application/json'
  self.send_response(status);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(raw)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(raw)
 def do_POST(self):
  name=self.path.removeprefix('/api/v1/chain/')
  if self.path.startswith('/api/v1/chain/')and name in READ:return self.relay('http://127.0.0.1:8890/v1/chain/'+name,self.rfile.read(int(self.headers.get('Content-Length','0'))))
  if self.path=='/history/v1/chain/get_info':return self.relay('http://127.0.0.1:7000/v1/chain/get_info',self.rfile.read(int(self.headers.get('Content-Length','0'))))
  self.send_error(405)
 def do_GET(self):
  route=self.path.split('?')[0];companions={'/history/v2/history/get_rotations':9051,'/history/v2/state/get_producer_summary':9051,'/history/v2/state/get_producer_votes':9053,'/history/v2/stats/get_peak_tps':9052}
  if route in companions:return self.relay('http://127.0.0.1:'+str(companions[route])+'/'+self.path[len('/history/'):])
  if self.path.startswith('/history/v2/')or self.path=='/history/v1/chain/get_info':return self.relay('http://127.0.0.1:7000/'+self.path[len('/history/'):])
  super().do_GET()
ThreadingHTTPServer(('127.0.0.1',9078),Handler).serve_forever()
