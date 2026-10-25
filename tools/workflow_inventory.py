"""Inspect effective workflow sources, including files hidden by sparse checkout."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


def _git(root, *args):
    return subprocess.run(['git', '-C', str(root), *args], check=True,
                          capture_output=True, timeout=20).stdout


def _workflow(name):
    path=PurePosixPath(name)
    return (len(path.parts)==3 and path.parts[:2]==('.github','workflows')
            and path.suffix in ('.yaml','.yml'))


def effective_workflows(root):
    root=Path(root).resolve(strict=True)
    folder=root/'.github/workflows'
    if folder.is_symlink() or folder.parent.is_symlink():
        raise ValueError('Workflow directory must not be a symlink')
    records={}
    if folder.exists():
        for path in sorted(folder.iterdir()):
            if path.suffix not in ('.yaml','.yml'):continue
            if path.is_symlink() or not path.is_file():
                raise ValueError('Workflow must be a regular file: '+path.name)
            records[path.name]=dict(content=path.read_bytes(),origin='working-tree')
    try:
        repository=Path(_git(root,'rev-parse','--show-toplevel').decode().strip()).resolve()
    except (subprocess.CalledProcessError,FileNotFoundError):
        repository=None
    # An exported source may live underneath an unrelated repository. Never
    # substitute that parent's index for this export's own materialized files.
    if repository!=root:return records
    deleted={p.decode() for p in _git(root,'diff','--no-ext-diff','--no-textconv',
        '--name-only','--diff-filter=D','-z','--','.github/workflows').split(b'\0') if p}
    for entry in _git(root,'ls-files','--stage','-z','--','.github/workflows').split(b'\0'):
        if not entry:continue
        header,relative=entry.split(b'\t',1)
        name=relative.decode()
        if not _workflow(name):continue
        mode,object_id,stage=header.decode().split()
        if stage!='0' or mode not in ('100644','100755'):
            raise ValueError('Unmerged or nonregular tracked workflow: '+name)
        if name in deleted:continue
        basename=PurePosixPath(name).name
        if basename not in records:
            # A sparse/assume-unchanged omission is not a repository deletion.
            records[basename]=dict(content=_git(root,'cat-file','blob',object_id),origin='git-index')
    return records


def require_workflows(root, expected):
    records=effective_workflows(root)
    expected=set(expected)
    if set(records)!=expected:
        raise ValueError('Workflow inventory differs: unexpected='+str(sorted(set(records)-expected))
                         +', missing='+str(sorted(expected-set(records))))
    return records


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--expected',nargs='+',required=True)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    report=dict(status='running',release_ready=False,scope=__doc__)
    try:
        records=effective_workflows(args.root)
        report['workflows']={name:dict(origin=row['origin'],bytes=len(row['content']),
            sha256=hashlib.sha256(row['content']).hexdigest()) for name,row in sorted(records.items())}
        require_workflows(args.root,args.expected)
        report['status']='passed-inventory-only'
    except Exception as error:
        report.update(status='failed',error=str(error))
    if args.output:
        with args.output.open('x') as stream:stream.write(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
    return 0 if report['status']=='passed-inventory-only' else 1


if __name__=='__main__':raise SystemExit(main())
