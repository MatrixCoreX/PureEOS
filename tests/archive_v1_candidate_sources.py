"""Preserve the actual private build source, not just an overlay; exact C only."""
import argparse
import hashlib
from datetime import datetime
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import time

SOURCE = Path(__file__).resolve().parents[1]

TOP = ('.cicd', '.github', '.gitmodules', 'CMakeLists.txt', 'CMakeModules', 'LICENSE',
       'README.md', 'benchmark', 'eos.doxygen.in', 'eosio.version.in', 'genesis',
       'libraries', 'package.cmake', 'plugins', 'programs', 'scripts', 'tests',
       'tools', 'unittests')


def verify_source_archive(path, core_digest):
    # Stream once. Random extractfile(name) over a gzip tar would repeatedly
    # decompress the entire vendor baseline when manifest order differs.
    files, links, seen, identity = {}, {}, set(), None
    with tarfile.open(path, 'r|gz') as archive:
        for member in archive:
            name=member.name
            assert name not in seen and not Path(name).is_absolute() and '..' not in Path(name).parts
            seen.add(name)
            if name=='source-identity.json':
                assert member.isfile();identity=json.load(archive.extractfile(member));continue
            assert name.startswith('source/'), 'Unexpected source archive member'
            relative=name.removeprefix('source/')
            if member.isfile():
                files[relative]=hashlib.file_digest(archive.extractfile(member),'sha256').hexdigest()
            elif member.issym():
                assert not Path(member.linkname).is_absolute()
                depth=len(Path(relative).parent.parts)
                for part in Path(member.linkname).parts:
                    if part=='..':depth-=1;assert depth>=0,'Source link escapes root'
                    elif part!='.':depth+=1
                links[relative]=member.linkname
            else:assert member.isdir(), 'No devices or hardlinks in source package'
    assert identity is not None and identity['core_archive_sha256']==core_digest
    assert files==identity['files_sha256'] and links==identity['relative_symlinks']
    return identity


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume',action='store_true',help='Resume only this task archive/download, not tests')
    args = parser.parse_args()
    sys.path.insert(0, str(SOURCE.parent / 'PureEOS/build/local-tests/unified-cdt5-20261021-TBMyRV'))
    import cloud_round as c
    args.output.mkdir(exist_ok=args.resume)
    report = dict(status='waiting-for-stopped-archived-candidate', release_ready=False,
                  deadline_unchanged=c.DEADLINE, scope=__doc__)
    def save():
        report['updated_utc'] = c.now().isoformat(); c.atomic(args.output/'report.json', report)
    try:
        save()
        while not (args.candidate/'download-verified.json').exists():
            assert (datetime.fromisoformat(c.DEADLINE)-c.now()).total_seconds() > 1200
            time.sleep(45)
        proof = c.read(args.candidate/'download-verified.json')
        assert proof['status']=='passed' and c.sha(proof['local'])==proof['sha256']
        launch = c.read(args.candidate/'launch.json'); plan = c.read(args.candidate/'input.json')
        assert launch['root'] in tuple('/opt/pureeos-v1-integrated-20261022'+s for s in ('-v6','-v7','-v8'))
        code = '''import hashlib,json,os,pathlib,tarfile,subprocess
os.umask(0o077);os.nice(19)
root=pathlib.Path(ROOT);source=pathlib.Path('/opt/pureeos-v1-integrated-20261022/source')
assert subprocess.check_output(['systemctl','show',UNIT,'-p','ActiveState','--value'],text=True).strip() in ('inactive','failed')
archive=root/'actual-build-source.tar.gz';manifest=root/'actual-build-source.json'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
if manifest.exists():
 v=json.loads(manifest.read_text());assert sha(archive)==v['archive_sha256'];print(json.dumps(v));raise SystemExit(0)
assert not archive.exists(),'Never overwrite an incomplete source archive'
assert all(sha(source/n)==d for n,d in OVERLAY.items())
assert {p.name for p in source.iterdir()}==set(TOP),'Unexpected source root entries require inspection'
items=[];hashes={};links={};excluded=[]
for top in TOP:
 p=source/top
 candidates=[p]+(list(p.rglob('*')) if p.is_dir() else [])
 for f in candidates:
  parts=f.relative_to(source).parts
  if any(n in ('.git','__pycache__') for n in parts):excluded.append(str(f.relative_to(source)));continue
  assert not any(n in ('config-dir','.ssh','wallet','wallets') for n in parts),'Unexpected private directory'
  name=str(f.relative_to(source))
  if f.is_symlink():
   target=os.readlink(f);assert not pathlib.Path(target).is_absolute() and f.resolve().is_relative_to(source)
   links[name]=target
  elif f.is_file():hashes[name]=sha(f)
  elif not f.is_dir():raise RuntimeError('Non-source special file: '+name)
  items.append(f)
identity=dict(scope='Actual frozen source used by candidate; vendor source included; not a release pass',
 core_archive_sha256=CORE,files_sha256=hashes,relative_symlinks=links,excluded_git_and_pycache=excluded,
 toolchain={name:subprocess.check_output(command,text=True,stderr=subprocess.STDOUT).strip() for name,command in
 [('compiler',['c++','--version']),('cmake',['cmake','--version']),('ninja',['ninja','--version'])]})
metadata=json.dumps(identity,indent=2).encode()
import io
with tarfile.open(archive,'x:gz',compresslevel=1) as t:
 for f in sorted(items):t.add(f,arcname='source/'+str(f.relative_to(source)),recursive=False)
 info=tarfile.TarInfo('source-identity.json');info.size=len(metadata);info.mode=0o600;t.addfile(info,io.BytesIO(metadata))
assert all(sha(source/n)==d for n,d in hashes.items())
assert all(os.readlink(source/n)==target for n,target in links.items())
v=dict(status='archived-source-not-release',path=str(archive),archive_sha256=sha(archive),bytes=archive.stat().st_size,
 files=len(hashes),relative_symlinks=len(links),core_archive_sha256=CORE)
manifest.write_text(json.dumps(v,indent=2));print(json.dumps(v))
'''.replace('ROOT',repr(launch['root'])).replace('UNIT',repr(launch['unit'])).replace('OVERLAY',repr(plan['sha256'])).replace('TOP',repr(TOP)).replace('CORE',repr(proof['sha256']))
        # A read-only source archive after all timing-sensitive core probes.
        result=json.loads(c.remote('c',['python3','-c',code],600))
        ssh=c.old.ssh_args('c'); local=args.output/'actual-build-source.tar.gz'
        subprocess.run(['rsync','-a','--partial','--chmod=F600','-e',shlex.join(ssh[:-1]),
                        ssh[-1]+':'+result['path'],str(local)],check=True,timeout=600)
        assert local.stat().st_size==result['bytes'] and c.sha(local)==result['archive_sha256']
        identity=verify_source_archive(local,proof['sha256'])
        c.save(args.output/'source-identity.json',identity)
        report.update(status='verified-source-not-release',archive={**result,'local':str(local)})
    except Exception as error:
        report.update(status='failed-or-not-started',error=str(error))
    save(); print(json.dumps({k:v for k,v in report.items() if k in ('status','error','archive')}))
    return 0 if report['status']=='verified-source-not-release' else 1


if __name__=='__main__':raise SystemExit(main())
