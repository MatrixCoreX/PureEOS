#!/usr/bin/env python3
"""Real Git/CMake metadata checks; no node processes or transaction workloads."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class VersionMetadataTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='pureeos version metadata ');self.addCleanup(self.tmp.cleanup);self.base=Path(self.tmp.name);self.repo=self.base/'source tree';self.repo.mkdir();self.out=self.base/'build metadata';self.git=shutil.which('git');self.cmake=shutil.which('cmake');self.assertTrue(self.git and self.cmake)
  self.run_git('init','--initial-branch=main');(self.repo/'tracked.txt').write_text('original\n');self.run_git('add','tracked.txt');self.run_git('-c','user.name=Metadata Test','-c','user.email=metadata@example.invalid','commit','-m','fixture');self.commit=self.run_git('rev-parse','HEAD').stdout.strip()
 def run_git(self,*args):return subprocess.run([self.git,'-C',str(self.repo),*args],text=True,capture_output=True,check=True)
 def generate(self,git=None,repo=None,success=True):
  command=[self.cmake,'-DGIT_EXEC='+str(git or self.git),'-DSRC_DIR='+str(repo or self.repo),'-DCUR_SRC_DIR='+str(ROOT/'libraries/version'),'-DCUR_BIN_DIR='+str(self.out),'-DV_MAJOR=2','-DV_MINOR=0','-DV_PATCH=0','-DV_SUFFIX=','-P',str(ROOT/'CMakeModules/VersionUtils.cmake')];r=subprocess.run(command,text=True,capture_output=True)
  self.assertEqual(r.returncode==0,success,r.stdout+r.stderr)
  return (self.out/'src/version_impl.cpp').read_text()if success else r.stderr
 def check_clean(self,text):self.assertIn(self.commit,text);self.assertRegex(text,r'version_dirty\s*\{\s*false\s*\}')
 def check_dirty(self,text):self.assertIn(self.commit,text);self.assertRegex(text,r'version_dirty\s*\{\s*true\s*\}')
 def fake_git(self,body):
  p=self.base/'fake git';p.write_text('#!/usr/bin/env python3\nimport sys\n'+body);p.chmod(0o755);return p
 def test_clean_checkout_keeps_real_hash(self):self.check_clean(self.generate())
 def test_unstaged_tracked_change_is_dirty(self):
  (self.repo/'tracked.txt').write_text('changed\n');self.check_dirty(self.generate())
 def test_staged_tracked_change_is_dirty(self):
  (self.repo/'tracked.txt').write_text('changed\n');self.run_git('add','tracked.txt');self.check_dirty(self.generate())
 def test_staged_new_file_is_dirty(self):
  (self.repo/'new.txt').write_text('new\n');self.run_git('add','new.txt');self.check_dirty(self.generate())
 def test_untracked_output_is_not_dirty(self):
  (self.repo/'build-output.txt').write_text('generated\n');self.check_clean(self.generate())
 def test_worktree_git_file_is_supported(self):
  tree=self.base/'separate worktree';self.run_git('worktree','add','--detach',str(tree),'HEAD');self.check_clean(self.generate(repo=tree))
 def test_missing_git_executable_fails(self):self.assertIn('identity lookup failed',self.generate(git=self.base/'not-present',success=False))
 def test_git_repository_error_cannot_become_dirty(self):
  git=self.fake_git("sys.exit(128)\n");self.assertIn('identity lookup failed',self.generate(git=git,success=False))
 def test_diff_error_cannot_become_dirty(self):
  git=self.fake_git("if sys.argv[1]=='rev-parse': print('a'*40)\nelse: sys.exit(128)\n");self.assertIn('status lookup failed',self.generate(git=git,success=False))
 def test_success_without_hash_fails(self):
  git=self.fake_git("sys.exit(0)\n");self.assertIn('complete hexadecimal commit ID',self.generate(git=git,success=False))
 def test_malformed_hash_fails(self):
  git=self.fake_git("print('z'*40)\n");self.assertIn('complete hexadecimal commit ID',self.generate(git=git,success=False))
if __name__=='__main__':unittest.main()
