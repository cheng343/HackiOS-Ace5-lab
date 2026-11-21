# SPDX-License-Identifier: BSD-2-Clause
"""Offline transport/replay fixtures: these tests do not launch adb."""
import importlib.util
from pathlib import Path
import re
import unittest
from unittest.mock import patch

MODULE=Path(__file__).with_name('capture-rc0-training.py')
spec=importlib.util.spec_from_file_location('capture_rc0_training',MODULE)
capture=importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
BOOT='17db1000-0000-4000-8000-000000000101'

class Phone:
    # Only a model of readbacks. This is not a real board baseline.
    def __init__(self):
        self.reg={0:0x011317cb,4:1<<20,8:0x06040001,0xc:0x00010000,0x34:0x70,
                  0x70:0x00420010,0x7c:0x7b4c23,0x100:0x1c010001,
                  0x1c0:0x00010019,0x1cc:0x16032401}
        self.fail_at=None
        self.missing_post=False
        self.suspend=False
        self.boot=BOOT
        self.cfg_calls=[]
    def execute(self,cmd):
        assert '0000:01:00.0' not in cmd and 'reboot' not in cmd and ' of=' not in cmd and '/power/control' not in cmd
        prefix=capture.scope()
        if cmd==prefix+' && cat '+capture.BOOT:
            return '' if self.suspend else self.boot
        if cmd==capture.scope(BOOT)+' && printf stable':
            return '' if self.missing_post else 'stable'
        self.assert_scope(cmd)
        m=re.search(r'bs=4 skip=(\d+) count=1',cmd)
        assert m
        off=int(m[1])*4
        self.cfg_calls.append(off)
        if off==self.fail_at:
            return 'word: 01 02 03:end'
        return 'word: '+self.reg[off].to_bytes(4,'little').hex(' ')+':end'
    @staticmethod
    def assert_scope(cmd):
        checks=capture.scope(BOOT)
        assert cmd.startswith(checks+' || exit 1;') and cmd.count(checks)==2
        assert cmd.count(capture.ROOT+'/config')==1
        assert '/sys/bus/pci/devices/0000:00:00.0/power/runtime_status' in cmd

class TestTransport(unittest.TestCase):
    def test_noncontiguous_caps_no_endpoint_no_dump_replay_does_not_trust_claim(self):
        phone=Phone();samples=[]
        with patch('subprocess.run',side_effect=AssertionError('no adb in synthetic run')):
            result=capture.capture(phone.execute,samples)
            self.assertEqual((result['status'],result['offset'],result['lanes']),
                             ('present',0x1c0,[0x2401,0x1603]))
            doc={'schema':capture.SCHEMA,'capture_complete':True,'dwords':samples,'result':{'raw':0}}
            self.assertEqual(capture.replay(doc),result)
            self.assertNotIn(0x1c4, phone.cfg_calls); self.assertNotIn(0x1c8, phone.cfg_calls)
            for damage in [samples[:-1], samples+samples[:1], [samples[1],samples[0],*samples[2:]],
                           [[False,samples[0][1]],*samples[1:]],[[samples[0][0],-1],*samples[1:]]]:
                with self.assertRaises(ValueError):capture.replay(dict(doc,dwords=damage))
            with self.assertRaises(ValueError):capture.replay(dict(doc,capture_complete=False))
    def test_stop_on_bad_board_scope_or_short_reads(self):
        for what in ('suspend','wrong-boot','short','changed'):
            p=Phone();samples=[]
            if what=='suspend':p.suspend=True
            if what=='wrong-boot':p.boot=';echo bad'
            if what=='short':p.fail_at=0x100
            if what=='changed':p.missing_post=True
            with self.assertRaises(ValueError):capture.capture(p.execute,samples)
            if what in ('suspend','wrong-boot'):self.assertEqual((samples,p.cfg_calls),([],[]))
            if what=='short':self.assertEqual(p.cfg_calls[-1],0x100)
    def test_unknown_without_contact_for_legacy_records(self):
        for record in ({'model':'PKG110','registers':{}},{'samples':[{'0x80c':0x110190}]}):
            with self.assertRaises(ValueError):capture.replay(record)

if __name__=='__main__':
    unittest.main()
