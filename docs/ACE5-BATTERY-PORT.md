# Ace 5 battery telemetry experiment

This is an opt-in source implementation, not hardware-validated battery support.
The normal one-click package does not enable it. `ace5_battery=1` is accepted only
with strict loader board identity `giulia`, the expected GIC map, `cpus=1`,
`bootcpu=1`, and a one-CPU XNU topology. All these checks precede MMIO access.

The live PKG110 device tree identifies slave `11-0055` as
`oplus,bq27541-battery`, with `oplus,batt_nfg8011b`, one battery cell, and the
external-command property. The generic Android driver name does not imply a
TI BQ27541 chip. Its controller is QUP2 SE0 at `0x880000`; active pinctrl maps
SDA to GPIO0 and SCL to GPIO1. Private device-tree dumps and battery samples
are not included in this repository.

`giulia_battery.c` uses open-drain GPIO I2C on the verified pins. It saves their
configuration, accepts only GPIO inputs or the documented QUP function, checks
for an idle bus, releases a line to signal high, and only actively drives low.
The original mux and data latches are restored after every poll. It does not
program GENI, regulators, charging limits or a new fuel-gauge configuration.
Physical bus ownership, pin access permissions and real gauge responses still
require an explicitly requested hardware test; an idle-line check is not proof
of exclusive ownership.

Each transaction is an address write, one register selector, repeated START,
two-byte read and STOP at slave address `0x55`. ACK/NACK, arbitration loss and
clock stretching are checked. SCL stretching is bounded to 5 ms per edge;
SDA setup intervals are explicitly at least 5 microseconds. A busy bus or lost
arbitration is released without issuing STOP into another transaction. No
reset/recovery clock train, unseal, authentication, data-flash or calibration
commands are sent.

The standard-mode registers are SOC `0x2c`, voltage `0x08`, signed current
`0x0c`, temperature `0x06`, remaining/full capacity `0x10`/`0x12`, and cycles
`0x2a`. The vendor uses `0x1a`/`0x1c` only when its fast-sampling mode is enabled;
this driver never enables that mode. The raw current is signed mA, with positive
meaning charge enters the battery; the Oplus public callback negates that sign.
Temperature is converted from tenths of kelvin using the vendor's `-2731`
offset. Failed or implausible samples are not published, and test values are
synthetic rather than production fallbacks.

Successful complete samples enter the existing `redwood_battery` sysctls and
`RedwoodBattery` IOKit service. The gauge alone does not establish charger
presence, so `external=-1` and charging status remains unknown. Sysctls retain
that unknown value; the IOKit service omits charger/charging properties instead
of asserting an unplugged or charging state. Whether the iOS UI accepts this
partial power-source report remains untested.

A normal return blocks new register reads and drains the current bounded
read-word, including STOP, before restoring the pins. Its wait has a 250 ms
deadline. Final/panic quiesce uses short non-preemptible GPIO sections, with no
sleep or allocation; resumed worker code cannot write after restoration on the
required single CPU. A panic that interrupts a slave mid-byte can still leave
the protocol unfinished: restoring pin configuration is not proof that the bus
is idle. Final quiesce deliberately sends no speculative recovery clocks.

Cloud regression command:

```sh
python3 tools/ace5/test-giulia-battery.py --cc clang-18
```

The tests compile the real protocol/sample implementation and extract the
actual board-entry/GPIO/stop functions with fake MMIO and XNU services. They
cover framing, setup time, NACK, arbitration, stretching, failed reads,
synthetic unit conversions, unknown charger state, opt-in/board/CPU rejection,
open-drain writes, mux restoration and normal/final stop behavior. Host tests
do not verify electrical behavior, the real device, or the iOS status bar.

Register and layout references:

- [OnePlus SM8650 modules and device trees](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8650), branch `oneplus/sm8650_b_16.0.0_ace_5`: `oplus_hal_bq27541.c/.h`, `oplus_hal_nfg8011b.h`, and `pineapple-pinctrl.dtsi`.
- [Linux SM8650 pin controller](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-sm8650.c): GPIO register stride, mux/OE/data bits and QUP2 SE0 function index.
- [XNU IOPMPowerSource](https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.121.2/iokit/Kernel/IOPMPowerSource.cpp): updating/removing properties requires handling its internal dictionary as well as IORegistry.
