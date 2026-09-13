# aen-sdcard-readout

Bring up the Ensemble **E8 SD Host Controller** on the E1M-AEN801 (M55-HE) through
the vendored `snps,dwc-sdhc` driver + the Zephyr SDMMC disk, and probe a microSD
card via the standard **disk-access API** (`disk_access_init` /
`disk_access_ioctl`).

## The block

The E8 SD/SDIO controller is `sdhc@48102000` (`snps,dwc-sdhc`, IRQ 102/103).
Upstream Zephyr v4.4 and `hal_alif` ship **no** DesignWare SDHC driver, so the
controller driver is vendored **verbatim** from the Apache-2.0 `zephyr_alif` fork
(`drivers/sdhc/sdhc_dwc.c` + `sdhc_dwc.h`) as an **ADR 0017 Tier-2** copy. Data is
moved by the card's **internal ADMA2/SDMA** engine (no Alif system-DMA
dependency). The optional CPU↔bus DMA address translation — which would pull in
the `hal_alif` `soc_memory_map.h` and the `itcm/dtcm` `global_base` props absent
from our dtsi (the same gap the NPU work hit) — is left **off**, so the driver
uses identity addressing.

One porting patch vs the fork: `bus_4_bit_support` moved from
`sdhc_host_caps` to `sdhc_host_props` between the fork's `sdhc.h` and upstream
v4.4 — the driver now sets `props->bus_4_bit_support`.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-sdcard-readout
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

## What it shows

1. `cc3501e_bridge_bringup()` → power up the on-module CC3501E coprocessor and
   bring the inter-chip SPI1 bridge up (~900 ms), then a meta handshake
   (VERSION/MAC/CAPABILITIES) and GPIO proxy attach — proves the bridge link
   itself, independent of the SD card path below.
2. Three controller-level probes, none of which touch an SD pin: a bare CMD0
   (does the card clock exist at all), a read-only check that the SD
   peripheral clock gate (`CLKCTL_PER_MST` bit 16, `SDC_CKEN`) is set plus a
   real `sdhc_hw_reset()` + CMD0, and an SE transport sanity check.
3. **SAFETY (#2051): does NOT assert the SDIO mux ENABLE, does NOT drive
   `SD_RST`, and does NOT attempt `disk_access_init()`.** See Status below —
   this board's mux hardware makes all three unsafe or pointless.

**Still read-only by construction** — no `CONFIG_FILE_SYSTEM`, no `fs_*`
call, no `disk_access_write`, no `mkfs` anywhere in this app.

This app prints `RESULT SKIPPED` and exits 0: not a failure, the expected
outcome on this board revision (see Status).

## Status

> **SAFETY (#2051) — this app no longer drives the SD mux ENABLE or
> `SD_RST` on the E1M-EVK 2626-R2, and does not attempt
> `disk_access_init()`.** The board's netlist
> (`metadata/boards/e1m-evk/netlists/E1M-EVK-2626-R2_pinmap.csv`, `U38`/`U39`)
> shows every 74LVC157 mux OUTPUT drives a SoC-facing net: `E1M_D3/D2/D1/D0`,
> `E1M_SDIO_RST`, `E1M_CLK`, `E1M_CMD`. The maintainer confirmed (2026-09-13)
> this mux stage has a hardware defect and needs a component change. With
> `MUX_EN` asserted, a mux whose outputs are stuck driving fights the SoC's
> own drivers on those pads — SD CLK/CMD/DAT and `SD_RST` — which is driver
> contention on live pads, not a benign "no card" failure. So this app leaves
> `MUX_EN` disabled/undriven (its safe idle state) and stops before ever
> reaching the card path. Do not read `RESULT SKIPPED`, or the absence of a
> `-116`/`DISK_STATUS_NOMEDIA` reading, as evidence the card path works —
> it was never attempted.

`#2051` also found and fixed a real **driver** bug unrelated to the mux: the
SD peripheral clock gate (`SDC_CKEN`, `CLKCTL_PER_MST` bit 16) was never
enabled, so the whole SDHC block read back all-zero (see the CHANGELOG),
plus secondary reset-path hardening. **Neither has been run on real hardware
as of this head** — the fix is expected, not bench-confirmed, on this
specific commit; the evidence for the ORIGINAL bug (the all-zero readback)
was bench-confirmed on an earlier revision, which is a different claim.
`SDC_CKEN` explains that all-zero evk-03 readback only. It does **not**
explain, and does not close, this file's original open question: a
`SW_RST_CMD`=1 reading on a **clocked** block (`SW_RST_R` at `0x4810202F`
stayed `0x02`, alongside `CAPABILITIES1[5:0]=0x0a` and
`NORMAL_INT_STAT_EN=0x7eff`/`ERROR_INT_STAT_EN=0xffff` — values an unclocked
block cannot give back). That question is still open and is not this app's
job to fix; see `main()`'s own header comments for the full history.

> **The mux CONTROL lines — separate from the mux STAGE above.** ENABLE
> (E1M `IO20` → CC3501E `GPIO_26`) was, in an earlier revision of this app,
> proven driven by a write + read-back over the GPIO proxy — but per the
> SAFETY note above, this app no longer drives it at all. SELECT (E1M
> `IO21`) is not software-drivable on this module at all — on r2 it is
> physically open, on r1 driving it would contend with the P18 header
> jumper — so it is set by a jumper on header **P18**, and unlike ENABLE it
> has never been read back or otherwise verified by any app: "driven
> correctly" was never an accurate description of SELECT, only of ENABLE,
> and ENABLE is not driven any more either.

So on this bench: the SDHC **controller + driver build and init**, a real
clock-gate + reset-path driver bug is fixed (`#2051`, expected but not yet
bench-verified on this head), and the SD **card path is intentionally not
attempted at all** on a 2626-R2 EVK — the mux **stage** (not its control
lines) is hardware-broken and needs a component change, per the maintainer,
and no firmware change can work around driver contention on a physically
broken mux. Tier-2 retires onto the opt-in fork once a card is actually
read on hardware where that is possible.
