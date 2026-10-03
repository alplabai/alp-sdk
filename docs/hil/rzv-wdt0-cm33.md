# RZ/V2N CM33 WDT0 -- bench steps (BENCH-UNVERIFIED)

Everything below is a plan, not a result.  The driver
(`zephyr/drivers/watchdog/wdt_renesas_rzv.c`, compatible `renesas,rzv-wdt`) and the
vendored FSP module (`zephyr/drivers/watchdog/r_wdt/`) only have a build proof
(`alp_sdk.wdt.rzv_cm33`, both CM33 boards, node enabled).  No step here has been run on silicon.

Target: an E1M-X V2N / V2M SoM on the bench (Cortex-M33 `m33_sm` core).  An expiry of WDT0 resets
the **whole SoM** (CPG_ERRORRST_SEL2), not just the M33.  Use the bench flashing flow in the
`flashing-and-bench-debugging-v2n` skill; bench boards are R&D units.

## Facts the steps verify (each is an assumption today)

| Assumption | Where it comes from |
|---|---|
| WDT0 base `0x41C00400`, registers `WDT0_WDTRR/WDTCR/WDTSR/WDTRCR/WDTCSTPR` | hal_renesas `R9A09G056N/iodefines/wdt_iodefine.h` |
| WDT0 counts at 24 MHz (`clock-freq` in the node) | inferred: the A55-side WDT1's fixed ~175 ms period = 16384 counts at /256 |
| The CM33 may write `CPG_ERRORRST_SEL2` and release the WDT0 clock/reset/bus-stop | `R_BSP_WDT_SYSTEM_RESET_ENABLE`, `R_BSP_MODULE_START(FSP_IP_WDT, 0)` in hal_renesas rzv2n BSP; the A55-side WDT1 routing is programmed by TF-A BL2 |
| No WDT0 NVIC line is reachable from the CM33 | hal_renesas `bsp_irq_id.h` has none (only ELC event `ELC_EVENT_IWDT_ELCWUN_CM33` = 142) |

## Ordered steps

1. Build `tests/zephyr/wdt_rzv_cm33` for `alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33` (then the
   `v2m101` board).  Flash the CM33 image per the V2N bench skill.  Keep the A55 console open: the
   CM33 has no console of its own.
2. Boot with the node enabled but the app **feeding every 500 ms** (the shipped test `main.c`).  Let
   Linux run past the late-boot unused-clock sweep (>= 60 s).  Expected: the SoM stays up.  If the
   CM33 dies with a bus fault around that time, the WDT0 module clocks / bus-stop were gated by
   Linux: the CM33-owned clock hold below is required before this step can pass.
3. Read back the armed configuration from the M33 (SWD / RAM console): `WDT0_WDTCR` should be
   `TOPS=2 (8192 counts)`, `CKS=0x8 (/8192)`, window `RPES=3 / RPSS=3`; `WDT0_WDTRCR.RSTIRQS=1`;
   `CPG_ERRORRST_SEL2` bit 0 set.  A hung write here means the CM33 cannot reach that register.
4. Starve it: build the app without the `alp_wdt_feed()` call.  Expected: the **whole SoM** resets
   (A55 console shows the reset / U-Boot banner), not just the M33.  Note which of the two happened.
5. Measure the period: with `timeout_ms = 3000` the driver picks the longest period not above it,
   `8192 x 8192 / 24 MHz = 2.796 s`.  Time last-feed to reset (scope on a CM33-driven GPIO released at
   the last feed, against the reset edge).  The real clock is `8192 x 8192 / measured_seconds`.  If it
   is not 24 MHz, fix `clock-freq` in `scripts/gen_zephyr_board.py` (`_v2n_dts`), regenerate the two
   boards, and record the measured value here.
6. Negative cases through `<alp/wdt.h>` on the same image: `ALP_WDT_INTERRUPT_ONLY`,
   `ALP_WDT_RESET_CPU`, `window_min_ms != 0` and `ALP_WDT_PAUSE_HALTED_BY_DEBUG` must each return
   `ALP_ERR_NOSUPPORT`; `ALP_WDT_PAUSE_IN_SLEEP` should arm (WDTCSTPR.SLCSTP=1).
7. Only after steps 2-5 pass: flip the `test-plan.md` row for this feature from `⏳` and regenerate
   `docs/verification-status.md`.

## Clock hold (not wired yet)

While WDT0 is enabled, Linux must keep its module clocks and bus-stop gates open, exactly as it does
for RSCI7.  The mechanism is `renesas,cm33-owned-clocks` on the CPG node, generated from the SoM
ownership metadata by `scripts/gen_linux_ownership_dt.py` on branch `feat/v2n-assignable-ownership`
(#2660) -- not on `dev` yet.  When it lands, add the WDT0 clocks to that list (hal_renesas names the
two gates `CPG_CLKON_4` CLK11 and CLK12 for channel 0, `FSP_IP_WDT_CLKP` / `FSP_IP_WDT_LOCO`; the Linux
registered names, likely `wdt_0_clkp` / `wdt_0_clk_loco`, are not verified).  Until then WDT0 stays
disabled by default and step 2 is the check that decides whether it is needed.
