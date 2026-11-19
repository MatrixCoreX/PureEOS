from pathlib import Path
import subprocess,sys,json,datetime
D=Path(__file__).resolve().parent;R=D.parents[1];S=R/'release-work-20261102/cheap-vps-35bp-history-r1';source=D/sys.argv[1];output=D/sys.argv[2];assert source.is_file()and not output.exists()
ssh=['ssh','-i',str(S/'ssh-key'),'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(S/'known_hosts'),'-o','ConnectTimeout=15','-o','ServerAliveInterval=10','-o','ServerAliveCountMax=2','root@66.245.194.132','python3','-']
try:
 q=subprocess.run(ssh,input=source.read_bytes(),capture_output=True,timeout=180);output.write_bytes(q.stdout);output.with_suffix(output.suffix+'.stderr').write_bytes(q.stderr);print(json.dumps({'rc':q.returncode,'bytes':len(q.stdout),'stderr_bytes':len(q.stderr),'output':str(output)}));assert q.returncode==0
except subprocess.TimeoutExpired as e:
 output.write_bytes(e.stdout or b'');output.with_suffix(output.suffix+'.stderr').write_bytes(e.stderr or b'');raise
