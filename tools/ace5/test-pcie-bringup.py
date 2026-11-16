#!/usr/bin/env python3
"""Cloud-only tests of the actual SM8650 Wi-Fi board/PCIe transport sources.

MMIO and clock services are mocked; success cannot establish hardware support.
Use --check-only locally to inspect inputs without compiling C.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check-only', action='store_true')
    ap.add_argument('--cc', nargs=argparse.REMAINDER, default=['cc'])
    args = ap.parse_args()
    if not args.cc:
        ap.error('--cc needs a compiler')
    root = Path(__file__).resolve().parents[2]
    drivers = root/'xnu/files/bsd/dev'
    cases = [(drivers/f'giulia_pcie_{name}.c', root/f'tools/ace5/test-pcie-{name}.c')
             for name in ('phy', 'link', 'noc', 'qos')]
    cases += [
        (drivers/'giulia_arc.c', drivers/'giulia_gem_power.c', drivers/'giulia_gem_clock.c',
         drivers/'giulia_pcie_qos.c', drivers/'giulia_rpmh.c', drivers/'giulia_wlan_rails.c',
         root/'tools/ace5/test-gem-power.c'),
        (drivers/'giulia_gem_clock.c', drivers/'giulia_pcie_qos.c', root/'tools/ace5/test-gem-clock.c'),
        (drivers/'giulia_pcie_power.c', drivers/'giulia_pcie_qos.c', root/'tools/ace5/test-pcie-power.c'),
        (drivers/'giulia_aop.c', root/'tools/ace5/test-aop.c'),
        (drivers/'giulia_aop.c', drivers/'giulia_wlan_pdc.c', drivers/'giulia_wifi_gpio.c',
         root/'tools/ace5/test-aop-xnu.c'),
        (drivers/'giulia_aop.c', drivers/'giulia_wlan_pdc.c', root/'tools/ace5/test-wlan-pdc.c'),
        (drivers/'giulia_crm.c', root/'tools/ace5/test-crm.c'),
        (drivers/'giulia_crm.c', drivers/'giulia_bcm.c', root/'tools/ace5/test-crm-xnu.c'),
        (drivers/'giulia_crm.c', drivers/'giulia_bcm.c', root/'tools/ace5/test-bcm.c'),
        (drivers/'giulia_crm.c', drivers/'giulia_bcm.c', drivers/'giulia_rpmh.c',
         drivers/'giulia_wlan_rails.c', drivers/'giulia_pcie_cpu_noc.c', root/'tools/ace5/test-pcie-cpu-noc.c'),
        (drivers/'giulia_wifi_gpio.c', root/'tools/ace5/test-wifi-gpio.c'),
        (drivers/'giulia_rpmh.c', drivers/'giulia_wlan_rails.c', root/'tools/ace5/test-rpmh.c'),
        (drivers/'giulia_wifi_board.c', root/'tools/ace5/test-wifi-board.c'),
        (drivers/'giulia_wifi_supply.c', root/'tools/ace5/test-wifi-supply.c'),
        (drivers/'giulia_mhi_snapshot.c', root/'tools/ace5/test-mhi-snapshot.c'),
        (drivers/'giulia_aop.c', drivers/'giulia_pcie_noc.c', drivers/'giulia_bcm.c', drivers/'giulia_arc.c', drivers/'giulia_arc_votes.c',
         drivers/'giulia_crm.c', root/'tools/ace5/test-wifi-probe.c'),
    ]
    for case in cases:
        for path in case:
            path.read_text(encoding='utf-8')
    if args.check_only:
        print('PCIe bringup source inputs present; C tests NOT compiled or executed')
        return
    with tempfile.TemporaryDirectory(prefix='ace5-pcie-bringup-', dir=root.parent) as temporary:
        for case in cases:
            *sources, test = case
            exe = Path(temporary)/(test.stem + ('.exe' if sys.platform == 'win32' else ''))
            subprocess.run(args.cc + ['-std=c11', '-O1', '-g', '-UNDEBUG', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-I', str(drivers),
                *map(str, sources), str(test), '-o', str(exe)], check=True, timeout=90)
            subprocess.run([str(exe)], check=True, timeout=30)
            if test.stem == 'test-aop-xnu':
                subprocess.run([str(exe), '--pdc'], check=True, timeout=30)
                for index in range(1, 11):
                    subprocess.run([str(exe), '--pdc-timeout', str(index)], check=True, timeout=30)
                for index in (1, 9):
                    subprocess.run([str(exe), '--pdc-drift', str(index)], check=True, timeout=30)
            if test.stem == 'test-pcie-cpu-noc':
                for scenario in ('--timeout', '--cleanup', '--pretrigger'):
                    subprocess.run([str(exe), scenario], check=True, timeout=30)
            if test.stem == 'test-wifi-supply':
                for stage in 'GIADVCWP':
                    subprocess.run([str(exe), '--start-fail', stage], check=True, timeout=30)
                for stage in 'rpwcvda':
                    subprocess.run([str(exe), '--stop-fail', stage], check=True, timeout=30)
                for scenario in ('--modes','--keep-pdc','--external','--contract','--handoff','--reset-attempted','--early-stop-fail'):
                    subprocess.run([str(exe), scenario], check=True, timeout=30)
                for check in (1,2):
                    subprocess.run([str(exe), '--guard', str(check)], check=True, timeout=30)
            if test.stem == 'test-crm':
                for scenario in ('success', 'timeout', 'post-error', 'drift'):
                    subprocess.run([str(exe), '--empty-' + scenario], check=True, timeout=30)
            if test.stem == 'test-crm-xnu':
                for scenario in ('--retained', '--timeout', '--bandwidth', '--bandwidth-timeout'):
                    subprocess.run([str(exe), scenario], check=True, timeout=30)


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
