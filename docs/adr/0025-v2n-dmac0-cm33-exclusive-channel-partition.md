# 0025. V2N DMAC0: CM33-exclusive ownership, channels 0-1 reserved

Status: Proposed
Date: 2026-08-07

## Context

RZ/V2N's MCPU DMAC (`DMAC0`, base `0x11400000`) is dual-claimable: the
CM33 firmware arms it directly through the vendored Renesas FSP
`r_dmac_b` module for the GD32 supervisor link's SCI7 SPI DMA fast
path, and the upstream Linux SoC devicetree also declares a
`dma-controller@11400000` node the `rz-dmac` driver binds
unconditionally when enabled. Both sides reach the same 8 channels and
their 8 GIC interrupts.

Silicon evidence, recorded verbatim in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi:326-338`:
with the Linux node enabled, "rz-dmac bound + 8 GIC IRQs claimed on the
live system while the CM33 FSP was arming the same channels"
(2026-06-06). The current mitigation is a whole-unit disable —
`&dmac0 { status = "disabled"; }` — landed against issue #84
(`dev 1deb984`, per that issue's own "Fixed already" list).

alp-sdk#1152 asks for the ownership split to be a *documented
contract*, not just an emergent side effect of one dtsi line, and for
that contract to name the channel-level detail: which channels the
CM33 actually uses, and what's asked of the Linux side going forward.

**What the CM33 side actually claims**, from
`zephyr/drivers/spi/spi_renesas_rz_sci_b.c:257-259`:

```c
#define ALP_V2N_SCI7_DMAC_UNIT  0 /* DMAC0 = MCPU (CM33) DMAC            */
#define ALP_V2N_SCI7_DMAC_RX_CH 0 /* DMAINT0 -> NVIC 89                   */
#define ALP_V2N_SCI7_DMAC_TX_CH 1 /* DMAINT1 -> NVIC 90                   */
```

Channel 0 (RX) and channel 1 (TX) are the CM33's only DMAC0 claim
today, wired to the SCI7 Simple-SPI RXI/TXI triggers. This path is
currently gated off at compile time (`ALP_V2N_SCI7_DMAC` defaults to
`0`, `spi_renesas_rz_sci_b.c:235-237`) pending the unrelated silicon
issue #84 (TX requests stop after the post-init idle window); the
channel *reservation* is independent of that gate — the FSP config
structs name ch0/ch1 whether or not the path is compiled in.

There is no `renesas,rz-dmac-b` devicetree node in the Zephyr RZ/V2N
SoC port at all (`r_dmac_b_cfg.h:14-15`: "there is no
renesas,rz-dmac-b node in the rzv2n devicetree"); the CM33 configures
DMAC0 by writing FSP config structs directly, not through Zephyr's
device-tree DMA abstraction. So there is no DT-expressible per-channel
partition to publish on the CM33 side — the partition is a fact about
which channel numbers the C code claims, not a devicetree property.

## Decision

**DMAC0 stays CM33-exclusive, whole unit.** The A55/Linux side MUST
NOT enable `&dmac0` under any circumstance — this is Expected Behavior
option 2 from #1152 ("DMAC0 exclusively CM33-owned with Linux using a
different DMAC instance"), not a per-channel split enforced by a
devicetree mask. Two things make a Linux-side channel mask the wrong
answer to write down as the contract, rather than merely undone work:

1. No such mechanism is evidenced anywhere in this tree or the
   upstream `rz-dmac` binding. Fabricating a `dma-channel-mask`-style
   property here would be inventing a devicetree contract this driver
   has never been shown to honor — worse than the whole-unit disable
   it would replace.
2. The A55 has four other DMAC units and does not need DMAC0 for
   anything today (`e1m-v2n-som.dtsi:334`, "The A55 has four other
   DMAC units; this one is off-limits"). There is no Linux DMA need
   this SoM is blocked on by keeping the whole node off.

**Within the CM33's exclusive ownership, the channel assignment is:**
ch0 (RX, DMAINT0 → NVIC 89) + ch1 (TX, DMAINT1 → NVIC 90) reserved for
the SCI7 SPI DMA fast path; ch2-7 are unclaimed margin for future
CM33-side DMA consumers. This is recorded so a future CM33 DMA user
knows ch0/ch1 are spoken for and picks ch2-7, and so nobody reads the
whole-unit disable as accidental generosity that a partial Linux
re-enable could safely claw back.

`&dmac0 { status = "disabled"; }` in the Linux dtsi **is** the
enforcement mechanism for this decision, not a placeholder for a
future finer-grained one.

## Alternatives

**A devicetree channel-mask property letting Linux's `rz-dmac` bind
only channels 2-7.** Rejected: not fabricated here for lack of
evidence (see Decision §1). If the upstream `rz-dmac` binding is later
found to support this safely (a real property name, a real driver
version that honors it, silicon-verified not to still claim all 8
IRQs), this ADR is the one to revisit and supersede — not a reason to
guess the property now.

**A second, Linux-owned DMAC unit is never needed.** True today (no
current A55 DMA consumer is blocked), recorded as the reason the
whole-unit-disable cost is acceptable, not as a permanent claim — if
that changes, the fix is to point the new Linux DMA consumer at one of
the other four DMAC units, not to reopen DMAC0.

## Consequences

Good:
- The ownership split is explicit and citable in one place instead of
  requiring a fresh code dig (FSP config struct here, dtsi comment
  there) each time the question comes up.
- The channel-level detail (ch0/ch1 CM33, ch2-7 unclaimed) is on
  record for whoever next touches CM33-side DMA.

Bad / costs — stated honestly:
- Linux loses all 80 SoC-declared DMA channels across every DMAC unit
  it could otherwise reach through this specific controller instance
  (`metadata/socs/renesas/rzv2n/n44.json` `peripherals.dmac_channels:
  80`, SoC-wide across all DMAC units) — mitigated, not eliminated, by
  the other four units being available.
- The SCI7 DMA fast path itself remains blocked on the separate #84
  silicon issue; this ADR documents ownership, not throughput.

**Revisit triggers, stated explicitly:**
- A verified Linux-side per-channel DMA partition mechanism is found
  and silicon-proven not to claim ch0/ch1's IRQs.
- CM33-side DMA use grows past ch0/ch1 and needs the documented ch2-7
  margin (update the reservation here, not just in code comments).
- #84 resolves and the SCI7 DMAC path ships — re-verify the channel
  claim still matches `spi_renesas_rz_sci_b.c` at that point.

## See also

- `docs/gd32-link-sci7-next-rev.md` — the SCI7 DMAC hardening plan
  (item 1) this ADR's channel numbers are drawn from.
- `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi` —
  the `&dmac0` node this ADR's decision enforces.
- `zephyr/drivers/spi/spi_renesas_rz_sci_b.c` — the CM33-side FSP
  channel reservation.
- alp-sdk issue #84 — the separate, still-open silicon issue blocking
  the SCI7 DMAC data path itself.
