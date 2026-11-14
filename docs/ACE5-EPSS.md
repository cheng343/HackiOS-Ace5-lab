# Ace 5 A520 EPSS performance request

This experimental XNU driver is off by default. `ace5_cpu_khz=N` requests a
firmware-programmed CPU frequency of at most N kHz. Accepted nonzero requests
are 600000 through 1804800 kHz, rounded down to an eligible hardware LUT row.
It does not turn on additional CPU cores. Initially it admits exactly one XNU
CPU with MPIDR affinity ID `0x000` or `0x100` (the two A520s). All other
topologies are rejected before MMIO. The one-click package does not enable it.

It is a fixed, bounded performance request, not an Android-style governor.
The limit is a conservative experimental choice, not a proven device thermal
limit. Hardware voltage sequencing and LMh controls are left unchanged;
Android's software thermal/charging policies are not running in XNU. There is
no DDR, RPMh, CPU-enable, voltage, thermal or interrupt-register programming.
The existing `cpuload` diagnostic starts before board-specific clock dispatch,
so frame/thread timing remains available even while this driver is disabled.

## Public hardware references

The domain layout is in upstream Linux
[sm8650.dtsi](https://github.com/torvalds/linux/blob/v6.12/arch/arm64/boot/dts/qcom/sm8650.dtsi).
The register protocol and EPSS LUT interpretation were checked against the
OnePlus Ace 5 kernel branch at commit
[`66305c56af1fcd7299c906c3257e585be93d93ed`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/cpufreq/qcom-cpufreq-hw.c)
and [upstream qcom-cpufreq-hw.c](https://github.com/torvalds/linux/blob/master/drivers/cpufreq/qcom-cpufreq-hw.c).
The XNU implementation is original code based on this interface; Linux kernel
code is not included. Private device trees and device logs are not distributed.

| Android CPUs | Hardware domain | Register base | Per-core slots |
| --- | --- | --- | --- |
| 0, 1 | 0 | 0x17d91000 | 0, 1 |
| 5, 6 | 1 | 0x17d92000 | 0, 1 |
| 7 | 2 | 0x17d93000 | 0 |
| 2, 3, 4 | 3 | 0x17d94000 | 0, 1, 2 |

Each domain has a 0x1000-byte register window. Only domain 0 is mapped by this
initial driver. Register offsets used are enable `0x0` (read only), domain
state `0x20` (read only), per-core DCVS control `0xb0` (read only), frequency
LUT `0x100 + row*4` (40 entries, read only), and performance request
`0x320 + slot*4`. XO frequency is 19200 kHz. The alternate-clock row is not
selected. Firmware LUT rows with a reduced core-count field are excluded as
boost rows, following the Ace 5 vendor driver's EPSS semantics. In particular,
core-count 1 by itself is not assumed to be a generic turbo flag.

When per-core DCVS is set, only the XNU A520's corresponding vote is changed.
Otherwise the domain has a shared vote, so the loader's A520 also shares the
requested frequency. The original register value is saved before writing,
the request is read back, and loader return restores exactly that saved value
after the other CPUs have stopped. The restore hook performs no MMIO when
the driver was disabled or made no change. No worker thread can reapply a vote
after restore. A failed write readback attempts immediate restoration.

## Validation and limits

`tools/ace5/test-giulia-cpufreq.c` includes the actual driver with kernel stubs
and a RAM register window. Cloud CI checks opt-in/platform/topology rejection,
invalid tables, bounded/non-boost row selection, both DCVS slot modes, no writes
to unrelated registers, and exact restoration with interrupts disabled.
These tests do not establish EPSS behavior on a real device.

In a limited PKG110 trial on 2026-11-14 (kernel `6c79fd0`), requesting 1200000
kHz changed the shared domain-0 vote from row 12 to row 7. Both the selected LUT
frequency and the domain-state reading were 1132800 kHz. Graceful return
restored slot `0x320` to row 12 with matching readback, followed by a successful
Android boot. The user reported worse UI responsiveness in that combined
trial; the GPU had also refused the no-timer boot profile. This does not
isolate the contribution of either change or demonstrate a speedup. A lower
ceiling can lower the firmware's existing vote. Subsequent graphics comparisons
should leave `ace5_cpu_khz` disabled to preserve that original vote.

Useful loader log prefix: `ace5-cpufreq:`. The successful vote line distinguishes
the requested cap, selected LUT frequency and hardware domain-state reading.
The hardware reading may be lower because of hardware limits; it is never used
to assert an achieved frequency or UI improvement. Returning to the loader
should log `restore ... done`. GPU acceleration and multicore scheduling are
separate work and are not claimed by this driver.
