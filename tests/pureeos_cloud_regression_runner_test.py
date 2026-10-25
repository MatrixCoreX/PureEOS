from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
from pureeos_cloud_regression_runner import Runner, verified_root


class CloudRunnerControlTests(unittest.TestCase):
    def test_private_temp_suffix_underscore_is_closed_but_old_run_is_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);folder=root/'input';folder.mkdir()
            (folder/'old-peer-roots.json').write_text('["/opt/pureeos-peer/integer-v2-oldtest1"]')
            owner=Runner.__new__(Runner);owner.role='b';owner.input=folder
            units='pureeos-peer-integer-v2-xiwflpz_-nodea-30000.service loaded inactive dead\n'
            units+='pureeos-peer-integer-v2-oldtest1-nodeb-30001.service loaded inactive dead\n'
            # Stop only the new task unit, then deliberately refuse archival
            # because a process remains: no real process or archive is touched.
            with patch('pureeos_cloud_regression_runner.subprocess.check_output',return_value=units), \
                 patch('pureeos_cloud_regression_runner.subprocess.run',return_value=Mock(returncode=0)) as run:
                with self.assertRaisesRegex(RuntimeError,'process is still running'):owner.close()
            stopped=[call.args[0] for call in run.call_args_list if call.args[0][0]=='systemctl']
            self.assertEqual(stopped,[['systemctl','stop','pureeos-peer-integer-v2-xiwflpz_-nodea-30000.service']])

    def test_unexpected_service_name_is_not_stopped(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);folder=root/'input';folder.mkdir();(folder/'old-peer-roots.json').write_text('[]')
            owner=Runner.__new__(Runner);owner.role='b';owner.input=folder
            with patch('pureeos_cloud_regression_runner.subprocess.check_output',return_value='unrelated-user.service loaded running\n'), \
                 patch('pureeos_cloud_regression_runner.subprocess.run') as run:
                with self.assertRaisesRegex(RuntimeError,'Unexpected task peer unit'):owner.close()
                run.assert_not_called()

    def test_unrelated_or_broad_directories_are_refused(self):
        for path in ('/','/opt','/home/guagua','/opt/other','/opt/pureeos-history-/child'):
            with self.assertRaises(ValueError):verified_root(Path(path))
        self.assertEqual(verified_root(Path('/opt/pureeos-history-W6fSHS')),Path('/opt/pureeos-history-W6fSHS'))

    def owner(self, root):
        owner=Runner.__new__(Runner);owner.root=root;owner.input=root/'input';owner.input.mkdir()
        owner.runtime=root/'runtime';owner.role='a';owner.pair={'a':{'id':'test-instance'}}
        (owner.input/'limits.json').write_text(json.dumps({'deadline_utc':(datetime.now(timezone.utc)+timedelta(hours=12)).isoformat()}))
        owner.verify=Mock(return_value=26);owner.collect_all=Mock(return_value={'status':'passed'})
        return owner

    def test_child_failure_is_recorded_and_archived_not_promoted_to_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            owner=self.owner(Path(temp));child=Mock();child.wait.return_value=1
            with patch('pureeos_cloud_regression_runner.subprocess.Popen',return_value=child):
                self.assertEqual(owner.run('cohort'),1)
            self.assertEqual(json.loads((owner.root/'cohort-runner.json').read_text())['status'],'failed')
            owner.collect_all.assert_called_once()

    def test_missing_child_report_does_not_hide_a_launcher_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            owner=self.owner(Path(temp))
            with patch('pureeos_cloud_regression_runner.subprocess.Popen',side_effect=OSError('test launch failed')):
                self.assertEqual(owner.run('cohort'),1)
            report=json.loads((owner.root/'cohort-runner.json').read_text())
            self.assertEqual(report['status'],'failed');self.assertIn('test launch failed',report['error'])

    def test_existing_marker_prevents_restart_and_evidence_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            owner=self.owner(Path(temp));(owner.root/'cohort-launch.json').write_text('original')
            with patch('pureeos_cloud_regression_runner.subprocess.Popen') as launch:
                self.assertEqual(owner.run('cohort'),1)
            launch.assert_not_called();self.assertEqual((owner.root/'cohort-launch.json').read_text(),'original')
            self.assertEqual(json.loads((owner.root/'cohort-restart-refused.json').read_text())['status'],'interrupted-restart-refused')


if __name__=='__main__':unittest.main()
