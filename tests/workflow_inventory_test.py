"""Actual Git sparse/full checkout controls; never invoke GitHub Actions."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


MODULE=Path(__file__).resolve().parents[1]/'tools/workflow_inventory.py'
SPEC=importlib.util.spec_from_file_location('workflow_inventory_controls',MODULE)
WF=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(WF)


class WorkflowInventoryControls(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='aic-workflow-git-')
        self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name)/'repo';self.root.mkdir()
        self.git('init','-q','-b','main','--template=')
        folder=self.root/'.github/workflows';folder.mkdir(parents=True)
        (folder/'build.yaml').write_text('name: current\n')
        (folder/'old.yml').write_text('name: obsolete\n')
        (self.root/'src').mkdir();(self.root/'src/keep.txt').write_text('tracked source')
        self.git('add','.')
        self.git('-c','user.name=Workflow control','-c','user.email=workflow@example.invalid',
                 'commit','-q','-m','isolated inventory fixture')

    def git(self,*args):
        return subprocess.check_output(['git','-c','core.hooksPath=/dev/null','-C',str(self.root),*args],
                                       stderr=subprocess.PIPE,timeout=20)

    def sparse(self):
        self.git('sparse-checkout','init','--cone')
        self.git('sparse-checkout','set','src')
        folder=self.root/'.github/workflows';folder.mkdir(parents=True)
        (folder/'build.yaml').write_text('name: current\n')

    def test_sparse_omission_is_not_a_repository_deletion(self):
        self.sparse()
        self.assertEqual({p.name for p in (self.root/'.github/workflows').iterdir()},{'build.yaml'})
        rows=WF.effective_workflows(self.root)
        self.assertEqual(set(rows),{'build.yaml','old.yml'})
        self.assertEqual(rows['old.yml']['origin'],'git-index')
        with self.assertRaisesRegex(ValueError,'old.yml'):
            WF.require_workflows(self.root,{'build.yaml'})

    def test_unstaged_and_staged_deletions_both_remove_a_workflow(self):
        (self.root/'.github/workflows/old.yml').unlink()
        WF.require_workflows(self.root,{'build.yaml'})
        self.git('add','-u','--','.github/workflows/old.yml')
        WF.require_workflows(self.root,{'build.yaml'})

    def test_untracked_workflow_is_also_checked(self):
        (self.root/'.github/workflows/untracked.yaml').write_text('name: untracked\n')
        with self.assertRaisesRegex(ValueError,'untracked.yaml'):
            WF.require_workflows(self.root,{'build.yaml','old.yml'})

    def test_workflow_symlink_cannot_read_an_external_target(self):
        external=self.root.parent/'outside';external.write_text('must not be consumed')
        (self.root/'.github/workflows/linked.yml').symlink_to(external)
        with self.assertRaisesRegex(ValueError,'regular file'):
            WF.effective_workflows(self.root)

    def test_source_export_does_not_borrow_a_parent_repository_index(self):
        export=self.root/'src/export'
        folder=export/'.github/workflows';folder.mkdir(parents=True)
        (folder/'build.yaml').write_text('name: exported\n')
        rows=WF.require_workflows(export,{'build.yaml'})
        self.assertEqual(rows['build.yaml']['origin'],'working-tree')

    def test_full_clone_and_sparse_checkout_have_the_same_effective_bytes(self):
        full=self.root.parent/'full'
        self.git('clone','-q','--no-hardlinks',str(self.root),str(full))
        self.sparse()
        def content(root):return {n:r['content'] for n,r in WF.effective_workflows(root).items()}
        self.assertEqual(content(full),content(self.root))
        self.assertEqual(set(content(full)),{'build.yaml','old.yml'})


if __name__=='__main__':unittest.main()
