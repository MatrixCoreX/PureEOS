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
import time


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


def restore_source_metadata(root, cached_rows, expected_sha256, *, invalidate_after_ns=0):
    """Reuse timestamps for identical source and invalidate changed includes.

    Epoch-zero source overlays can otherwise leave Ninja using an old binary
    after a .inc/header changed. Validate the entire frozen source first.
    """
    root = Path(root).resolve(strict=True)
    if type(invalidate_after_ns) is not int or not 0 <= invalidate_after_ns < 2 ** 63 - 1:
        raise ValueError('Invalid source invalidation fence')
    for row in cached_rows.values():
        if (type(row.get('mtime_ns')) is not int or not 0 <= row['mtime_ns'] < 2 ** 63 - 1
                or type(row.get('bytes')) is not int or row['bytes'] < 0):
            raise ValueError('Invalid cached source metadata')
    fence = max(time.time_ns(), invalidate_after_ns,
                max((row['mtime_ns'] for row in cached_rows.values()), default=0)) + 1
    if fence >= 2 ** 63:
        raise ValueError('Source invalidation fence overflow')
    changes = []
    invalidated = []
    for name, expected in sorted(expected_sha256.items()):
        path = _path(root, name)
        actual = _sha(path)
        if actual != expected:
            raise ValueError('Source bytes do not match the frozen identity: ' + name)
        previous = cached_rows.get(name)
        if previous and previous.get('sha256') == actual and previous['bytes'] == path.stat().st_size:
            stamp = previous['mtime_ns']
        else:
            stamp = fence
            invalidated.append(name)
        changes.append((path, stamp))
    for path, stamp in changes:
        os.utime(path, ns=(stamp, stamp), follow_symlinks=False)
        if path.stat().st_mtime_ns != stamp:
            raise ValueError('Destination filesystem cannot preserve source timestamp')
    return dict(matched_files=len(changes) - len(invalidated), invalidated_files=invalidated,
                removed_cache_files=sorted(cached_rows.keys() - expected_sha256.keys()))
