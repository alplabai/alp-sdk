# GD32 supervisor link — RSPI migration package (next SoM revision)

Status: **engineering package for the next E1M-V2N SoM spin.** The current
revision SHIPS on the SCI7 link at 25 MHz (validated; see
`zephyr/drivers/spi/spi_renesas_rz_sci_b.c` and the v0.2.0 GD32 firmware) —
this document is the plan to move the link to a **native RSPI controller**
on the next board revision.

## Why move (what the SCI7 bring-up taught us)

| Pain on SCI7 (current) | On RSPI |
|---|---|
| No hardware chip-select in master mode → software CS latch shim, gpio_rz PM9 drive-mode quirk, 60/30 µs software setup/hold windows, 3-owner CS hazard | **Hardware SSL0** with programmable RSPCK-delay (1–8 cycles) and SSL-negation delay — the whole CS stack disappears |
| 8-bit frames only (IP-fixed) | 8–32-bit frames (GD32 slave caps the link at 16-bit) — halves DMA/poll beats |
| FSP sci_b_spi ↔ DMAC-B pairing vendor-unvalidated (silicon-blocked; channel parks RQST\|SUS — see the post-mortem in `spi_renesas_rz_sci_b.c`) | Dedicated SPI0/1/2 RXI/TXI DMAC trigger events exist (`bsp_dmac.h` 140–145); rzv FSP ships `r_rspi` *and* `r_spi_b`; deep FIFOs as fallback |
| SCI eri quirks (TE/RE latch class, patched in the vendored module) | Purpose-built SPI IP |

**What it does NOT buy: speed.** RSPCK is rated 50 MHz, but the GD32G553
slave's datasheet ceiling is 27 MHz (Table 4-49) — the link stays ~25 MHz.

## Pin plan (HW manual PFC Table 1.2-3)

RSPI channel A sits on **port 9, immediately adjacent to the current link**
(P96/P97 today) — a minimal-delta reroute of the four GD32 traces on the SoM:

| Signal | Pad | PFC function | GD32 side (unchanged) |
|---|---|---|---|
| MOSIA  | **P90** | func1 | PB15 (SPI1_MOSI, AF5) |
| MISOA  | **P91** | func1 | PA10 (SPI1_MISO, AF5) |
| RSPCKA | **P92** | func1 | PA9  (SPI1_SCK, AF5)  |
| SSLA0  | **P93** | func1 | PA8  (SPI1_NSS, AF5, HW-NSS — pairs perfectly with a real master SSL) |

Notes:
- P94 (freed from the Linux SD1_CD claim in `8682ad3`) is **SSLA1** — keep it
  unclaimed; it is a natural expansion CS.
- Alternates if P9x is congested: RSPI-B (RSPCKB = PB0 group) or RSPI-C
  (RSPCKC = PB5 group).
- Port-9 AMP ownership rules from this bring-up carry over: keep the carrier
  DT free of port-9 claims (the SD1_CD lesson).

## Software plan (~1–2 days at bring-up)

1. **CM33 driver**: revive `zephyr/drivers/spi/spi_renesas_rz_spi_b.c`
   (in-tree, currently gated off) or write thin glue over the rzv FSP
   `r_rspi`; confirm which of `r_rspi` / `r_spi_b` serves the V2N RSPI
   instances (dtsi `spi1@42800400`) and use hardware SSL0 (drop every CS
   shim).  Wire the board dts: `&spi1 { pinctrl P90..P93; }`.
2. **DMA**: use the dedicated `DMAC_TRIGGER_EVENT_SPI1_RXI/TXI` events with
   the same DMAC-B instances built tonight (parked in the SCI driver behind
   `ALP_V2N_SCI7_DMAC`).  If the DMAC-B IP issue (channel parks SUS)
   reproduces on the SPI events too, fall back to the RSPI FIFOs +
   interrupt — still far better than the SCI's 1-deep registers — and use
   the Renesas ticket (post-mortem in `spi_renesas_rz_sci_b.c`).
3. **Kernel AMP gating**: extend the `DEF_MOD_CRITICAL` kernel patch
   (`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0001-clk-renesas-...`)
   to the chosen `rspi<n>` clocks (entries already exist in the cip43
   table, BUS_11 bits 0-2) — proven mechanism, 5-minute change.
4. **GD32 firmware**: zero changes — the v0.2.0 DMA slave is
   master-agnostic.  Optionally raise frames to 16-bit on both sides.
5. **Validation**: the bench flow from this bring-up applies verbatim
   (GD32 SWD burst-sampling, cold-cycle autonomy, PING + odd-length
   GET_VERSION soak).

## Acceptance criteria (same bar as the current rev)

- Cold power-cycle → link autonomous within seconds, zero intervention.
- Sustained soak: PING (4 B) + GET_VERSION (7 B, odd) alternating at the
  1-in-8 cadence, zero STATUS_IO, ≥15 min.
- CS framing fully hardware (no software CS code paths compiled in).
