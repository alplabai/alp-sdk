# aen-sdcard-readout

Register-level bring-up probes for the Ensemble **E8 SD Host Controller**
(`snps,dwc-sdhc`) on the E1M-AEN801 (M55-HE).

## SD is disabled entirely on the E1M-EVK 2626-R2 (#2051)

The board's SDIO 74LVC157 mux (`U38`/`U39`) has **no high-impedance state**:
with its `/E` ENABLE input HIGH, the part's Y outputs are forced LOW, not
released — ON Semiconductor's datasheet gives no Hi-Z condition in the
function table for this part, unlike parts (e.g. 74LVC257) that add a
genuine output-enable. `U38`/`U39`'s Y outputs are the **SoC-facing** SDIO
nets (`E1M_CLK`, `E1M_CMD`, `E1M_D3..D0`, `E1M_SDIO_RST`), so those nets are
actively held low by the mux **whenever it is powered, regardless of
ENABLE**. Enabling `sdhc0` would apply SD pinctrl and let
`sdhc_dwc_init()` start the 400 kHz identification clock on `P14_1` at
`POST_KERNEL`, before this app's own `main()` ever runs — putting the SoC's
own 8 mA CLK/CMD drivers in a fight with the mux's held-low outputs on every
SD-routed pad.

The maintainer's decision, given that: keep `sdhc0` **disabled**
(`status = "disabled"`, the shared SoC dtsi's own default) on this board
revision, rather than merely leave the mux ENABLE undriven. This is a
hardware defect pending a component change, not a firmware workaround —
see `docs/boards/e1m-evk.md` and `include/alp/boards/alp_e1m_evk.h` for the
corrected part behavior (an earlier revision of both incorrectly described
`/E` HIGH as Hi-Z/isolating).

## What it shows

With `sdhc0` disabled, `main()` compiles out to a one-line `RESULT SKIPPED`
message and exits 0 — no SD pin is opened, configured, or driven; no
CC3501E bridge is brought up (nothing downstream needs it any more, since
the mux is never touched); no `disk_access_init()` is attempted.

The full register-probe logic stays in the source, guarded behind
`#if DT_NODE_HAS_STATUS(DT_NODELABEL(sdhc0), okay)`, so a future board whose
overlay re-enables `sdhc0` (a working mux, or a carrier with no mux at all)
gets the same controller-level bring-up test this app has always been for,
without a second app to maintain:

1. **PROBE 1** — a bare CMD0 (`GO_IDLE_STATE`) with no reset call in front
   of it: does the SD controller's card clock exist at all, independent of
   `SW_RST`? Touches no clock register.
2. **PROBE 2** — a READ-ONLY check that the SD peripheral clock gate
   (`CLKCTL_PER_MST` bit 16, `SDC_CKEN`) is set, then a real
   `sdhc_hw_reset()` + CMD0 exercise.

**Still read-only by construction** — no `CONFIG_FILE_SYSTEM`, no `fs_*`
call, no `disk_access_write`, no `mkfs` anywhere in this app; none of that
machinery is even linked in any more (see the CHANGELOG).

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-sdcard-readout
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

## The block

The E8 SD/SDIO controller is `sdhc@48102000` (`snps,dwc-sdhc`, IRQ 102/103),
declared once in the shared SoC dtsi
(`zephyr/dts/alif/ensemble_e8_peripherals.dtsi`) with
`clocks = <&clockctrl ALIF_SDC_CLK>`; this example's overlay only sets
`status = "disabled"` explicitly, so a reader does not have to go looking
for why no SD pinctrl or bus-limit property is set anywhere in this app.
Upstream Zephyr v4.4 and `hal_alif` ship **no** DesignWare SDHC driver, so
the controller driver is vendored **verbatim** from the Apache-2.0
`zephyr_alif` fork (`drivers/sdhc/sdhc_dwc.c` + `sdhc_dwc.h`) as an
**ADR 0017 Tier-2** copy.

## SD peripheral clock gate, resolved (#2035, #2051)

`CLKCTL_PER_MST` bit 16 is `SDC_CKEN`, a plain peripheral clock ENABLE
(Alif's own DFP header, `sys_ctrl_sd.h:30`, "Enable clock supply for
SDMMC", set with `|=` at `:40`) — not a source-select/mux, which an earlier
revision of this app's own comments (following a vendor Linux
`clk-ensemble.c` reading) used to claim. Bench evidence settled it: with
the bit clear, EVERY SDHC register — including read-only `CAPABILITIES1`
— read `0x00000000`, which a source-select could never produce.
`sdhc_dwc_init()` now sets this bit via the `clocks` property above before
main() ever runs — PROBE 2 is the read-only confirmation, on a board where
`sdhc0` is enabled.

**This does not close the original open question.** The all-zero readback
is a different symptom from this app's earlier diagnostics, recorded
before `SDC_CKEN` was identified, on a block that WAS clocked:
`CAPABILITIES1[5:0]=0x0a`, `SW_RST_R=0x02` (not `0x00`),
`NORMAL_INT_STAT_EN=0x7eff`/`ERROR_INT_STAT_EN=0xffff` are all values an
entirely unclocked block cannot give back. A `SW_RST_CMD`=1 reading on a
**clocked** block stays open; `SDC_CKEN` is a real, separate, now-fixed bug
found while investigating it, not its resolution. **None of this has been
run on real hardware as of this head** — see the BENCH-UNVERIFIED note in
`zephyr/include/zephyr/dt-bindings/clock/alif-ensemble-clocks-ext.h`.

Tier-2 retires onto the opt-in fork once a card is actually read on
hardware where that is possible.
