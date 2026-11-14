#!/usr/bin/env python3
"""Test actual phone.py result/polling functions without CLI or device access.

The functions are loaded from their AST; argument parsing and all top-level
phone commands are excluded. fastboot, timing, logging and stdout are stubbed.
"""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock


SOURCE = Path(__file__).with_name('phone.py')
FUNCTIONS = {'loader_result', 'check_loader_result', 'wait_for_result', 'collect_run_diagnostics'}


class Clock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds

    def strftime(self, _format):
        return '20000101-000000'


class PhoneTests(unittest.TestCase):
    def setUp(self):
        tree = ast.parse(SOURCE.read_text(encoding='utf-8'), filename=str(SOURCE))
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in FUNCTIONS]
        self.assertEqual({node.name for node in functions}, FUNCTIONS)
        self.clock = Clock()
        self.results = []
        self.commands = []
        self.snapshot_error = None
        self.live_response = 'OKAY'
        self.namespace = dict(re=re, subprocess=subprocess, sys=sys, time=self.clock,
                              fb=self.fb, call=self.call, print=Mock())
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(SOURCE), 'exec'), self.namespace)
        self.temporary = tempfile.TemporaryDirectory(prefix='ace5-phone-tests-', dir=SOURCE.resolve().parents[3])
        self.directory = Path(self.temporary.name)
        self.log = self.directory / 'run.log'

    def tearDown(self):
        self.temporary.cleanup()

    def fb(self, *command):
        self.commands.append(command)
        if command == ('getvar', 'result'):
            result = self.results.pop(0) if self.results else 'result: running 0 0\n'
            if isinstance(result, Exception):
                raise result
            return result
        if command[0] == 'get_staged':
            if self.snapshot_error:
                raise self.snapshot_error
            Path(command[1]).write_text('diagnostic bytes\n', encoding='utf-8')
            return 'OKAY'
        self.fail('Unexpected fastboot command: ' + repr(command))

    def call(self, command, check):
        self.assertEqual(command, ['fastboot', '-s', 'xnuloader', 'oem', 'live-log'])
        self.assertFalse(check)
        self.commands.append(('live-log',))
        return self.live_response

    def collect(self, command='run', seconds=10, bootcpu=1, live_log=False, interval=1):
        return self.namespace['collect_run_diagnostics'](
            command, seconds, bootcpu, live_log, interval, self.directory, self.log)

    def test_result_parser_uses_actual_field(self):
        parse = self.namespace['loader_result']
        self.assertEqual(parse('result: returned 42 0\nFinished. Total time: 0.1s\n'), 'returned')
        self.assertEqual(parse('result: selftest-ok 0 0\r\n'), 'selftest-ok')
        self.assertEqual(parse('result: idle\r\n'), 'idle')
        for reply in ('', 'OKAY', 'result:\nreturned', 'result: ', 'result: returned\nresult: running\n'):
            with self.subTest(reply=reply), self.assertRaises(RuntimeError):
                parse(reply)

    def test_run_success_fetches_final_log(self):
        self.results = ['result: running 0 0\n', 'result: returned 100 0\n']
        self.assertIn('returned', self.collect())
        self.assertEqual(self.log.read_text(), 'diagnostic bytes\n')
        self.assertEqual(self.commands[-1], ('get_staged', self.log))

    def test_selftest_requires_its_own_success(self):
        self.results = ['result: selftest-ok 0 0\n']
        self.assertIn('selftest-ok', self.collect(command='selftest'))
        self.results = ['result: returned 0 0\n']
        with self.assertRaisesRegex(RuntimeError, 'selftest failed: returned'):
            self.collect(command='selftest')

    def test_failure_statuses_are_not_success_and_logs_survive(self):
        for status in ('not-started', 'early-exception', 'checkpoint', 'idle', 'other-core-hung', 'selftest-ok'):
            with self.subTest(status=status):
                self.results = [f'result: {status} 0 0\n']
                with self.assertRaisesRegex(RuntimeError, f'run failed: {status}'):
                    self.collect()
                self.assertEqual(self.log.read_text(), 'diagnostic bytes\n')
                self.assertEqual(self.commands[-1], ('get_staged', self.log))

    def test_missing_result_preserves_unconfirmed_staged_data(self):
        self.results = ['OKAY\n']
        with self.assertRaisesRegex(RuntimeError, 'Missing or ambiguous'):
            self.collect()
        staged = self.directory / 'run-poll-failure-staged.log'
        self.assertTrue(staged.is_file())
        self.assertFalse(self.log.exists())
        self.assertIn('final result unconfirmed', str(self.namespace['print'].call_args))

    def test_cpu_zero_wait_counts_towards_deadline(self):
        with self.assertRaisesRegex(RuntimeError, 'Kernel did not return'):
            self.collect(seconds=10, bootcpu=0, interval=2)
        self.assertGreaterEqual(self.clock.now, 70)
        self.assertLessEqual(self.clock.now, 72)
        self.assertTrue((self.directory / 'run-poll-failure-staged.log').is_file())

    def test_poll_timeout_preserves_original_error_and_staged_data(self):
        error = subprocess.TimeoutExpired('fake-fastboot', 20)
        self.results = [error]
        with self.assertRaises(subprocess.TimeoutExpired) as raised:
            self.collect()
        self.assertIs(raised.exception, error)
        self.assertTrue((self.directory / 'run-poll-failure-staged.log').is_file())

    def test_failed_diagnostic_fetch_does_not_mask_poll_failure(self):
        error = RuntimeError('original poll failure')
        self.results = [error]
        self.snapshot_error = RuntimeError('USB unavailable')
        with self.assertRaises(RuntimeError) as raised:
            self.collect()
        self.assertIs(raised.exception, error)
        self.assertIn('Could not fetch', str(self.namespace['print'].call_args))

    def test_live_snapshots_and_final_log_are_both_preserved(self):
        self.results = ['result: running 0 0\n', 'result: running 0 0\n', 'result: returned 10 0\n']
        self.collect(live_log=True)
        self.assertTrue((self.directory / '20000101-000000-live-001.log').is_file())
        self.assertTrue(self.log.is_file())
        self.assertEqual(self.commands[-1], ('get_staged', self.log))

    def test_live_log_race_preserves_final_log(self):
        self.results = ['result: running 0 0\n', 'result: running 0 0\n', 'result: returned 10 0\n']
        self.live_response = 'FAILED (remote: no live kernel; fetch the final staged log)'
        self.collect(live_log=True)
        self.assertTrue(self.log.is_file())
        self.assertFalse(list(self.directory.glob('*-live-*.log')))

    def test_live_log_failure_is_reported_after_best_effort_preservation(self):
        self.results = ['result: running 0 0\n', 'result: running 0 0\n']
        self.live_response = 'FAILED (remote: live-log unavailable)'
        with self.assertRaisesRegex(RuntimeError, 'Live-log request failed'):
            self.collect(live_log=True)
        self.assertTrue((self.directory / 'run-poll-failure-staged.log').is_file())


if __name__ == '__main__':
    unittest.main(verbosity=2)
