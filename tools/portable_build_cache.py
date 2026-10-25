"""Preserve real build-cache timestamps without weakening Ninja validation.

Tar extraction through floating-point seconds can invalidate Ninja's dependency
log even when an object's bytes are unchanged. Restore the exact original
integer timestamps only after validating every selected file's path and bytes.
Compiler commands, source dependencies and actual test execution remain Ninja's
responsibility; this helper never marks a build or test as passed.
"""
import hashlib
from pathlib import Path, PurePosixPath
import os


def _path(root, name):
    if not isinstance(name, str) or '\\' in name:
        raise ValueError('Expected canonical relative cache path')
    relative = PurePosixPath(name)
    if relative.is_absolute() or not relative.parts or '..' in relative.parts or str(relative) != name:
        raise ValueError('Expected canonical relative cache path')
    path = root
    for part in relative.parts:
        path = path / part
        if path.is_symlink():
            raise ValueError('Cache file or ancestor is a symlink')
    if not path.is_file():
        raise ValueError('Cache input is not a regular file')
    return path


def _sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def snapshot_metadata(root, names):
    root = Path(root).resolve(strict=True)
    names = list(names)
    if len(names) != len(set(names)):
        raise ValueError('Duplicate cache path')
    rows = {}
    for name in sorted(names):
        path = _path(root, name)
        before = path.stat()
        digest = _sha(path)
        after = path.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns):
            raise ValueError('Cache input changed while recording metadata')
        rows[name] = dict(bytes=after.st_size, sha256=digest, mtime_ns=after.st_mtime_ns)
    return rows


def restore_metadata(root, rows):
    root = Path(root).resolve(strict=True)
    validated = []
    # Reject malformed or changed inputs before changing any file timestamp.
    for name, row in rows.items():
        path = _path(root, name)
        stamp = row.get('mtime_ns')
        size = row.get('bytes')
        if type(stamp) is not int or not 0 <= stamp <= 2 ** 63 - 1:
            raise ValueError('Invalid nanosecond timestamp')
        if type(size) is not int or size < 0 or path.stat().st_size != size or _sha(path) != row.get('sha256'):
            raise ValueError('Cache bytes do not match the recorded input')
        validated.append((path, stamp))
    for path, stamp in validated:
        os.utime(path, ns=(stamp, stamp), follow_symlinks=False)
        if path.stat().st_mtime_ns != stamp:
            raise ValueError('Destination filesystem cannot preserve the original timestamp')
    return len(validated)
