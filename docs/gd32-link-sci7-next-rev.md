# GD32 supervisor link — SCI7 hardening plan (next firmware rev)

Status: **SCI7 Simple-SPI is the permanent transport for the GD32 link.**
A reroute to a native RSPI controller (hardware SSL, wider frames) was
evaluated 2026-06-04 and **rejected**: the candidate RSPI pads (P90–P93)
are committed to other functions on the SoM, and no other RSPI channel
reaches the GD32 without disturbing shipped routing.  The link therefore
stays on SCI7 (P76/P77/P96/P97 ↔ GD32 SPI1), and this document tracks the
remaining hardening work on that basis.

## Where the link stands (2026-06-04, firmware v0.2.1)

Silicon-validated state after the FIFO/pacing rework (see
`zephyr/drivers/spi/spi_renesas_rz_sci_b.c` and `CHANGELOG.md`):

- 32-deep FIFO burst engine, credit-bound (31 in flight), zero per-byte
  flag polls; wire runs dense at 25 MHz (scope-confirmed).
- CS windows 3/2 µs on a raw SysTick spin; single CS owner (the
  direct-latch shim; consumers pass `ALP_SPI_NO_CS`).
- Reply re-read made real: the slave rewinds + re-arms its staged reply
  on every drain/empty transaction; the host reads after an opcode-aware
  staging gap and retries down a 25 µs → 1.6 ms ladder.
- Measured (256-op averages, zero errors): ping 120 µs, get_version
  133 µs, gpio_read 153 µs, adc_read(4) 196 µs, adc_read(8) 261 µs;
  raw transaction floor 27 µs; ~0.85 µs/byte marginal.

## Next-rev items (priority order)

1. **SCI7 DMAC data path** — the true wire-rate, zero-CPU host path.
   Benched 2026-06-04 with the **vendor ground truth** config (e2-studio
   FSP generator, SCI7 @ 25 MHz: `MASK_DACK_OUTPUT`, `NO_DETECTION`,
   request direction `SOURCE_MODULE` both ways, DREQ/DACK/TEND pins
   explicitly unused — the 2026-06-03 sweep had REQD inverted on RX and
   the external-pin fields zero-initialised to PIN0).  Result: the DMAC
   **streams** — complete request+reply transactions ran end-to-end
   through the FSP machinery (channel-end IRQs, TEND completion, intact
   frames at the GD32), killing the old "parks SUS after one beat"
   diagnosis.  Two FSP-layer bugs fixed on the way: the rzv
   `R_DMAC_B_Reset` stub broke every RX arm (now `reconfigure()`-based;
   documented RZ-port deviation in `r_sci_b_spi.c`), and the
   `transceive()` FSP branch + `SCI_B_SPI_CFG_DMA_SUPPORT_ENABLE` now
   follow the `ALP_V2N_SCI7_DMAC` gate.  **Remaining blocker**: after
   initial success the TX channel intermittently parks `CHSTAT.ER`
   (AXI bus error) one beat into a TDR write through the `0x128xxxxx`
   convert window while RX reads through the same window keep
   completing — a bus-master security/Master-MPU attribution lead
   (BL2-programmed SYS master-access controls), not channel config.
   Next bench session: dump/compare the SYS master-access-control
   registers, then re-bench; rewrite the Renesas ticket around the ER
   evidence if the security angle dead-ends.
2. **Sequence echo in the reply STATUS byte** (wire-protocol MINOR).
   Kills the residual stale-reply hazard: if a request is ever lost
   whole to CS-edge coalescing, the slave's re-armed previous reply is
   CRC-valid stale data for a same-opcode re-read (documented in
   `firmware/gd32-bridge/src/transport_spi.c`).  A 4-bit sequence in the
   STATUS high nibble, echoed from the request, makes staleness
   detectable.
3. **GD32 ADC sampling-time config** — conversions cost ~18-20 µs per
   sample inside the request's CS-rising handler (bench-bracketed),
   which is the dominant reply-staging latency for ADC opcodes.  The
   host's staging gap deliberately under-covers it (measured optimum);
   the real fix is slave-side sampling-time/clock tuning.
4. **GD32 rising-path slim-down** — the per-transaction RCU-reset flush
   + reconfigure is the slave-side floor (~tens of µs with decode).  A
   targeted FIFO flush (if a reliable one exists on this errata class)
   or a leaner reconfigure would cut every command's floor directly.

## Carry-over facts

- Kernel AMP gating: the `DEF_MOD_CRITICAL` patch already holds the
  DMAC0 clocks; no kernel change needed for item 1.
- GD32 firmware is master-agnostic on the data path; items 3-4 are
  firmware-internal and wire-compatible.
- Port-9 AMP ownership rules stand: keep the carrier DT free of port-9
  claims (the SD1_CD lesson).

## Acceptance criteria (unchanged bar)

- Cold power-cycle → link autonomous within seconds, zero intervention.
- Sustained soak: full command set at the HiL soak cadence, zero
  STATUS_IO on the active surfaces, ≥15 min.
- Any DMAC enablement ships only after the soak + OTA e2e re-validate
  on silicon at the new data path.
