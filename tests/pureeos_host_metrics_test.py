"""Probe interpretation regressions; no real host processes or journal queried."""
import subprocess
import unittest

from pureeos_host_metrics import collect, interpret

MEMORY = '               total used free shared buff/cache available\nMem: 1000 100 700 0 200 850\nSwap: 0 0 0\n'
HEADER = '    PID %CPU   RSS NLWP ELAPSED COMMAND\n'


class HostMetricsTests(unittest.TestCase):
    def test_no_oom_journal_entries_exit_one_is_valid_empty_evidence(self):
        result=interpret('oom',1,'-- No entries --\n','')
        self.assertEqual(result['status'],'empty')
        self.assertFalse(result['oom_detected'])

    def test_no_oom_journal_entries_exit_zero_is_also_valid(self):
        self.assertEqual(interpret('oom',0,'-- No entries --\n','')['status'],'empty')

    def test_journal_permissions_are_not_mistaken_for_no_oom(self):
        result=interpret('oom',1,'-- No entries --\n','Permission denied\n')
        self.assertEqual(result['status'],'error')
        self.assertNotIn('oom_detected',result)

    def test_unknown_empty_journal_failure_is_not_silenced(self):
        self.assertEqual(interpret('oom',1,'','')['status'],'error')
        self.assertEqual(interpret('oom',2,'-- No entries --\n','')['status'],'error')

    def test_real_oom_is_preserved_not_reported_as_no_entries(self):
        result=interpret('oom',0,'kernel: Out of memory: Killed process 1234 (nodeos)\n','')
        self.assertEqual(result['status'],'ok')
        self.assertTrue(result['oom_detected'])

    def test_before_peers_start_ps_header_exit_one_is_valid(self):
        result=interpret('processes',1,HEADER,'')
        self.assertEqual(result['status'],'empty')
        self.assertEqual(result['nodes'],[])

    def test_real_ps_error_is_preserved(self):
        for code,text,error in ((1,HEADER,'ps: error'),(1,'',''),(2,HEADER,'')):
            self.assertEqual(interpret('processes',code,text,error)['status'],'error')

    def test_actual_node_pid_and_memory_are_parsed(self):
        result=interpret('processes',0,HEADER+' 1234 2.1 60000 8 31 nodeos.elf\n','')
        self.assertEqual(result['status'],'ok')
        self.assertEqual(result['nodes'][0]['rss_kib'],60000)

    def test_malformed_process_row_is_an_error(self):
        self.assertEqual(interpret('processes',0,HEADER+'1234 broken row\n','')['status'],'error')

    def test_memory_is_in_bytes(self):
        result=interpret('memory',0,MEMORY,'')
        self.assertEqual(result['status'],'ok')
        self.assertEqual(result['memory_bytes']['available'],850)

    def test_missing_or_failed_memory_probe_is_not_fabricated(self):
        for code,text,error in ((1,MEMORY,''),(0,'',''),(0,MEMORY,'warning')):
            self.assertEqual(interpret('memory',code,text,error)['status'],'error')

    def test_collection_records_permission_timeout_and_tool_absence(self):
        def fail(command,**_):
            if command[0]=='free':raise FileNotFoundError('free')
            if command[0]=='ps':raise subprocess.TimeoutExpired(command,10)
            return subprocess.CompletedProcess(command,1,'-- No entries --\n','Permission denied')
        result=collect(fail)
        self.assertEqual(result['status'],'incomplete')
        self.assertTrue(all(p['status']=='error' for p in result['probes'].values()))

    def test_empty_idle_host_is_a_complete_sample(self):
        def idle(command,**_):
            code,text={'free':(0,MEMORY),'ps':(1,HEADER),'journalctl':(1,'-- No entries --\n')}[command[0]]
            return subprocess.CompletedProcess(command,code,text,'')
        self.assertEqual(collect(idle)['status'],'complete')

    def test_new_run_oom_window_excludes_old_failures_without_erasing_them(self):
        seen=[]
        def idle(command,**_):
            seen.append(command)
            code,text={'free':(0,MEMORY),'ps':(1,HEADER),'journalctl':(1,'-- No entries --\n')}[command[0]]
            return subprocess.CompletedProcess(command,code,text,'')
        result=collect(idle,since='2026-10-21T10:30:00+08:00')
        self.assertEqual(result['since_utc'],'2026-10-21T02:30:00+00:00')
        self.assertEqual(seen[-1][-2:],['--since',result['since_utc']])
        self.assertEqual(result['status'],'complete')

    def test_ambiguous_metric_window_is_rejected(self):
        with self.assertRaises(ValueError):collect(since='2026-10-21T02:30:00')


if __name__=='__main__':unittest.main()
