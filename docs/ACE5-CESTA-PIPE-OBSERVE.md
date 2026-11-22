# Ace 5 PCIe0: separate CESTA-managed PIPE trial

Candidate J is based on E, not F. It leaves the non-opt-in path unchanged.
The archived PKG110 RC0 device trees have one host clock name, the CNOC SF
AXI clock, and do not supply a direct PIPE clock or mux handle.
[OnePlus's clock lookup](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/pci/controller/pci-msm.c#L4997-L5053)
tolerates a missing PIPE list on the CESTA-managed route. XNU's existing route
directly parks/selects the mux and votes for the branch. That is a software
sequencing difference, not a measurement of the endpoint or proof that software
and CESTA actually contend.

The software vote is GCC `0x52008` bit 4, and the published PIPE halt register
is `0x6b048`. The legacy sample at `0x6b040` is not that published branch.
[OnePlus GCC definition](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650/blob/66305c56af1fcd7299c906c3257e585be93d93ed/drivers/clk/qcom/gcc-pineapple.c)
and [upstream SM8650 definition](https://github.com/torvalds/linux/blob/v6.12/drivers/clk/qcom/gcc-sm8650.c)
both mark the branch `BRANCH_HALT_SKIP`; the new observations are a conservative
experimental admission condition, not Linux's normal wait rule or a clock-waveform
measurement.

## Guarded comparison

1. The host opt-in `--wifi-cesta-pipe-observe` requires the current DT, exact
   stock RC0 clock list, and all existing E Stage 4/endpoint/finite-reset guards.
   It rejects a kernel without the opt-in marker. No persistent log route.
2. During pre-START host preparation, preserve the PIPE mux and vote bit.
   Other necessary clock votes remain. Before any START, unwind only fields this
   route changed; never restore the unowned PIPE bit or mux.
3. Require the existing firmware-quiet/IRQ/independent-reset path, real early
   START and existing PCS-first preparation. Keep endpoint reset asserted.
   The QMP PIPE callback changes only software phase, not GCC registers.
4. A bounded poll requires two consecutive positive samples of the published
   branch with the PHY-selected mux, plus the existing PHY status and CESTA/CRM
   checks. All-ones, timeout, lease loss, or a non-qualifying sample cannot be
   rescued by a software vote or the old `0x6b040` sample. Stop before DBI if
   prerequisites fail. After START, retain state for the already armed reset;
   do not attempt a new PIPE disable or resume ordinary rollback.
5. Only the fully verified early route can proceed to the existing endpoint
   check. The PIPE observation is not a PCIe link or WCN7850 identity result;
   keep the exact endpoint identity and reset checks. No MHI, DMA or firmware
   loading is added.

This candidate does not establish that `0x6b048` has been measured on the phone,
that CESTA supplies endpoint REFCLK, or that the Wi-Fi chip has been enumerated.
Compare only with an independently validated E control. F's isolated
stock-lane experiment, git revision and private build package are not changed.
