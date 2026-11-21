#!/usr/bin/env python3
"""PKG110 RC0 training baseline. Explicit Android read or exact offline replay only.
No automatic ADB discovery, no restart/root-daemon/shell installation, no writes
to PCI/sysfs/PM. A stock Linux PCI config read can itself take a runtime-PM
reference. Checking active before and after cannot exclude a suspend/wake race.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

from rc0_training_config import inspect_root

ROOT = '/sys/bus/pci/devices/0000:00:00.0'
BOOT = '/proc/sys/kernel/random/boot_id'
UUID = r'[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}'
SCHEMA = 'giulia-rc0-selective-dwords-v1'
MAX_READS = 500  # global bound incl. both passes
TIME = 90

def scope(boot=None):
    # Only hardcoded paths/addresses or a strictly checked UUID enter shell.
    if boot is not None and not re.fullmatch(UUID, boot):
        raise ValueError('invalid boot identity')
    required = [('getprop ro.product.model','PKG110'),('getprop sys.boot_completed','1'),
                ('cat '+ROOT+'/vendor','0x17cb'),('cat '+ROOT+'/device','0x0113'),
                ('cat '+ROOT+'/class','0x060400'),('cat '+ROOT+'/power/runtime_status','active')]
    if boot is not None:
        required.append(('cat '+BOOT,boot))
    return ' && '.join('[ "$('+cmd+')" = '+shlex.quote(value)+' ]' for cmd,value in required)

def capture(execute, samples):
    start=execute(scope()+' && cat '+BOOT)
    if not re.fullmatch(UUID,start):
        raise ValueError('not an active PKG110 root port, or incomplete identity')
    guard=scope(start)
    def read32(offset):
        if type(offset) is not int or offset%4 or not 0<=offset<=4092 or len(samples)>=MAX_READS:
            raise ValueError('unexpected read location or read budget exhausted')
        # dd uses 4-byte blocks and seeks only this BDF; no PCI config payload dump.
        # Do not trust adb's exit status alone. Require the post-check and 4 actual bytes.
        cmd=(guard+' || exit 1; v=$(dd if='+ROOT+'/config bs=4 skip='+str(offset//4)+
             ' count=1 2>/dev/null | od -An -tx1) || exit 1; '+guard+
             ' || exit 1; printf "word:%s:end" "$v"')
        result=execute(cmd)
        m=re.fullmatch(r'word:\s*((?:[0-9a-fA-F]{2}\s+){3}[0-9a-fA-F]{2})\s*:end',result)
        if not m:
            raise ValueError(f'incomplete word or scope changed at 0x{offset:x}')
        value=int.from_bytes(bytes.fromhex(m[1]),'little')
        samples.append([offset,value])
        return value
    result=inspect_root(read32)
    if execute(guard+" && printf stable")!='stable':
        raise ValueError('root identity, boot or runtime state changed')
    return result

def replay(doc):
    if (not isinstance(doc,dict) or doc.get('schema')!=SCHEMA or
            doc.get('capture_complete') is not True):
        raise ValueError('missing complete selective root capture')
    rd=doc.get('dwords')
    if not isinstance(rd,list) or not 0<len(rd)<=MAX_READS:
        raise ValueError('invalid transcript size')
    pos=0
    def read32(offset):
        nonlocal pos
        if pos>=len(rd):
            raise ValueError(f'missing word at 0x{offset:x}')
        item=rd[pos];pos+=1
        if (not isinstance(item,list) or len(item)!=2 or
                type(item[0]) is not int or item[0]!=offset or
                type(item[1]) is not int or not 0<=item[1]<=0xffffffff):
            raise ValueError(f'unmatched word at 0x{offset:x}')
        return item[1]
    result=inspect_root(read32)
    if pos!=len(rd):
        raise ValueError('unconsumed evidence')
    return result

def adb_executor(adb, serial):
    if not adb.is_absolute() or not adb.is_file() or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,79}',serial):
        raise ValueError('provide an absolute platform-tools adb path and one USB serial')
    deadline=time.monotonic()+TIME
    def execute(script):
        remaining=deadline-time.monotonic()
        if remaining<=0:
            raise ValueError('capture deadline reached')
        process=subprocess.run([str(adb),'-s',serial,'exec-out','su -c '+shlex.quote(script)],
                               stdin=subprocess.DEVNULL,capture_output=True,timeout=min(8,remaining))
        if process.returncode or len(process.stdout)>256 or process.stderr.strip():
            raise ValueError('ADB/root read failed; no retry or fallback')
        try:
            return process.stdout.decode('ascii').strip()
        except UnicodeError as exc:
            raise ValueError('invalid Android response') from exc
    return execute

def main():
    p=argparse.ArgumentParser(description=__doc__)
    modes=p.add_mutually_exclusive_group(required=True)
    modes.add_argument('--read-active-android',action='store_true',help='explicit phone read; requires live authorization')
    modes.add_argument('--replay',type=Path,help='offline capture JSON; does not use ADB')
    p.add_argument('--adb',type=Path);p.add_argument('--serial')
    args=p.parse_args()
    if args.read_active_android!=(args.adb is not None and args.serial is not None) or (
            args.replay and (args.adb is not None or args.serial is not None)):
        p.error('supply --adb and --serial only with --read-active-android')
    out={'schema':SCHEMA,'capture_complete':False,'source':'offline-replay' if args.replay else 'stock-android-runtime',
         'cold_training_proven':False,'xnu_enumeration_proven':False}
    try:
        if args.replay:
            if args.replay.stat().st_size>128*1024:
                raise ValueError('oversized capture')
            doc=json.loads(args.replay.read_text(encoding='utf-8'))
            out['result']=replay(doc)
            out['dwords']=doc['dwords']
        else:
            out['dwords']=[]
            out['result']=capture(adb_executor(args.adb,args.serial),out['dwords'])
        out['capture_complete']=True
    except (ValueError,OSError,subprocess.TimeoutExpired) as exc:
        # Never print shell output, boot identity, USB serial or private file path.
        out['status']='unknown'
        out['reason']=str(exc) if type(exc) is ValueError else type(exc).__name__
    print(json.dumps(out,indent=2))
    return 0 if out['capture_complete'] else 2

if __name__=='__main__':
    sys.exit(main())
