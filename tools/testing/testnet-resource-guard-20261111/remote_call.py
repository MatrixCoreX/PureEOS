"""Use the existing pinned TEST SSH identity; save small public outputs locally."""
import json, subprocess, sys
from pathlib import Path
D=Path(__file__).resolve().parent
W=D.parents[1]
S=W/'release-work-20261102/cheap-vps-35bp-history-r1'
ssh=['ssh','-i',str(S/'ssh-key'),'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(S/'known_hosts'),'-o','ConnectTimeout=20','-o','ServerAliveInterval=15','root@66.245.194.132']
source=D/sys.argv[1];output=D/sys.argv[2]
assert source.is_file() and not output.exists(),'Never overwrite a previous receipt'
q=subprocess.run(ssh+['python3','-'],input=source.read_bytes(),capture_output=True,timeout=300)
output.write_bytes(q.stdout)
if q.stderr:output.with_suffix(output.suffix+'.stderr').write_bytes(q.stderr)
print(json.dumps({'returncode':q.returncode,'output':str(output),'bytes':len(q.stdout),'stderr_bytes':len(q.stderr)}))
if q.returncode:print(q.stderr.decode(errors='replace')[-3000:]);sys.exit(q.returncode)
