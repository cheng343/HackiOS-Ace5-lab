#!/usr/bin/env python3
"""Test actual phone.py result/polling functions without CLI or device access.

The functions are loaded from their AST; argument parsing and all top-level
phone commands are excluded. fastboot, timing, logging and stdout are stubbed.
"""
import ast
import hashlib
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
             'ace5_driver_arguments', 'ace5_wifi_probe_arguments', 'check_wifi_probe_log', 'ace5_aop_test_arguments',
             'ace5_crm_test_arguments', 'ace5_pdc_test_arguments', 'ace5_cx_vote_arguments',
             'ace5_rpmh_read_arguments', 'check_rpmh_read_log', 'check_rpmh_read_profile',
             'ace5_wifi_bringup_arguments', 'check_wifi_bringup_log', 'check_stock_startup_perf', 'check_stock_train_cmdline', 'finish_log_reboot_launch'}


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
    def test_wifi_bringup_result_requires_stage_and_actual_cleanup(self):
        check = self.namespace['check_wifi_bringup_log']
        def report(stage=4, ceiling=4, rc=0, stop=1, end=14):
            return (f'ace5-wifi-bringup: start rc={rc} stage={stage} max={ceiling} failed=0 err=0 sub=0 cleanup=0\n'
                    f'ace5-wifi-bringup: requested stage complete; stage={stage} max={ceiling}\n'
                    f'ace5-wifi-bringup: stop rc={stop} stage={end}\n')
        held = 'ace5-wifi-bringup: resources retained; firmware quiet and IRQ37 isolation retained until cold reboot'
        clean = 'ace5-wifi-bringup: cleanup complete; IRQ37 restored and firmware resumed (cold reboot required)'
        good = report() + held
        check(True, 4, good, 'yes', self.log)
        status = good + '\nace5-wifi-bringup: quiet watchdog status ret=0 ctl=0 since=0x11\n'
        check(True, 4, status, 'yes', self.log, watchdog=1)
        with self.assertRaisesRegex(RuntimeError, 'Idle Gunyah watchdog status'):
            check(True, 4, status + 'ace5-wifi-bringup: quiet watchdog status ret=0 ctl=0x1',
                  'yes', self.log, watchdog=1)
        with self.assertRaisesRegex(RuntimeError, 'Watchdog expiry did not reset'):
            check(True, 4, status, 'yes', self.log, watchdog=2)
        with self.assertRaisesRegex(RuntimeError, 'CESTA snapshot missing'):
            check(True, 4, good, 'yes', self.log, True)
        check(True, 4, good + '\nace5-wifi-bringup: post-phy cesta start=0 branch0=0 override=0 clkreq=0',
              'yes', self.log, True)
        snapshot = good + '\nace5-wifi-bringup: post-phy cesta start=0 branch0=0 override=0 clkreq=0'
        with self.assertRaisesRegex(RuntimeError, 'Empty CESTA tables unconfirmed'):
            check(True, 4, snapshot, 'yes', self.log, 2)
        cold = snapshot + '\nace5-wifi-bringup: cesta cold-check rc=0\n'
        check(True, 4, cold, 'yes', self.log, 2)
        with self.assertRaisesRegex(RuntimeError, 'Empty CESTA tables unconfirmed'):
            check(True, 4, cold + '[   22.243792]: ace5-wifi-bringup: cesta cold-check rc=-5\n', 'yes', self.log, 2)
        with self.assertRaisesRegex(RuntimeError, 'CESTA staging and rollback unconfirmed'):
            check(True, 4, cold, 'yes', self.log, 3)
        staged = cold + 'ace5-wifi-bringup: cesta stage rc=0 discard=0 cold=0\n'
        check(True, 4, staged, 'yes', self.log, 3)
        start = (snapshot + '\nace5-wifi-bringup: loader cold reset armed\n'
                 'ace5-wifi-bringup: cesta START write returned\n'
                 'ace5-wifi-bringup: cesta execution sample=0 start=0x1\n'
                 'ace5-wifi-bringup: cesta execution sample=1 start=0x1\n'
                 'ace5-wifi-bringup: cesta START result=1 observe=0; retained until reset\n')
        check(True, 4, start, 'yes', self.log, sm_diag=4, cold_reset=True)
        perf = (start + 'ace5-wifi-bringup: CESTA perf current=0x2 aggregate=0x2\n'
                'ace5-wifi-bringup: CESTA follow-up rc=0\n')
        check(True, 4, perf, 'yes', self.log, sm_diag=5, cold_reset=True)
        dbi = (perf + 'ace5-wifi-bringup: DBI+0 access returned 0x11317cb\n'
               'ace5-wifi-bringup: DBI root id rc=0 id=0x11317cb\n')
        check(True, 4, dbi, 'yes', self.log, sm_diag=6, cold_reset=True)
        endpoint = dbi + ('PCIe endpoint identify rc=0 id=0x110717cb lnk=0x2011\n'
                          'PCIe endpoint reset reassert rc=0\n')
        check(True, 4, endpoint, 'yes', self.log, sm_diag=7, cold_reset=True)
        stock = endpoint.replace('current=0x2 aggregate=0x2', 'current=0x4 aggregate=0x4')
        with self.assertRaisesRegex(RuntimeError, 'startup PERF 4 unconfirmed'):
            check(True, 4, stock, 'yes', self.log, sm_diag=7, cold_reset=True, stock_perf=True)
        stock = 'ace5-wifi-bringup: stock D0 reset armed before PERF vote\n' + stock
        check(True, 4, stock, 'yes', self.log, sm_diag=7, cold_reset=True, stock_perf=True)
        for field in ['current', 'aggregate']:
            with self.assertRaisesRegex(RuntimeError, 'startup PERF 4 unconfirmed'):
                check(True, 4, stock.replace(field + '=0x4', field + '=0x2'), 'yes', self.log,
                      sm_diag=7, cold_reset=True, stock_perf=True)
        with self.assertRaisesRegex(RuntimeError, 'PCS-before-PIPE trial unconfirmed'):
            check(True, 4, endpoint, 'yes', self.log, sm_diag=7, cold_reset=True, pcs_first=True)
        check(True, 4, endpoint + 'ace5-wifi-bringup: PCS-before-PIPE reset armed before PHY access\n',
              'yes', self.log, sm_diag=7, cold_reset=True, pcs_first=True)
        with self.assertRaisesRegex(RuntimeError, 'WCN7850 identity or reset'):
            check(True, 4, endpoint, 'yes', self.log, sm_diag=7, cold_reset=True, native_width=True)
        check(True, 4, endpoint.replace('lnk=0x2011', 'lnk=0x2021'), 'yes', self.log,
              sm_diag=7, cold_reset=True, native_width=True)
        hold = 'ace5-wifi-bringup: pre-PHY clock hold before=0x361c after=0x365e rc=0\n'
        armed = endpoint + 'ace5-wifi-bringup: PCS-before-PIPE reset armed before PHY access\n'
        def verify_hold(data, **override):
            kwargs = dict(sm_diag=7, cold_reset=True, pcs_first=True, pre_phy_clocks=True) | override
            check(True, 4, data, 'yes', self.log, **kwargs)
        verify_hold(armed + hold + hold)
        for extra in ('', hold.replace('365e', '361e'), hold.replace('365e', '365f'),
                      hold.replace('rc=0', 'rc=-1'), hold.replace('rc=0', 'rc=00'),
                      hold.replace('361c', 'ffffffbd').replace('365e', 'ffffffff'),
                      hold + 'ace5-wifi-bringup: pre-PHY clock hold refused\n',
                      hold + hold.replace('361c','365e')):
            with self.subTest(hold=extra), self.assertRaisesRegex(RuntimeError, 'clock hold readback'):
                verify_hold(armed + extra)
        with self.assertRaisesRegex(RuntimeError, 'clock hold readback'):
            verify_hold(armed + hold, pcs_first=False)
        with self.assertRaisesRegex(RuntimeError, 'PCS-before-PIPE'):
            verify_hold(endpoint + hold)
        with self.assertRaisesRegex(RuntimeError, 'WCN7850 identity or reset'):
            verify_hold(armed.replace('0x110717cb', '0xffffffff') + hold)
        ep2 = endpoint.replace('lnk=0x2011', 'lnk=0x2021')
        marker = 'ace5-wifi-bringup: stock train lanes before=2 after=1\n'
        def verify_ep(data):
            check(True, 4, data, 'yes', self.log, sm_diag=7, cold_reset=True, native_width=True, stock_train_lanes=True)
        verify_ep(ep2 + '\n' + marker + marker)
        for suffix in ('', marker + marker.replace('after=1', 'after=2'),
                       marker + marker.replace('before=2', 'before=32'), marker + marker.replace('after=1', 'after=1bad')):
            with self.assertRaisesRegex(RuntimeError, 'train-lanes readback'):
                verify_ep(ep2 + '\n' + suffix)
        with self.assertRaisesRegex(RuntimeError, 'WCN7850 identity or reset'):
            verify_ep(endpoint + '\n' + marker)
        for native, stable, training in ((False, '0x3011', '0x3811'), (True, '0x3021', '0x3821')):
            check(True, 4, endpoint.replace('0x2011', stable), 'yes', self.log,
                  sm_diag=7, cold_reset=True, native_width=native)
            with self.subTest(native=native), self.assertRaisesRegex(RuntimeError, 'WCN7850 identity or reset'):
                check(True, 4, endpoint.replace('0x2011', training), 'yes', self.log,
                      sm_diag=7, cold_reset=True, native_width=native)
        with self.assertRaisesRegex(RuntimeError, 'WCN7850 identity or reset'):
            check(True, 4, endpoint.replace('lnk=0x2011', 'lnk=0x2022'),
                  'yes', self.log, sm_diag=7, cold_reset=True)
        with self.assertRaisesRegex(RuntimeError, 'PCIe0 root identity'):
            check(True, 4, perf, 'yes', self.log, sm_diag=6, cold_reset=True)
        with self.assertRaisesRegex(RuntimeError, 'Guarded CESTA START'):
            check(True, 4, start + 'DBI+0 access begin', 'yes', self.log, sm_diag=4, cold_reset=True)
        with self.assertRaisesRegex(RuntimeError, 'Independent cold reset did not occur'):
            check(True, 4, good, 'yes', self.log, cold_reset=True)
        with self.assertRaisesRegex(RuntimeError, 'CESTA staging and rollback unconfirmed'):
            check(True, 4, staged + 'ace5-wifi-bringup: cesta stage rc=0 discard=-3 cold=-2\n',
                  'yes', self.log, 3)
        check(True, 4, good + '\n' + good, 'yes', self.log)
        check(True, 0, report(11, 0, stop=0, end=13) + clean, 'yes', self.log)
        check(False, 4, '', None, self.log)
        for text, reset in ((good, 'no'), ('', 'yes'), (held, 'yes'),
                (report(), 'yes'), (report(stop=-4, end=12) + clean, 'yes'),
                (report(rc=-6) + held, 'yes'), (report(3) + held, 'yes'),
                (report(4, 0) + held, 'yes'), (report() + clean, 'yes'),
                (report(stop=0, end=13) + held, 'yes'), (good + clean, 'yes'),
                (good + '\nace5-wifi-bringup: cleanup FAILED; retained', 'yes'),
                (good + '\n' + report(rc=-6), 'yes'),
                (report() + 'ace5-wifi-bringup: cleanup complete; firmware resumed (cold reboot required)', 'yes')):
            with self.subTest(text=text, reset=reset), self.assertRaises(RuntimeError):
                check(True, 4, text, reset, self.log)

    def test_mode7_timeout_explains_only_consistent_sampled_evidence(self):
        check = self.namespace['check_wifi_bringup_log']
        prefix = 'ace5-wifi-bringup: '
        head = '\n'.join(prefix + s for s in (
            'start rc=0 stage=4 max=4 failed=0 err=0 sub=0 cleanup=0',
            'post-phy cesta start=0 branch0=0 override=0 clkreq=0', 'loader cold reset armed',
            'cesta START write returned', 'cesta execution sample=0 start=0x1',
            'cesta execution sample=1 start=0x1', 'cesta START result=1 observe=0',
            'CESTA perf current=0x2 aggregate=0x2', 'DBI+0 access returned 0x11317cb',
            'DBI root id rc=0 id=0x11317cb')) + '\n'
        endpoint = prefix + 'PCIe endpoint identify rc=-7 id=0 lnk=0x1011 stage=4 fail=4 polls=1001 debug=0x8ac400/0x8200000 elbi=0x1 ltssm=0\n'
        history = prefix + 'PCIe training samples=1001 first=0 last=0 seen_lo=0x1 seen_hi=0\n'
        reset = prefix + 'PCIe endpoint reset reassert rc=0\n'
        followup = prefix + 'CESTA follow-up rc=-7\n'
        extra = prefix + 'train evidence parf_valid=0x3 parf_enable=0x100 parf_timeout=0x100 dllla=0 lt=0 xmlh=0 ready=0\n'
        def failure(report):
            with self.assertRaises(RuntimeError) as caught:
                check(True, 4, head + report, 'yes', self.log, sm_diag=7, cold_reset=True)
            return str(caught.exception)
        legacy = endpoint + reset + history + followup
        self.assertIn('Detect.Quiet at all sampled polls; PERST# reassert readback confirmed', failure(legacy + legacy))
        self.assertNotIn('PARF LTSSM_EN', failure(legacy))
        self.assertIn('sampled states=0x3', failure(legacy.replace('seen_lo=0x1', 'seen_lo=0x3')))
        self.assertIn('PERST# reassert unconfirmed', failure(legacy.replace(reset, '')))
        self.assertIn('PARF LTSSM_EN at enable/timeout=1/1', failure(legacy + extra))
        self.assertIn('1/0; DLLLA/LT/XMLH/ready counts=1/2/3/0',
                      failure(legacy + extra.replace('parf_timeout=0x100 dllla=0 lt=0 xmlh=0', 'parf_timeout=0 dllla=1 lt=2 xmlh=3')))
        self.assertIn('1/unknown', failure(legacy + extra.replace('parf_valid=0x3', 'parf_valid=0x1')))
        for invalid in (legacy.replace(history, ''), legacy + history.replace('first=0', 'first=1'),
                        legacy.replace('polls=1001', 'polls=1000'), legacy.replace('last=0', 'last=1'),
                        legacy.replace('lnk=0x1011', 'lnk=0xffff'), legacy.replace('stage=4', 'stage=3'),
                        legacy.replace('id=0 ', 'id=0x110717cb '), legacy.replace('seen_lo=0x1', 'seen_lo=0'),
                        legacy + reset.replace('rc=0', 'rc=-1'), legacy + extra.replace('ready=0', 'ready=1'),
                        legacy + extra.replace('parf_enable=0x100', 'parf_enable=0xffffffff'),
                        legacy + extra.replace('lt=0', 'lt=1002'), legacy + extra + extra.replace('lt=0', 'lt=1'),
                        legacy + extra.replace('xmlh=0', 'xmlh=oops'), legacy.replace('rc=-7\n', 'rc=-7junk\n')):
            with self.subTest(invalid=invalid):
                message = failure(invalid)
                self.assertIn('missing or conflicting', message)
                self.assertNotIn('all sampled polls', message)

    def test_log_reboot_requires_a_supported_profile_and_fresh_hex_identifier(self):
        fn = self.namespace['ace5_wifi_bringup_arguments']
        table = self.directory / 'pdc.bin'
        table.write_bytes(bytes(508))
        profile = self.directory / 'sm.bin'
        profile.write_bytes(bytes(80))
        # The digest of the private DT profile is checked by a separate test.
        digest = Mock()
        digest.sha256.return_value.hexdigest.return_value = 'b3ed154a6cabba921d99cf703d70255e2239977683153992c4a8f1cc5211d90e'
        self.namespace['hashlib'] = digest
        nonce = '0a1b2c3d4e5f6789'
        for mode in (5, 7):
            args = fn(True, table, 1, 'kernel', 60, False, False, 4, sm_diag=mode,
                      sm_profile=profile, cold_reset=True, log_reboot_nonce=nonce)
            self.assertIn(' ace5_wifi_log_reboot=1 ace5_run_nonce=' + nonce, args)
        for mode, run_id in ((6, nonce), (5, ''), (7, 'A' * 16), (7, nonce + '\n'), (7, nonce[:-1])):
            with self.subTest(mode=mode, run_id=run_id), self.assertRaises(RuntimeError):
                fn(True, table, 1, 'kernel', 60, False, False, 4, sm_diag=mode,
                   sm_profile=profile, cold_reset=True, log_reboot_nonce=run_id)
        with self.assertRaises(RuntimeError):
            fn(False, table, 1, 'kernel', 60, False, False, log_reboot_nonce=nonce)

    def test_log_reboot_ack_exits_with_observation_pending(self):
        finish = self.namespace['finish_log_reboot_launch']
        with self.assertRaises(SystemExit) as pending:
            finish('OKAY [  0.000s]\nFinished. Total time: 0.001s\n', '0a1b2c3d4e5f6789')
        self.assertEqual(pending.exception.code, 75)
        for reply in ('', 'INFO: MAYBE OKAY', 'OKAYISH'):
            with self.assertRaisesRegex(RuntimeError, 'acknowledgement missing'):
                finish(reply, '0a1b2c3d4e5f6789')

    def test_wifi_bringup_profile_is_explicit_and_isolated(self):
        fn = self.namespace['ace5_wifi_bringup_arguments']
        table = self.directory / 'pdc.bin'
        table.write_bytes(bytes(508))
        with self.assertRaisesRegex(RuntimeError, 'require --wifi-bringup-test'):
            fn(False, None, 1, 'kernel', 60, False, False, stock_perf_dtb=table)
        with self.assertRaisesRegex(RuntimeError, 'requires --wifi-cesta-endpoint'):
            fn(True, table, 1, 'kernel', 60, False, False, 4, cold_reset=True, stock_perf_dtb=table)
        self.assertEqual(fn(True, table, 1, 'kernel', 30, False, False, 4),
                         ' ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4')
        self.assertEqual(fn(True, table, 1, 'kernel', 30, False, False, 4, watchdog=1),
                         ' ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_watchdog=1')
        with self.assertRaisesRegex(RuntimeError, 'Watchdog diagnostic requires'):
            fn(True, table, 1, 'kernel', 30, False, False, 4, watchdog=2)
        self.assertEqual(fn(True, table, 1, 'kernel', 60, False, False, 4, cold_reset=True),
                         ' ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_cold_reset=1')
        self.assertEqual(fn(True, table, 1, 'kernel', 30, False, False, 4, False, True),
                         ' ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_sm_diag=1')
        self.assertEqual(fn(True, table, 1, 'kernel', 30, False, False, 4, False, 2),
                         ' ace5_wifi_bringup=1 ace5_wifi_bringup_stage=4 ace5_wifi_sm_diag=2')
        profile = self.directory / 'sm.bin'
        profile.write_bytes(bytes(80))
        with self.assertRaisesRegex(RuntimeError, 'CESTA profile does not match'):
            fn(True, table, 1, 'kernel', 30, False, False, 4, sm_profile=profile)
        with self.assertRaises(RuntimeError): fn(True, table, 1, 'kernel', 30, False, False, 5, False, True)
        with self.assertRaises(RuntimeError): fn(True, table, 1, 'kernel', 30, False, False, 4, True, True)
        self.assertEqual(fn(False, None, 0, None, 0, True, True), '')
        with self.assertRaises(RuntimeError): fn(False, None, 1, 'kernel', 30, False, False, 0, True, False)
        with self.assertRaises(RuntimeError): fn(False, None, 1, 'kernel', 30, False, False, 0, False, True)
        for cpu, kernel, seconds, other, live in ((0, 'kernel', 30, False, False),
                (1, None, 30, False, False), (1, 'kernel', 0, False, False),
                (1, 'kernel', 121, False, False), (1, 'kernel', 30, True, False),
                (1, 'kernel', 30, False, True)):
            with self.assertRaises(RuntimeError): fn(True, table, cpu, kernel, seconds, other, live)
        with self.assertRaises(RuntimeError): fn(False, None, 1, 'kernel', 30, False, False, 4)
        table.write_bytes(bytes(507))
        with self.assertRaises(RuntimeError): fn(True, table, 1, 'kernel', 30, False, False)

    def test_stock_train_lanes_isolated_and_bounded(self):
        fn = self.namespace['ace5_wifi_bringup_arguments']
        limit = self.namespace['check_stock_train_cmdline']
        table, profile = self.directory / 'pdc', self.directory / 'sm'
        table.write_bytes(bytes(508))
        profile.write_bytes(bytes(80))
        args = dict(stage=4, sm_diag=7, sm_profile=profile, cold_reset=True, native_width=True)
        with self.assertRaisesRegex(RuntimeError, 'CESTA profile'):
            fn(True, table, 1, 'kernel', 60, False, False, **args, stock_train_lanes=True)
        digest = Mock()
        digest.sha256.return_value.hexdigest.return_value = 'b3ed154a6cabba921d99cf703d70255e2239977683153992c4a8f1cc5211d90e'
        self.namespace['hashlib'] = digest
        legacy = fn(True, table, 1, 'kernel', 60, False, False, **args)
        self.assertEqual(fn(True, table, 1, 'kernel', 60, False, False, **args, stock_train_lanes=True),
                         legacy + ' ace5_wifi_stock_train=1')
        for override in (dict(native_width=False), dict(sm_diag=5), dict(cold_reset=False),
                         dict(sm_profile=None), dict(stage=3)):
            with self.assertRaises(RuntimeError):
                fn(True, table, 1, 'kernel', 60, False, False, **(args | override), stock_train_lanes=True)
        n = 607 - len('debug=0x8 rd=md0 ')
        limit('x' * n)
        with self.assertRaisesRegex(RuntimeError, '608-byte'): limit('x' * (n+1))

    def test_pre_phy_clocks_isolated_and_optional(self):
        fn = self.namespace['ace5_wifi_bringup_arguments']
        table, profile = self.directory / 'pdc', self.directory / 'sm'
        table.write_bytes(bytes(508))
        profile.write_bytes(bytes(80))
        args = dict(enabled=True, table=table, bootcpu=1, kernel='kernel', seconds=60,
                    other_hardware=False, live_log=False, stage=4, sm_diag=7,
                    sm_profile=profile, cold_reset=True, pcs_first=True)
        with self.assertRaisesRegex(RuntimeError, 'CESTA profile'):
            fn(**args, pre_phy_clocks=True)
        digest = Mock()
        digest.sha256.return_value.hexdigest.return_value = 'b3ed154a6cabba921d99cf703d70255e2239977683153992c4a8f1cc5211d90e'
        self.namespace['hashlib'] = digest
        base = fn(**args)
        self.assertNotIn('pre_phy', base)
        self.assertEqual(fn(**args, pre_phy_clocks=True), base + ' ace5_wifi_pre_phy_clks=1')
        for override in (dict(enabled=False), dict(pcs_first=False), dict(sm_diag=5),
                         dict(stage=3), dict(sm_profile=None), dict(cold_reset=False), dict(bootcpu=0),
                         dict(seconds=59), dict(seconds=121), dict(other_hardware=True),
                         dict(live_log=True), dict(watchdog=1), dict(crm_all_states=True)):
            with self.subTest(override=override), self.assertRaises(RuntimeError):
                fn(**(args | override), pre_phy_clocks=True)

    def test_stock_perf_requires_rc0_three_row_profile(self):
        import struct
        check = self.namespace['check_stock_startup_perf']
        path = '/soc/qcom,pcie@1c00000'
        profile = struct.pack('>9I', 64,64,19200000, 64,64,19200000, 256,256,100000000)
        rc = {'compatible': b'qcom,pci-msm\0', 'reg-names': b'dm_core\0pcie_sm\0', 'qcom,bw-scale': profile}
        check({path: rc})
        for bad in ({'/soc/qcom,pcie@1c08000': rc}, {path: dict(rc, compatible=b'qcom,pcie\0')},
                    {path: {**rc, 'reg-names': b'dm_core\0'}}, {path: {**rc, 'qcom,bw-scale': profile[:-12]}}):
            with self.assertRaisesRegex(RuntimeError, 'three-row'):
                check(bad)

    def test_rpmh_read_profile_uses_current_dt(self):
        fn=self.namespace['check_rpmh_read_profile']
        nodes={'/rsc':{'compatible':b'qcom,rpmh-rsc\0','reg':bytes.fromhex('17a000000001000017a100000001000017a2000000010000'),
                'interrupts':bytes.fromhex('000000000000000300000004000000000000000400000004000000000000000500000004')},
            '/rsc/drv@2':{'qcom,drv-id':bytes.fromhex('00000002'),'qcom,tcs-offset':bytes.fromhex('00000d00')},
            '/rsc/drv@2/channel@0':{'qcom,tcs-config':bytes.fromhex('00000002000000030000000000000002000000010000000200000003000000000000000400000001')},
            '/gic':{'compatible':b'arm,gic-v3\0','reg':bytes.fromhex('17100000000100001718000000200000')},
            '/db':{'compatible':b'qcom,cmd-db\0','reg':bytes.fromhex('0000000081c600000000000000020000')}}
        fn(nodes)
        for path,properties in nodes.items():
            for key in properties:
                bad={p:dict(v) for p,v in nodes.items()};bad[path][key]=b''
                with self.assertRaises(RuntimeError): fn(bad)
        nodes['/rsc']['qcom,hw-channel']=b''
        with self.assertRaises(RuntimeError): fn(nodes)
    def test_rpmh_read_trial_and_result_are_explicit(self):
        fn=self.namespace['ace5_rpmh_read_arguments']
        self.assertEqual(fn(False,0,None,0,True,True),'')
        self.assertEqual(fn(True,1,'kernel',90,False,False),' ace5_aop_channel_test=4')
        for cpu,kernel,seconds,other,live in ((0,'kernel',90,False,False),
            (1,None,90,False,False),(1,'kernel',0,False,False),(1,'kernel',121,False,False),
            (1,'kernel',90,True,False),(1,'kernel',90,False,True)):
            with self.assertRaises(RuntimeError): fn(True,cpu,kernel,seconds,other,live)
        check=self.namespace['check_rpmh_read_log']
        capture='ace5-rpmh-read: capture complete; 46 READ replies stable; resource ownership NOT assumed'
        cleanup='ace5-rpmh-read: cleanup complete; IRQ restored and firmware resumed'
        check(True,capture+'\n'+cleanup,'yes',self.log)
        for text,reset in ((capture,'yes'),(cleanup,'yes'),(capture+'\n'+cleanup,'no'),('','yes')):
            with self.assertRaises(RuntimeError): check(True,text,reset,self.log)
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
        self.namespace = dict(re=re, hashlib=hashlib, subprocess=subprocess, sys=sys, time=self.clock,
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

    def test_pdc_requires_table_quiet_run_and_excludes_other_clients(self):
        fn = self.namespace['ace5_pdc_test_arguments']
        table = self.directory/'pdc-table.bin'
        self.assertEqual(fn(False, None, 0, None, 0, False, False), '')
        with self.assertRaises(RuntimeError):
            fn(False, table, 1, Path('kernel'), 60, False, False)
        for size in (0, 507, 509):
            table.write_bytes(b'x'*size)
            with self.assertRaises(RuntimeError):
                fn(True, table, 1, Path('kernel'), 60, False, False)
        table.write_bytes(b'x'*508)
        self.assertEqual(fn(True, table, 1, Path('kernel'), 60, False, False), ' ace5_aop_channel_test=2')
        for args in ((None, 1, Path('kernel'), 60, False, False),
                     (table, 0, Path('kernel'), 60, False, False),
                     (table, 1, None, 60, False, False),
                     (table, 1, Path('kernel'), 9, False, False),
                     (table, 1, Path('kernel'), 121, False, False),
                     (table, 1, Path('kernel'), 60, True, False),
                     (table, 1, Path('kernel'), 60, False, True)):
            with self.assertRaises(RuntimeError): fn(True, *args)
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
        for mode, cpu, kernel, seconds in ((6, 1, 'kernel', 30), (1, 0, 'kernel', 30),
                                           (2, 1, None, 30), (1, 1, 'kernel', 0),
                                           (2, 1, 'kernel', -1)):
            with self.subTest(mode=mode, cpu=cpu, seconds=seconds), self.assertRaises(RuntimeError):
                arguments(mode, cpu, kernel, seconds)
        for mode in (1, 2, 3, 4, 5):
            self.assertEqual(arguments(mode, 1, 'kernel', 30), f' ace5_wifi_probe={mode}')
        self.assertEqual(self.commands, [])

    def test_wifi_probe_requires_its_actual_completion(self):
        check = self.namespace['check_wifi_probe_log']
        check(None, '', self.log)
        for mode in (1, 2, 3, 4, 5):
            start = f'ace5-wifi: probe={mode} read-only begin'
            end = 'ace5-wifi: read-only probe complete'
            header = 'ace5-wifi: early CESTA/CRM headers captured'
            aop = 'ace5-wifi: AOP descriptor captured'
            full = 'ace5-wifi: full SM/CRM snapshot captured; no table/channel writes'
            arc = 'ace5-wifi: HLOS ARC snapshot captured; resource ownership NOT assumed'
            markers = [start, end] + ([header] if 2 <= mode <= 4 else []) + ([aop] if 3 <= mode <= 4 else []) + ([full] if mode == 4 else []) + ([arc] if mode == 5 else [])
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

    def test_cx_vote_trial_is_finite_isolated_and_has_no_live_polling(self):
        args = self.namespace['ace5_cx_vote_arguments']
        self.assertEqual(args(False, 0, None, 0, True, True), '')
        self.assertEqual(args(True, 1, 'kernel', 60, False, False), ' ace5_aop_channel_test=3')
        for cpu, kernel, seconds, other, live in ((0, 'kernel', 60, False, False),
                (1, None, 60, False, False), (1, 'kernel', 9, False, False),
                (1, 'kernel', 121, False, False), (1, 'kernel', 60, True, False),
                (1, 'kernel', 60, False, True)):
            with self.assertRaises(RuntimeError):
                args(True, cpu, kernel, seconds, other, live)
        self.assertEqual(self.commands, [])

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

    def test_crm_trial_is_explicit_isolated_and_has_no_live_usb(self):
        args = self.namespace['ace5_crm_test_arguments']
        self.assertEqual(args(False, 0, None, 0, True, True), '')
        self.assertEqual(args(True, 1, 'kernel', 60, False, False), ' ace5_crm_channel_test=1')
        self.assertEqual(args(True, 1, 'kernel', 60, False, False, True),
                         ' ace5_crm_channel_test=2 ace5_crm_fixed_bw=1')
        for cpu, kernel, seconds, other, live in ((0, 'kernel', 60, False, False),
                (1, None, 60, False, False), (1, 'kernel', 0, False, False),
                (1, 'kernel', 121, False, False), (1, 'kernel', 60, True, False),
                (1, 'kernel', 60, False, True)):
            with self.assertRaises(RuntimeError):
                args(True, cpu, kernel, seconds, other, live)
        self.assertEqual(self.commands, [])

    def test_crm_trial_waits_for_quiet_return_before_usb_poll(self):
        polled = []
        def fb(*command):
            polled.append((self.clock.now, command))
            return 'result: returned 0 0\n'
        self.namespace['fb'] = fb
        result = self.namespace['wait_for_result']('run', 60, 1, False, 2, self.directory, True)
        self.assertEqual(result, 'result: returned 0 0\n')
        self.assertEqual(polled, [(65, ('getvar', 'result'))])

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
