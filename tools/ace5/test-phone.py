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
FUNCTIONS = {'loader_result', 'check_loader_result', 'wait_for_result', 'collect_run_diagnostics',
             'validate_run_timing', 'check_kernel_log', 'observe_persistent_startup', 'stop_running_kernel',
             'ace5_driver_arguments', 'ace5_wifi_probe_arguments', 'check_wifi_probe_log', 'ace5_aop_test_arguments'}


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
        self.staged_logs = []
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
            text = self.staged_logs.pop(0) if self.staged_logs else 'diagnostic bytes\n'
            Path(command[1]).write_text(text, encoding='utf-8')
            return 'OKAY'
        if command == ('oem', 'stop'):
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

    def test_experimental_drivers_are_off_by_default(self):
        self.assertEqual(self.namespace['ace5_driver_arguments'](0, None, False, 0), '')
        self.assertEqual(self.commands, [])

    def test_experimental_drivers_require_supported_run_conditions(self):
        arguments = self.namespace['ace5_driver_arguments']
        for battery, khz in ((True, 0), (False, 1200000)):
            with self.assertRaisesRegex(RuntimeError, 'explicit --kernel'):
                arguments(1, None, battery, khz)
        with self.assertRaisesRegex(RuntimeError, '--bootcpu 1'):
            arguments(0, Path('kernel'), True, 0)
        for khz in (-1, 599999, 1804801, 4000000):
            with self.assertRaisesRegex(RuntimeError, '600000..1804800'):
                arguments(1, Path('kernel'), False, khz)
        self.assertEqual(self.commands, [])

    def test_experimental_drivers_emit_only_requested_boot_arguments(self):
        arguments = self.namespace['ace5_driver_arguments']
        self.assertEqual(arguments(1, Path('kernel'), True, 0), ' ace5_battery=1')
        self.assertEqual(arguments(0, Path('kernel'), False, 600000), ' ace5_cpu_khz=600000')
        self.assertEqual(arguments(1, Path('kernel'), True, 1804800),
                         ' ace5_battery=1 ace5_cpu_khz=1804800')

    def test_run_success_fetches_final_log(self):
        self.results = ['result: running 0 0\n', 'result: returned 100 0\n']
        self.assertIn('returned', self.collect())
        self.assertEqual(self.log.read_text(), 'diagnostic bytes\n')
        self.assertEqual(self.commands[-1], ('get_staged', self.log))

    def test_wifi_probe_rejects_unobserved_or_persistent_runs(self):
        arguments = self.namespace['ace5_wifi_probe_arguments']
        self.assertEqual(arguments(None, 0, None, 0), '')
        for mode, cpu, kernel, seconds in ((5, 1, 'kernel', 30), (1, 0, 'kernel', 30),
                                           (2, 1, None, 30), (1, 1, 'kernel', 0),
                                           (2, 1, 'kernel', -1)):
            with self.subTest(mode=mode, cpu=cpu, seconds=seconds), self.assertRaises(RuntimeError):
                arguments(mode, cpu, kernel, seconds)
        for mode in (1, 2, 3, 4):
            self.assertEqual(arguments(mode, 1, 'kernel', 30), f' ace5_wifi_probe={mode}')
        self.assertEqual(self.commands, [])

    def test_wifi_probe_requires_its_actual_completion(self):
        check = self.namespace['check_wifi_probe_log']
        check(None, '', self.log)
        for mode in (1, 2, 3, 4):
            start = f'ace5-wifi: probe={mode} read-only begin'
            end = 'ace5-wifi: read-only probe complete'
            header = 'ace5-wifi: early CESTA/CRM headers captured'
            aop = 'ace5-wifi: AOP descriptor captured'
            full = 'ace5-wifi: full SM/CRM snapshot captured; no table/channel writes'
            markers = [start, end] + ([header] if mode >= 2 else []) + ([aop] if mode >= 3 else []) + ([full] if mode == 4 else [])
            text = '\n'.join(markers)
            check(mode, text, self.log)
            for marker in markers:
                with self.subTest(mode=mode, missing=marker), self.assertRaises(RuntimeError):
                    check(mode, text.replace(marker, ''), self.log)

    def test_selftest_requires_its_own_success(self):
        self.results = ['result: selftest-ok 0 0\n']
        self.assertIn('selftest-ok', self.collect(command='selftest'))
        self.results = ['result: returned 0 0\n']
        with self.assertRaisesRegex(RuntimeError, 'selftest failed: returned'):
            self.collect(command='selftest')

    def test_aop_channel_trial_is_explicit_and_isolated(self):
        args = self.namespace['ace5_aop_test_arguments']
        self.assertEqual(args(False, 0, None, 0, True), '')
        self.assertEqual(args(True, 1, 'kernel', 90, False), ' ace5_aop_channel_test=1')
        for cpu, kernel, seconds, other in ((0, 'kernel', 90, False), (1, None, 90, False),
                (1, 'kernel', 0, False), (1, 'kernel', 9, False), (1, 'kernel', 301, False),
                (1, 'kernel', 90, True)):
            with self.assertRaises(RuntimeError):
                args(True, cpu, kernel, seconds, other)
        self.assertEqual(self.commands, [])

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

    def timing(self, **changes):
        values = dict(seconds=0, bootcpu=1, gpu_probe=20, gpu_direct=True,
                      gpu_keep=True, gpu_step_delay=0, live_log=False)
        values.update(changes)
        return self.namespace['validate_run_timing'](**values)

    def observe(self, timeout=120):
        return self.namespace['observe_persistent_startup'](self.directory, self.log, timeout)

    def test_no_timer_requires_complete_persistent_profile(self):
        self.assertTrue(self.timing())
        for changed in ({'bootcpu': 0}, {'gpu_probe': None}, {'gpu_probe': 1}, {'gpu_probe': 19},
                        {'gpu_direct': False}, {'gpu_keep': False}, {'gpu_step_delay': 1},
                        {'live_log': True}):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                self.timing(**changed)

    def test_finite_timing_keeps_probe_guards(self):
        self.assertFalse(self.timing(seconds=120))
        self.assertFalse(self.timing(seconds=120, gpu_probe=None, bootcpu=0, gpu_direct=False, gpu_keep=False))
        self.assertFalse(self.timing(seconds=1, gpu_probe=2, bootcpu=0, gpu_direct=False, gpu_keep=False))
        for changed in ({'seconds': -1}, {'seconds': 3601}, {'seconds': 120, 'bootcpu': 0},
                        {'seconds': 9}, {'seconds': 10, 'gpu_step_delay': 1},
                        {'seconds': 120, 'gpu_step_delay': 3001},
                        {'seconds': 120, 'gpu_step_delay': -1},
                        {'seconds': 120, 'gpu_probe': None, 'bootcpu': 0, 'live_log': True}):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                self.timing(**changed)

    def test_persistent_requires_both_ready_markers_and_final_running_result(self):
        self.staged_logs = ['ace5-gpu: stage 20 complete\n',
                            'ace5-gpu: stage 20 complete\nace5-gpu: persistent startup ready=1\n']
        self.assertIn('running', self.observe())
        self.assertEqual(self.commands[-1], ('getvar', 'result'))
        self.assertEqual(sum(c == ('live-log',) for c in self.commands), 2)
        self.assertEqual(self.clock.now, 2)
        self.assertNotIn(('oem', 'stop'), self.commands)
        self.assertIn('not a stability test', str(self.namespace['print'].call_args))

    def test_persistent_rejects_premature_return_even_after_ready_log(self):
        self.staged_logs = ['ace5-gpu: stage 20 complete\nace5-gpu: persistent startup ready=1\n', 'final log\n']
        self.results = ['result: running\n', 'result: returned\n']
        with self.assertRaisesRegex(RuntimeError, 'ended before confirmation: returned'):
            self.observe()
        self.assertEqual(self.log.read_text(), 'final log\n')
        self.assertNotIn(('oem', 'stop'), self.commands)

    def test_persistent_nonrunning_status_saves_final_diagnostics(self):
        for state in ('returned', 'not-started', 'other-core-hung', 'idle'):
            with self.subTest(state=state):
                self.results = [f'result: {state}\n']
                with self.assertRaisesRegex(RuntimeError, 'ended before confirmation: ' + state):
                    self.observe()
                self.assertTrue(self.log.is_file())
        self.assertNotIn(('live-log',), self.commands)

    def test_persistent_live_log_race_saves_final_and_rejects_success(self):
        self.results = ['result: running\n', 'result: returned\n']
        self.live_response = 'FAILED (remote: no live kernel; fetch the final staged log)'
        with self.assertRaisesRegex(RuntimeError, 'ended before confirmation: returned'):
            self.observe()
        self.assertTrue(self.log.is_file())
        self.assertFalse(list(self.directory.glob('*-startup-0*.log')))

    def test_persistent_timeout_only_stops_host_observation(self):
        with self.assertRaisesRegex(RuntimeError, 'within 120 seconds.*no automatic stop or reboot'):
            self.observe()
        self.assertEqual(self.clock.now, 120)
        self.assertTrue((self.directory / 'run-startup-unconfirmed-staged.log').is_file())
        self.assertNotIn(('oem', 'stop'), self.commands)
        self.assertNotIn(('reboot',), self.commands)

    def test_persistent_failures_are_not_ready(self):
        for failure in ('panic(cpu 1 caller 0x00)', 'ace5-gpu: stage 20 FAILED',
                        'ace5-gpu: backend cleanup FAILED', 'ace5-gpu: live failure'):
            with self.subTest(failure=failure):
                self.staged_logs = [failure + '\nace5-gpu: stage 20 complete\nace5-gpu: persistent startup ready=1\n']
                with self.assertRaisesRegex(RuntimeError, 'Kernel/probe failure'):
                    self.observe()
        self.assertNotIn(('oem', 'stop'), self.commands)

    def test_persistent_usb_failure_preserves_diagnostics_without_stop(self):
        self.live_response = 'FAILED (remote: cannot read live log)'
        with self.assertRaisesRegex(RuntimeError, 'Live-log request failed.*Kernel may still be running'):
            self.observe()
        self.assertTrue((self.directory / 'run-startup-unconfirmed-staged.log').is_file())
        self.assertNotIn(('oem', 'stop'), self.commands)

    def test_manual_stop_waits_for_return_and_saves_log(self):
        self.results = ['result: running\n', 'result: running\n', 'result: returned\n']
        self.namespace['stop_running_kernel'](self.directory, self.log)
        self.assertEqual(self.commands[:2], [('getvar', 'result'), ('oem', 'stop')])
        self.assertEqual(self.commands[-1], ('get_staged', self.log))
        self.assertTrue(self.log.is_file())

    def test_manual_stop_skips_inactive_kernel(self):
        self.results = ['result: returned\n']
        self.namespace['stop_running_kernel'](self.directory, self.log)
        self.assertEqual(self.commands, [('getvar', 'result')])

    def test_manual_stop_timeout_never_reboots(self):
        with self.assertRaisesRegex(RuntimeError, 'No reboot was sent.*physical power'):
            self.namespace['stop_running_kernel'](self.directory, self.log)
        self.assertEqual(self.clock.now, 60)
        self.assertNotIn(('reboot',), self.commands)
        self.assertTrue((self.directory / 'run-poll-failure-staged.log').is_file())

    def test_manual_stop_rejects_cleanup_failure(self):
        self.results = ['result: running\n', 'result: returned\n']
        self.staged_logs = ['ace5-gpu: backend cleanup FAILED\n']
        with self.assertRaisesRegex(RuntimeError, 'No reboot was sent'):
            self.namespace['stop_running_kernel'](self.directory, self.log)


if __name__ == '__main__':
    unittest.main(verbosity=2)
