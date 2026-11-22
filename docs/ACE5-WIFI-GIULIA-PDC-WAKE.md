# Ace 5 GPIO18/19 PDC wake forwarding

This is a separate source candidate based on the guarded K PIPE-only build.
It does not replace the default HackiOS device initialization.

## Why this is the first transplant

The [same-board UniXNU report](https://github.com/tycykp/UniXNU-OnePlus-Ace5/blob/aaa100d19f6fb14e5897a7e496edf7838d4d4e9f/hardware/sm8650-wifi-pdc-routing.json)
records that routing GPIO18/19 wake events to PDC let a PKG110 reach L0
and enumerate `17cb:1107`, after earlier Detect.Quiet failures. It does
not report a working network. Our archived PKG110 Android FDT independently
has an active CNSS Kiwi node with `mpm_wake_set_gpios = <18 19>`. The FDT
was checked as existing local evidence, not newly acquired or distributed.

The existing ten AOP WLAN PDC table messages are separate from these TLMM
pin wake-forwarding bits. The original reset, regulator and PCIe paths
are unchanged. The new GPIO transaction is implemented in this tree with
our GPIO owner and guard, using the public reference for register semantics.

## Isolation and ordering

- The new boot mode is opt-in and requires the pre-existing guarded Stage 4
  PIPE-only parent profile. It branches away before that profile's normal
  clock/MMIO sequence. Only the shared loader mailbox, GIC and GPIO bank
  are mapped. No AOP mailbox, RPMh, PCIe controller, PHY, DBI, iATU, CFG or
  BAR aperture is mapped by this mode.
- Acquire firmware quiet and IRQ37 isolation. Arm the existing independent
  cold return before acquiring the GPIO outputs. Use the existing owner to
  hold WLAN_EN, BT_EN and PERST low; check both GPIO18/19 inputs and their
  routing capability before changing either.
- The only change to GPIO18/19 is an opt-in RMW of bit 7 in INTR_CFG after
  verifying capability bit 6 and that the local APSS IRQ is off. Keep any
  inherited forwarding in place; no GPIO18/19 direction, mux, IRQ selector,
  edge, pending or interrupt-enable writes. Require a fresh lease for I/O
  and readback. A partial write retains state instead of guessing rollback.
- This route-only mode does **not** raise WLAN_EN, release PERST, send the
  AOP table, vote regulators, start CESTA, train LTSSM or bring up the radio.
  After verification it leaves the isolated state to the already-armed
  cold return. The current independent deadline is not extended and does
  not include the potentially longer AOP transaction.

Cloud C fixtures should verify admission, dropped writes, exact write
masks, partial ownership, revoked leases and the no-PCIe-mapping boundary
before any real run. Even a passing route-only device receipt is not link
enumeration. A later, separate link candidate must validate PDC readback
before training, then require L0 and exact `17cb:1107` before proceeding to
firmware, MSI/DMA or WLAN functionality.
