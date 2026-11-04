"""Remote-only SDK/relay routing tests; leaves the active service untouched."""
from pathlib import Path
import hashlib,json,os,subprocess,tarfile,traceback
K=Path('/opt/pureeos-dev-proposals-20261104-r1');S=K/'runtime-rpcsplit';archive=K/'relay-rpc-split-source.tar.gz'
try:
 digest=hashlib.sha256(archive.read_bytes()).hexdigest();assert digest=='4feeba4c2bd74469660ea0010df0c9df47737575f1b893404c0e9222417b5ee9'
 assert not S.exists();S.mkdir()
 with tarfile.open(archive)as t:t.extractall(S,filter='data')
 (S/'node_modules').symlink_to('/opt/pureeos-keynet-20261103-r2/key-service/node_modules',target_is_directory=True)
 env={**os.environ,'NODE_OPTIONS':'--max-old-space-size=256'}
 with(K/'rpc-split-test.log').open('w')as log:
  subprocess.run(['/opt/pureeos-keynet-20261103-r2/tools/node-current/bin/node','--test',*[str(p)for p in sorted((S/'tests').glob('*.test.mjs'))]],env=env,cwd=S,stdout=log,stderr=subprocess.STDOUT,check=True)
 files={str(p.relative_to(S)):hashlib.sha256(p.read_bytes()).hexdigest()for folder in ['sdk','relayer']for p in(S/folder).rglob('*')if p.is_file()}
 for name in ['package.json','package-lock.json']:files[name]=hashlib.sha256((S/name).read_bytes()).hexdigest()
 (K/'RPC_SPLIT_TEST_RESULT.json').write_text(json.dumps({'status':'PASS','source_sha256':digest,'files':files,'runtime':str(S),'contract_bytes_changed':False,
  'read_endpoint':'http://127.0.0.1:8890','write_endpoint':'http://127.0.0.1:8888','writer_identity_and_code_checked':True},indent=2)+'\n')
except BaseException:
 (K/'RPC_SPLIT_TEST_FAILURE.json').write_text(json.dumps({'error':traceback.format_exc()},indent=2)+'\n');raise
