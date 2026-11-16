#!/usr/bin/env python3
"""Ace 5 RAM boot and single-core trials. No flash, erase or slot changes.

The loader may write logdump, as explicitly authorized for this device.
All iOS images are sent to RAM. Other storage and unported drivers stay disabled.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import zlib


def loader_result(reply):
    """Require one actual getvar result; missing output is not completion."""
    matches = re.findall(r'^result:[ \t]*([a-z][a-z-]*)(?:[ \t].*)?\r?$', reply, re.M)
    if len(matches) != 1:
        raise RuntimeError('Missing or ambiguous loader result: ' + reply.strip())
    return matches[0]


def check_loader_result(reply, command, log):
    expected = 'selftest-ok' if command == 'selftest' else 'returned'
    actual = loader_result(reply)
    if actual != expected:
        raise RuntimeError(f'Loader {command} failed: {actual}; saved diagnostic log: {log}')


def wait_for_result(command, seconds, bootcpu, live_log, live_interval, log_dir, quiet_until_return=False):
    # The budget starts when launch has been accepted, including the wait while
    # CPU 0 owns USB. Do not grant a second full run duration after that wait.
    deadline = time.monotonic() + seconds + 60
    if command == 'run' and quiet_until_return:
        print('CRM trial parks firmware USB until kernel return; waiting before result polling.', flush=True)
        quiet_deadline = time.monotonic() + seconds + 5
        while time.monotonic() < quiet_deadline:
            time.sleep(min(5, quiet_deadline - time.monotonic()))
    elif command == 'run' and bootcpu == 0:
        print('XNU owns the boot core; waiting before asking USB for its result.', flush=True)
        time.sleep(seconds + 5)
    next_live = time.monotonic() + live_interval
    live_prefix = time.strftime('%Y%m%d-%H%M%S')
    live_count = 0
    while True:
        result = fb('getvar', 'result')
        if loader_result(result) != 'running':
            return result
        if time.monotonic() >= deadline:
            raise RuntimeError('Kernel did not return; preserve the screen and inspect the live log')
        if live_log and time.monotonic() >= next_live:
            response = call(['fastboot', '-s', 'xnuloader', 'oem', 'live-log'], check=False)
            # Stock fastboot discards the optional OKAY payload for OEM commands.
            if 'OKAY' in response and 'FAILED' not in response:
                live_count += 1
                path = log_dir / f'{live_prefix}-live-{live_count:03d}.log'
                fb('get_staged', path)
                print(f'Live log: {path.name} ({path.stat().st_size} bytes)', flush=True)
            elif 'no live kernel' not in response:
                raise RuntimeError('Live-log request failed: ' + response.strip())
            next_live = time.monotonic() + live_interval
        time.sleep(min(2, live_interval))


def collect_run_diagnostics(command, seconds, bootcpu, live_log, live_interval, log_dir, log, quiet_until_return=False):
    try:
        result = wait_for_result(command, seconds, bootcpu, live_log, live_interval, log_dir, quiet_until_return)
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        # Preserve what is already staged without "oem log", which would discard
        # a final kernel log. A failed poll cannot prove this is the final log.
        partial = log.with_name(log.stem + '-poll-failure-staged.log')
        try:
            fb('get_staged', partial)
            print(f'Saved staged diagnostics (final result unconfirmed): {partial}', file=sys.stderr)
        except (RuntimeError, subprocess.TimeoutExpired, OSError) as fetch_error:
            print(f'Could not fetch staged diagnostics after {error}: {fetch_error}', file=sys.stderr)
        raise
    # Fetch before checking the outcome: rejected/exceptional runs need their
    # diagnostics too, and idle "oem log" would discard the staged kernel log.
    fb('get_staged', log)
    check_loader_result(result, command, log)
    return result


def validate_run_timing(seconds, bootcpu, gpu_probe, gpu_direct, gpu_keep, gpu_step_delay, live_log):
    """Only the complete, independently observed GPU profile may omit its timer."""
    if not 0 <= seconds <= 3600:
        raise RuntimeError('seconds must be 0..3600')
    if not 0 <= gpu_step_delay <= 3000:
        raise RuntimeError('--gpu-step-delay must be between 0 and 3000 ms')
    if live_log and (bootcpu != 1 or seconds == 0):
        raise RuntimeError('--live-log requires --bootcpu 1 and a finite return timer')
    persistent = (seconds == 0 and bootcpu == 1 and gpu_probe == 20 and
                  gpu_direct and gpu_keep and gpu_step_delay == 0)
    if seconds == 0 and not persistent:
        raise RuntimeError('No-timer mode requires --bootcpu 1 --gpu-probe 20 '
                           '--gpu-direct --gpu-keep and zero trace delay')
    if gpu_probe and gpu_probe >= 3 and (bootcpu != 1 or (seconds == 0 and not persistent)):
        raise RuntimeError('GPU power probes require --bootcpu 1 and a finite return timer')
    if gpu_probe and gpu_probe >= 17 and not persistent and seconds < 10 + (240 * gpu_step_delay + 999) // 1000:
        raise RuntimeError('Return timer is too short for all GPU trace steps and cleanup')
    return persistent


def check_kernel_log(saved_log, log):
    if 'panic(cpu ' in saved_log or re.search(
            r'ace5-gpu: (stage [0-9]+ FAILED|.*cleanup FAILED|live failure)', saved_log):
        raise RuntimeError(f'Kernel/probe failure; saved diagnostic log: {log}')


def observe_persistent_startup(log_dir, log, timeout=120):
    """Observe startup without imposing a kernel timer or stopping it on failure."""
    deadline = time.monotonic() + timeout
    count = 0

    def require_running(result):
        actual = loader_result(result)
        if actual != 'running':
            fb('get_staged', log)
            raise RuntimeError(f'Persistent startup ended before confirmation: {actual}; '
                               f'saved diagnostic log: {log}')

    try:
        while time.monotonic() < deadline:
            require_running(fb('getvar', 'result'))
            response = call(['fastboot', '-s', 'xnuloader', 'oem', 'live-log'], check=False)
            if 'no live kernel' in response:
                # The kernel may have returned between getvar and live-log. Its
                # final staged log must be fetched before any other OEM command.
                require_running(fb('getvar', 'result'))
                raise RuntimeError('Live-log unavailable although loader still reports running')
            if 'OKAY' not in response or 'FAILED' in response:
                raise RuntimeError('Live-log request failed: ' + response.strip())
            count += 1
            snapshot = log_dir / f'{log.stem}-startup-{count:03d}.log'
            fb('get_staged', snapshot)
            saved_log = snapshot.read_bytes().decode('utf-8', errors='replace')
            check_kernel_log(saved_log, snapshot)
            ready = ('ace5-gpu: stage 20 complete' in saved_log and
                     'ace5-gpu: persistent startup ready=1' in saved_log)
            if ready:
                result = fb('getvar', 'result')
                require_running(result)
                if time.monotonic() >= deadline:
                    raise RuntimeError(f'Startup was not confirmed within {timeout} seconds')
                print(f'GPU startup confirmed in {snapshot}; loader still reports running. '
                      'iOS is left running without an automatic return timer. '
                      'The desktop may still be loading; this is not a stability test. '
                      'Use the reboot command to stop XNU and return to Android.', flush=True)
                return result
            time.sleep(min(2, max(0, deadline - time.monotonic())))
        raise RuntimeError(f'Startup was not confirmed within {timeout} seconds')
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        partial = log.with_name(log.stem + '-startup-unconfirmed-staged.log')
        try:
            fb('get_staged', partial)
        except (RuntimeError, subprocess.TimeoutExpired, OSError) as fetch_error:
            print(f'Could not save staged startup diagnostics: {fetch_error}', file=sys.stderr)
        raise RuntimeError(f'{error}. Kernel may still be running; no automatic stop or reboot was sent. '
                           'Use the reboot command for a graceful stop; if USB is unavailable, '
                           'hold physical power + volume-up to restart.') from error


def stop_running_kernel(log_dir, log):
    """Do not reset the device while its other core is known to own live GPU state."""
    try:
        if loader_result(fb('getvar', 'result')) != 'running':
            return
        fb('oem', 'stop')
        collect_run_diagnostics('run', 0, 1, False, 10, log_dir, log)
        check_kernel_log(log.read_bytes().decode('utf-8', errors='replace'), log)
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        raise RuntimeError(f'Graceful kernel stop was not confirmed: {error}. No reboot was sent. '
                           'Preserve the logs; if USB recovery fails, hold physical power + '
                           'volume-up to restart.') from error


def ace5_driver_arguments(bootcpu, kernel, battery, cpu_khz):
    """Explicit experimental hardware paths; the normal launcher leaves them off."""
    if not battery and cpu_khz == 0:
        return ''
    if not kernel:
        raise RuntimeError('--battery/--cpu-khz require an explicit --kernel built with these drivers')
    if battery and bootcpu != 1:
        raise RuntimeError('--battery requires --bootcpu 1 and the single-core run profile')
    if cpu_khz != 0 and not 600000 <= cpu_khz <= 1804800:
        raise RuntimeError('--cpu-khz must be 0 (off) or 600000..1804800 kHz; hardware selects a stock LUT entry')
    result = ' ace5_battery=1' if battery else ''
    if cpu_khz:
        result += f' ace5_cpu_khz={cpu_khz}'
    return result


ROOT = Path(__file__).resolve().parents[2]
def ace5_wifi_probe_arguments(mode, bootcpu, kernel, seconds):
    if not mode:
        return ''
    if mode not in (1, 2, 3, 4, 5) or not kernel or bootcpu != 1 or seconds <= 0:
        raise RuntimeError('--wifi-probe requires mode 1..5, --kernel, --bootcpu 1 and a finite run')
    return f' ace5_wifi_probe={mode}'


def check_wifi_probe_log(mode, text, log):
    if not mode:
        return
    markers = [f'ace5-wifi: probe={mode} read-only begin',
               'ace5-wifi: read-only probe complete']
    if 2 <= mode <= 4:
        markers.append('ace5-wifi: early CESTA/CRM headers captured')
    if 3 <= mode <= 4:
        markers.append('ace5-wifi: AOP descriptor captured')
    if mode == 4:
        markers.append('ace5-wifi: full SM/CRM snapshot captured; no table/channel writes')
    if mode == 5:
        markers.append('ace5-wifi: HLOS ARC snapshot captured; resource ownership NOT assumed')
    if any(marker not in text for marker in markers):
        raise RuntimeError(f'Wi-Fi handoff probe incomplete; see {log}')


def ace5_aop_test_arguments(enabled, bootcpu, kernel, seconds, other_hardware):
    if not enabled:
        return ''
    if not kernel or bootcpu != 1 or not 10 <= seconds <= 300 or other_hardware:
        raise RuntimeError('--aop-channel-test requires --kernel, --bootcpu 1, 10..300 seconds and no other hardware probes')
    return ' ace5_aop_channel_test=1'


def ace5_cx_vote_arguments(enabled, bootcpu, kernel, seconds, other_hardware, live_log):
    if not enabled:
        return ''
    if not kernel or bootcpu != 1 or not 10 <= seconds <= 120 or other_hardware or live_log:
        raise RuntimeError('--aop-cx-vote-test requires verified firmware profile, --kernel, CPU1, 10..120 seconds, no other probes and no live-log')
    return ' ace5_aop_channel_test=3'


def ace5_crm_test_arguments(enabled, bootcpu, kernel, seconds, other_hardware, live_log, bandwidth=False):
    if not enabled:
        return ''
    if not kernel or bootcpu != 1 or not 10 <= seconds <= 120 or other_hardware or live_log:
        raise RuntimeError('--crm-channel-test requires --kernel, CPU 1, 10..120 seconds, no live USB polling and no other hardware probes')
    return ' ace5_crm_channel_test=2 ace5_crm_fixed_bw=1' if bandwidth else ' ace5_crm_channel_test=1'


def ace5_pdc_test_arguments(enabled, table, bootcpu, kernel, seconds, other_hardware, live_log):
    if not enabled:
        if table:
            raise RuntimeError('--wlan-pdc-table requires --wlan-pdc-test')
        return ''
    if not table or not kernel or bootcpu != 1 or not 10 <= seconds <= 120 or other_hardware or live_log:
        raise RuntimeError('--wlan-pdc-test requires current DT table, --kernel, CPU1, 10..120 seconds, no live-log and no other hardware probes')
    if not table.is_file() or table.stat().st_size != 508:
        raise RuntimeError('Missing or invalid current-DT PDC table (508 bytes); no upload attempted')
    return ' ace5_aop_channel_test=2'


ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--serial', required=True, help='Original ADB/bootloader serial')
sub = ap.add_subparsers(dest='command', required=True)
enter = sub.add_parser('enter', help='Temporary boot into the giulia loader')
enter.add_argument('--stage', choices=['diagnostic', 'kernel'], default='diagnostic')
ramdisk = sub.add_parser('ramdisk', help='Upload a system image to RAM')
ramdisk.add_argument('--image', type=Path, help='Explicit local system APFS image; defaults to userland/system.apfs')
run = sub.add_parser('run', help='Run single-core iOS, then save logs and framebuffer')
run.add_argument('--seconds', type=int, default=120,
                 help='Return after this many seconds; 0 requires the complete CPU 1 persistent GPU profile')
run.add_argument('--verbose', action='store_true')
run.add_argument('--kernel', type=Path, help='Explicit locally built Mach-O kernel')
run.add_argument('--root-image', type=Path, help='Local small APFS image, e.g. with device firmware')
run.add_argument('--touch', action='store_true', help='Enable the giulia S3910 driver; requires --kernel')
run.add_argument('--wifi-probe', type=int, choices=[1, 2, 3, 4, 5],
                 help='Read-only Ace 5 handoff probe: 1 baseline, 2 CESTA/CRM headers, 3 AOP headers, 4 full SM/CRM snapshot, 5 HLOS ARC votes with a separately verified firmware profile; requires --kernel, CPU 1 and a finite run')
run.add_argument('--aop-channel-test', action='store_true',
                 help='Open/close only the AOP host channel while loader firmware clients are parked; requires CPU 1 and 10..300 seconds')
run.add_argument('--aop-cx-vote-test', action='store_true',
                 help='Query current AOP CX votes through shared SRAM; requires separately verified firmware profile, CPU1, finite run and no live-log')
run.add_argument('--wlan-pdc-test', action='store_true',
                 help='Deliver the 10 stock AOP WLAN PDC messages with WLAN/BT quiet; no rail/radio startup; CPU1, finite <=120 seconds, no live-log; cold reboot afterwards')
run.add_argument('--wlan-pdc-table', type=Path,
                 help='508-byte qcom,pdc_init_table property extracted from this phone current DT; kernel compares every byte against the supported board table')
run.add_argument('--crm-channel-test', action='store_true',
                 help='Select empty CRM channel0 with zero tables under firmware quiet; requires CPU1, 10..120 seconds, no live-log; cold reboot required afterwards')
run.add_argument('--crm-bandwidth-test', action='store_true',
                 help='Stage PCIe0 fixed avg500/peak800 profile from current CMD DB under firmware quiet; caller verifies current DT policy; requires CPU1, finite run, no live-log and cold reboot afterwards')
run.add_argument('--battery', action='store_true',
                 help='Experimental Ace 5 NFG8011B sampling; requires --kernel and --bootcpu 1')
run.add_argument('--cpu-khz', type=int, default=0,
                 help='Experimental A520 EPSS frequency ceiling, 600000..1804800 kHz; 0 leaves stock boot state unchanged')
run.add_argument('--gpu-probe', type=int, choices=range(1, 21),
                 help='Ace 5 GPU probe: 1 GCC, 2 GPUCC, 3 CX power, 4 XO clocks/GMU, 5 TCM, 6 streams, 7 firmware, 8 SMMU, 9 operating clocks, 10 GMU boot, 11 HFI query, 12 power tables, 13 core firmware, 14 scheduler memory, 15 GX power, 16 GPU SMMU and firmware, 17 CP initialization, 18 ZAP authentication, 19 nonsecure CP DMA, 20 BR/BV indirect buffer; requires --kernel')
run.add_argument('--bootcpu', type=int, choices=[0, 1], default=0,
                 help='0: validated boot core; 1: experimental separate USB observer core')
run.add_argument('--live-log', action='store_true',
                 help='Save live log snapshots every 10 seconds; requires CPU 1 and a finite run')
run.add_argument('--live-interval', type=float, default=10,
                 help='Seconds between live-log snapshots, 0.1 to 60 (default 10)')
run.add_argument('--gpu-step-delay', type=int, default=0,
                 help='Pause 0 to 3000 ms before each new GPU core register access for tracing')
run.add_argument('--gpu-direct', action='store_true',
                 help='Use a host CP ring instead of GMU HWSCHED; requires --gpu-probe 13 or later')
run.add_argument('--gpu-keep', action='store_true',
                 help='Publish the experimental A750 RedwoodGPU backend; requires direct stage 20 and zero trace delay')
run.add_argument('--render-trace', action='store_true',
                 help='Log each desktop frame and GL calls slower than 20 ms; requires --gpu-keep')
run.add_argument('--frame-timing', action='store_true',
                 help='Log touch, compositor, presentation and CPU timing for tools/frames.py')
run.add_argument('--thread-trace', choices=['SpringBoard', 'backboardd', 'Preferences', 'wifid',
                                          'mediaserverd', 'PosterBoard', 'CollectionsPoster'],
                 help='Sample up to 16 threads with the existing kernel stack sampler every 5 seconds')
run.add_argument('--metal', action='store_true',
                 help='Allow Metal discovery properties; requires --gpu-keep and a root with RedwoodMetal.bundle')
run.add_argument('--gpu-cached', action='store_true',
                 help='Experimental cached buffers with synchronous ownership; requires --gpu-keep')
run.add_argument('--fb-filter', choices=['bilinear', 'nearest'],
                 help='Display scaling filter; nearest trades edge smoothness for less CPU work')
run.add_argument('--gpu-display', action='store_true',
                 help='Experimental A750 hardware scaling directly into the panel; requires --gpu-keep')
selftest = sub.add_parser('selftest')
selftest.add_argument('--kernel', type=Path, help='Explicit locally built Mach-O kernel')
sub.add_parser('screenshot')
sub.add_parser('reboot', help='Return to the unchanged Android boot chain')
args = ap.parse_args()
logs = ROOT / 'logs/giulia'
logs.mkdir(parents=True, exist_ok=True)

def call(command, timeout=20, check=True):
    p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    output = p.stdout + p.stderr
    if check and (p.returncode or re.search(r'FAILED|^ERROR:', output, re.M)):
        raise RuntimeError(output.strip())
    return output

def fb(*command, timeout=20):
    return call(['fastboot', '-s', 'xnuloader', *map(str, command)], timeout)

def getvar(key):
    answer = fb('getvar', key)
    m = re.search(r'^' + re.escape(key) + r': (.*)$', answer, re.M)
    return m.group(1).strip() if m else ''

def ensure_loader(kernel=False):
    if (getvar('product') != 'xnuloader' or getvar('platform') != 'giulia' or
            getvar('storage-writes') != 'disabled'):
        raise RuntimeError('Requires the guarded giulia loader built by these tools')
    if kernel and getvar('build-stage') != 'kernel':
        raise RuntimeError('Diagnostic loader cannot execute XNU; enter --stage kernel first')

def wait_device(serial):
    for _ in range(60):
        devices = call(['fastboot', 'devices'])
        if any(line.split() and line.split()[0] == serial for line in devices.splitlines()):
            return
        time.sleep(1)
    raise RuntimeError(f'Device {serial} did not appear; no flash fallback is attempted')

def screenshot():
    ensure_loader()
    w, h, stride = map(int, getvar('screen').split())
    fb('oem', 'screenshot')
    raw_path = logs / 'screen.raw'
    fb('get_staged', raw_path, timeout=60)
    data = raw_path.read_bytes()
    if not (0 < w <= 4096 and 0 < h <= 8192 and stride >= w * 4 and len(data) == stride * h):
        raise RuntimeError('Unexpected framebuffer geometry')
    rows = bytearray()
    for y in range(h):
        row = data[y * stride:y * stride + w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b'\0' + rgb
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = logs / (time.strftime('%Y%m%d-%H%M%S') + '-screen.png')
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    raw_path.unlink()
    print(png)

try:
    if args.command == 'enter':
        directory = ROOT / ('out/giulia' if args.stage == 'diagnostic' else 'out/giulia-kernel')
        image = directory / f'hackios-giulia-{args.stage}.img'
        manifest = json.loads(image.with_suffix('.json').read_text())
        if (manifest['stage'] != args.stage or manifest['allowed_partition_write'] != ['logdump'] or
                hashlib.sha256(image.read_bytes()).hexdigest() != manifest['sha256']):
            raise RuntimeError('Image manifest or SHA-256 mismatch')
        adb = ['adb', '-s', args.serial]
        if call(adb + ['get-state'], check=False).strip() == 'device':
            if call(adb + ['shell', 'getprop', 'ro.product.model']).strip() != 'PKG110':
                raise RuntimeError('Connected Android device is not PKG110')
            call(adb + ['reboot', 'bootloader'])
        wait_device(args.serial)
        stock = ['fastboot', '-s', args.serial]
        for key, value in [('product', 'pineapple'), ('unlocked', 'yes')]:
            if not re.search(r'^' + key + ': ' + value + r'$', call(stock + ['getvar', key]), re.M):
                raise RuntimeError('Unexpected or locked bootloader')
        print(call(stock + ['boot', str(image)], timeout=60), flush=True)
        wait_device('xnuloader')
        ensure_loader(kernel=args.stage == 'kernel')
        print('Giulia loader ready; Android partitions were not flashed.')
    elif args.command == 'ramdisk':
        ensure_loader(kernel=True)
        image = args.image.resolve() if args.image else ROOT / 'userland/system.apfs'
        if not image.is_file() or not 0 < image.stat().st_size < 0xffffffff:
            raise RuntimeError('Missing or oversized system image')
        fb('oem', 'ramdisk')
        print(fb('stage', image, timeout=600))
    elif args.command in ('run', 'selftest'):
        ensure_loader(kernel=True)
        payload = ROOT / 'out/giulia-kernel/run-payload.bin'
        seconds = args.seconds if args.command == 'run' else 15
        persistent = args.command == 'run' and validate_run_timing(
            seconds, args.bootcpu, args.gpu_probe, args.gpu_direct, args.gpu_keep,
            args.gpu_step_delay, args.live_log)
        commandline = (f'cpus=1 loader_return_after={seconds} epoch={int(time.time())} sb_delay=8 '
                       'touchscreen=0 buttons=0 cpufreq=0 gpu=0 wifi=0 adsp=0')
        if args.command == 'run':
            if args.crm_channel_test and args.crm_bandwidth_test:
                raise RuntimeError('--crm-channel-test and --crm-bandwidth-test are mutually exclusive')
            commandline += ace5_driver_arguments(args.bootcpu, args.kernel, args.battery, args.cpu_khz)
        if args.command == 'run':
            commandline += ace5_wifi_probe_arguments(args.wifi_probe, args.bootcpu, args.kernel, seconds)
            commandline += ace5_aop_test_arguments(args.aop_channel_test, args.bootcpu, args.kernel,
                seconds, args.wifi_probe or args.touch or args.battery or args.cpu_khz or args.gpu_probe or args.crm_channel_test or args.crm_bandwidth_test or args.wlan_pdc_test or args.aop_cx_vote_test)
            commandline += ace5_cx_vote_arguments(args.aop_cx_vote_test,args.bootcpu,args.kernel,seconds,
                args.wifi_probe or args.touch or args.battery or args.cpu_khz or args.gpu_probe or args.aop_channel_test or args.crm_channel_test or args.crm_bandwidth_test or args.wlan_pdc_test,
                args.live_log)
            commandline += ace5_crm_test_arguments(args.crm_channel_test or args.crm_bandwidth_test, args.bootcpu, args.kernel,
                seconds, args.wifi_probe or args.touch or args.battery or args.cpu_khz or args.gpu_probe or args.aop_channel_test or args.wlan_pdc_test or args.aop_cx_vote_test,
                args.live_log, args.crm_bandwidth_test)
            commandline += ace5_pdc_test_arguments(args.wlan_pdc_test, args.wlan_pdc_table,
                args.bootcpu, args.kernel, seconds,
                args.wifi_probe or args.touch or args.battery or args.cpu_khz or args.gpu_probe or args.aop_channel_test or args.crm_channel_test or args.crm_bandwidth_test or args.aop_cx_vote_test,
                args.live_log)
        if args.command == 'run' and args.touch:
            if not args.kernel:
                raise RuntimeError('--touch requires an explicit locally built --kernel')
            commandline += ' ace5_touch=1'
        if args.command == 'run' and args.verbose:
            commandline += ' -v'
        if args.command == 'run' and args.gpu_direct:
            if not args.gpu_probe or args.gpu_probe < 13:
                raise RuntimeError('--gpu-direct requires --gpu-probe 13 or later')
            commandline += ' ace5_gpu_direct=1'
        if args.command == 'run' and args.gpu_keep:
            if args.gpu_probe!=20 or not args.gpu_direct or args.gpu_step_delay:
                raise RuntimeError('--gpu-keep requires --gpu-probe 20 --gpu-direct and zero trace delay')
            commandline += ' ace5_gpu_keep=1'
        if args.command == 'run' and args.render_trace:
            if not args.gpu_keep:
                raise RuntimeError('--render-trace requires --gpu-keep')
            commandline += ' glslow=20000 glframes=1'
        if args.command == 'run' and args.frame_timing:
            commandline += ' touch_lat=1 cpuload=1000'
        if args.command == 'run' and args.metal:
            if not args.gpu_keep:
                raise RuntimeError('--metal requires --gpu-keep')
            commandline += ' metal=1'
        if args.command == 'run' and args.gpu_cached:
            if not args.gpu_keep:
                raise RuntimeError('--gpu-cached requires --gpu-keep')
            commandline += ' gpu_cached=2'
        if args.command == 'run' and args.fb_filter:
            commandline += f' fb_filter={int(args.fb_filter == "bilinear")}'
        if args.command == 'run' and args.gpu_display:
            if not args.gpu_keep:
                raise RuntimeError('--gpu-display requires --gpu-keep')
            commandline += ' fb_gpu=1'
        if args.command == 'run' and args.thread_trace:
            commandline += (f' ustacks={args.thread_trace} ustacks_at=25 ustacks_ms=5000 '
                            'ustacks_count=60 ustacks_tname=* ustacks_threads=16 cpuload=1000')
        if args.command == 'run' and args.gpu_probe:
            if args.gpu_probe>=18 and not args.gpu_direct:
                raise RuntimeError('ZAP stages require --gpu-direct')
            if not args.kernel:
                raise RuntimeError('--gpu-probe requires an explicit locally built --kernel')
            commandline += f' ace5_gpu_probe={args.gpu_probe}'
            commandline += f' ace5_gpu_step_delay={args.gpu_step_delay}'
        live_interval=args.live_interval if args.command=='run' else 10
        if not 0.1<=live_interval<=60:
            raise RuntimeError('--live-interval must be between 0.1 and 60 seconds')
        if args.command == 'run' and args.bootcpu:
            commandline += f' bootcpu={args.bootcpu}'
        kernel = args.kernel if args.kernel else ROOT / 'prebuilt/mach.development.bcm2837'
        if not kernel.is_file():
            raise RuntimeError(f'Missing kernel: {kernel}')
        print('Kernel SHA-256:', hashlib.sha256(kernel.read_bytes()).hexdigest(), flush=True)
        command = [sys.executable, str(ROOT / 'xnuloader/mkpayload.py'), '-o', str(payload),
                   f'kernel={kernel.resolve()}']
        if args.command == 'run':
            root_image = args.root_image or ROOT / 'userland/root.apfs'
            if not root_image.is_file() or not 0 < root_image.stat().st_size <= 32 * 1024 * 1024:
                raise RuntimeError('Missing or oversized small root image (limit 32 MiB)')
            print('Root image SHA-256:', hashlib.sha256(root_image.read_bytes()).hexdigest(), flush=True)
            command.append(f'ramdisk={root_image.resolve()}')
        command += ['--cmdline', commandline]
        print(call(command))
        if args.command == 'run' and args.wlan_pdc_test:
            fb('stage', args.wlan_pdc_table, timeout=15)
            print(fb('oem', 'wlan-pdc-table'), flush=True)
        fb('stage', payload, timeout=60)
        print(fb('oem', 'run' if args.command == 'run' else 'selftest'), flush=True)
        log = logs / (time.strftime('%Y%m%d-%H%M%S') + '-' + args.command + '.log')
        if persistent:
            observe_persistent_startup(logs, log)
            sys.exit(0)
        result = collect_run_diagnostics(args.command, seconds,
                                         args.bootcpu if args.command == 'run' else 0,
                                         args.live_log if args.command == 'run' else False,
                                         live_interval, logs, log,
                                         args.command == 'run' and (args.crm_channel_test or args.crm_bandwidth_test or args.wlan_pdc_test))
        print(result, log)
        saved_log = log.read_bytes().decode('utf-8', errors='replace')
        for line in saved_log.splitlines():
            if re.search(r'ace5-gpu: (stage [0-9]+|probe ended|.*cleanup FAILED)', line):
                print(line, flush=True)
        check_kernel_log(saved_log, log)
        if args.command == 'run':
            check_wifi_probe_log(args.wifi_probe, saved_log, log)
            if (args.aop_channel_test or args.aop_cx_vote_test) and 'ace5-aop: channel handshake complete; inherited link preserved; firmware resumed' not in saved_log:
                raise RuntimeError(f'AOP channel handshake incomplete; see {log}')
            if args.aop_cx_vote_test and 'ace5-cx-vote: AOP capture complete; no ARC access or power requests' not in saved_log:
                raise RuntimeError(f'AOP CX vote capture incomplete; see {log}')
            if args.wlan_pdc_test:
                marker = 'ace5-pdc: all 10 stock messages acknowledged; table/GPIO/firmware quiet retained until kernel return'
                if marker not in saved_log or getvar('power-reset-required') != 'yes':
                    raise RuntimeError(f'WLAN PDC delivery incomplete; preserve {log} and cold reboot before another operation')
                print('All 10 PDC messages acknowledged; no PMIC readback or radio startup claimed; cold reboot required.', flush=True)
            if args.crm_channel_test or args.crm_bandwidth_test:
                marker = 'ace5-crm: channel test complete; selected channel and firmware quiet retained until kernel return'
                if marker not in saved_log or getvar('crm-reset-required') != 'yes':
                    raise RuntimeError(f'CRM channel test incomplete; preserve {log} and cold reboot before another operation')
                specific = ('ace5-crm: bandwidth profile acknowledged;' if args.crm_bandwidth_test else
                            'ace5-crm: empty channel0 acknowledged; all PERF/BW table words remain zero')
                if specific not in saved_log:
                    raise RuntimeError(f'CRM requested profile not confirmed; preserve {log} and cold reboot')
                print('CRM requested profile confirmed; cold reboot required before another kernel or payload upload.', flush=True)
        if args.command == 'run' and args.gpu_probe and f'ace5-gpu: stage {args.gpu_probe} complete' not in saved_log:
            raise RuntimeError(f'GPU probe did not complete requested stage {args.gpu_probe}; see {log}')
        if args.command == 'run' and args.gpu_keep and 'ace5-gpu: persistent startup ready=1' not in saved_log:
            raise RuntimeError(f'A750 backend did not start; see {log}')
        screenshot()
    elif args.command == 'screenshot':
        screenshot()
    else:
        ensure_loader()
        stop_running_kernel(logs, logs / (time.strftime('%Y%m%d-%H%M%S') + '-stop.log'))
        print(fb('reboot'))
except (RuntimeError, subprocess.TimeoutExpired, OSError, ValueError) as error:
    raise SystemExit(str(error))
