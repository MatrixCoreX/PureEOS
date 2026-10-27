"""Real Ninja dependency-cache regression and rejected transport metadata."""
import copy
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('portable_cache', Path(__file__).resolve().parents[1] / 'tools/portable_build_cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class PortableCacheControls(unittest.TestCase):
    def test_changed_epoch_zero_include_rebuilds_the_actual_test_program(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'main.cpp').write_text('#include "fixture.inc"\n#include <cstdio>\nint main() { puts(test_name()); }\n')
            fixture = root / 'fixture.inc'
            fixture.write_text('const char* test_name() { return "legacy_case"; }\n')
            (root / 'build.ninja').write_text('rule compile\n  command = c++ -MMD -MF $out.d $in -o $out\n  depfile = $out.d\n  deps = gcc\nbuild test: compile main.cpp\n')
            subprocess.run(['ninja'], cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            cached = cache.snapshot_metadata(root, ['main.cpp', 'fixture.inc'])
            fence = (root / 'test').stat().st_mtime_ns
            fixture.write_text('const char* test_name() { return "parameter_mirror_case"; }\n')
            os.utime(fixture, ns=(0, 0))
            stale = subprocess.check_output(['ninja'], cwd=root, text=True)
            self.assertIn('no work to do', stale)
            self.assertEqual(subprocess.check_output([root / 'test'], text=True).strip(), 'legacy_case')
            expected = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                        for name in ('main.cpp', 'fixture.inc')}
            restored = cache.restore_source_metadata(root, cached, expected, invalidate_after_ns=fence)
            self.assertEqual(restored['invalidated_files'], ['fixture.inc'])
            self.assertEqual((root / 'main.cpp').stat().st_mtime_ns, cached['main.cpp']['mtime_ns'])
            rebuilt = subprocess.check_output(['ninja'], cwd=root, text=True)
            self.assertIn('c++ -MMD', rebuilt)
            self.assertEqual(subprocess.check_output([root / 'test'], text=True).strip(), 'parameter_mirror_case')

    def test_real_ninja_reuses_only_after_exact_timestamp_restore(self):
        self.assertIsNotNone(shutil.which('ninja'))
        self.assertIsNotNone(shutil.which('c++'))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'case.cpp'
            source.write_text('int cache_case() { return 7; }\n')
            # Force a timestamp that floating-point seconds cannot represent.
            # This real cache timestamp rounds FORWARD by 101 ns through the
            # legacy transport. Ninja can tolerate a backward rounding, so a
            # mere unequal timestamp would not reproduce this failure.
            stamp = 1792718900510152000
            os.utime(source, ns=(stamp - 1000000000, stamp - 1000000000))
            (root / 'build.ninja').write_text('rule compile\n  command = c++ -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\nbuild case.o: compile case.cpp\n')
            subprocess.run(['ninja'], cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            obj = root / 'case.o'
            # Teach Ninja's real deps log the exact output timestamp through
            # a second actual compilation, rather than editing its binary log.
            (root / 'build.ninja').write_text('rule compile\n  command = c++ -MMD -MF $out.d -c $in -o $out && python3 -c "import os; os.utime(\'$out\', ns=(' + str(stamp) + ', ' + str(stamp) + '))"\n  depfile = $out.d\n  deps = gcc\nbuild case.o: compile case.cpp\n')
            subprocess.run(['ninja'], cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            metadata = cache.snapshot_metadata(root, ['case.o'])
            self.assertEqual(obj.stat().st_mtime_ns, stamp)
            original = obj.read_bytes()
            archive = io.BytesIO()
            with tarfile.open(fileobj=archive, mode='w') as stream:
                stream.add(obj, arcname='case.o', recursive=False)
            obj.unlink()
            archive.seek(0)
            with tarfile.open(fileobj=archive) as stream:
                stream.extractall(root, filter='data')
            self.assertEqual(obj.read_bytes(), original)
            self.assertNotEqual(obj.stat().st_mtime_ns, stamp)
            self.assertGreater(obj.stat().st_mtime_ns, stamp)
            broken = subprocess.check_output(['ninja', '-n', '-d', 'explain'], cwd=root, text=True, stderr=subprocess.STDOUT)
            self.assertIn('c++ -MMD', broken)
            self.assertEqual(cache.restore_metadata(root, metadata), 1)
            restored = subprocess.check_output(['ninja', '-n', '-d', 'explain'], cwd=root, text=True, stderr=subprocess.STDOUT)
            self.assertIn('no work to do', restored)

    def test_rejects_changed_bytes_before_any_timestamp_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('a.o', 'b.o'):
                (root / name).write_bytes(name.encode())
            metadata = cache.snapshot_metadata(root, ['a.o', 'b.o'])
            (root / 'b.o').write_bytes(b'corrupted')
            before = (root / 'a.o').stat().st_mtime_ns
            metadata['a.o']['mtime_ns'] = 1700000000000000000
            with self.assertRaises(ValueError):
                cache.restore_metadata(root, metadata)
            self.assertEqual((root / 'a.o').stat().st_mtime_ns, before)

    def test_rejects_symlink_ancestors_and_invalid_integer_timestamps(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inside = root / 'inside'; inside.mkdir()
            (inside / 'case.o').write_bytes(b'object')
            metadata = cache.snapshot_metadata(root, ['inside/case.o'])
            for stamp in (True, -1, 2 ** 63, 1.25):
                altered = copy.deepcopy(metadata); altered['inside/case.o']['mtime_ns'] = stamp
                with self.assertRaises(ValueError):
                    cache.restore_metadata(root, altered)
            (root / 'alias').symlink_to(inside, target_is_directory=True)
            with self.assertRaises(ValueError):
                cache.snapshot_metadata(root, ['alias/case.o'])
            for name in ('../case.o', '/case.o', './inside/case.o', 'inside/../inside/case.o'):
                with self.assertRaises(ValueError):
                    cache.snapshot_metadata(root, [name])


if __name__ == '__main__':
    unittest.main()
