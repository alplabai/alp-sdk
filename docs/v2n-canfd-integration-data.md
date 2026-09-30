# RZ/V2N CAN-FD (CANFD2 / CANFD3) integration data

Status: reference facts for issue #1146.  The A55/Linux side is
brought up separately (`&canfd` channel2/channel3, function 5 on P84-P87);
this page records the integration facts the **CM33/Zephyr** side needs,
and states exactly what is still missing.  Source: Renesas RZ/V2N Group
User's Manual: Hardware, R01UH1071EJ0120 Rev.1.20 (sections named below).
Values are facts read from the manual, not a substitute for it.

## Why this is not in `metadata/socs/renesas/rzv2n/n44.json`

`peripheral_instances` in that file is **generated** by
`scripts/gen_soc_peripheral_instances.py` from Zephyr's
`r9a09g056.dtsi`, and its schema forbids hand edits.  That DTSI has no
CAN-FD node, so the generator cannot project one and a hand-added entry
would be rejected by `--check`.  Its `irq` field is a Zephyr IRQ number,
which for CAN-FD is only known after the SEL-slot allocation below.
The facts therefore live here until a DT node exists.

## Controller

| Item | Value | Manual |
|---|---|---|
| Instance | one CANFD module, six channels (0-5); the E1M-X SoM routes ch 2 and ch 3 (E1M_X_CAN0 = ch 3, E1M_X_CAN1 = ch 2, see #2524) | 7.9.1 |
| Register base, A55 physical | `0x12440000` | 7.9.2, Table 7.9-4 |
| Register base, CM33 non-secure | `0x52440000` | Table 7.9-4 |
| Register base, CM33 secure | `0x42440000` | Table 7.9-4 |
| Channel n registers | `CFDCnNCFG` at `0x0000 + 0x10*n`, then `CFDCnCTR`, `CFDCnSTS`, `CFDCnERFL` at +0x4/+0x8/+0xC | 7.9.2.1 |
| Global config | `CFDGCFG` (clock source select, TX priority, mirror mode) | 7.9.2.2.11 |
| CAN protocol clock `CANFD_0_clkc` | 80 MHz (supply: PLLCLN); matches the `can_clk` rate reported by Linux | 4.4, Table 4.4-2 |
| Bit rate limits | classical 1 Mbps; FD nominal max 1 Mbps, data max 8 Mbps | Table 7.9-1 |

The `canfd_iodefine.h` struct doc-comment address `0x400B0000` in
hal_renesas is wrong; use the values above (the real `R_CANFD_BASE` in the
same header is `0x42440000`).

## Pins (PFC)

Function 5 (`Func5`) in Table 1.2-3, same as the Renesas RZ/V2N EVK DT.

| Pad | Signal | PFC func |
|---|---|---|
| P84 | `CTX2` | 5 |
| P85 | `CRX2` | 5 |
| P86 | `CTX3` | 5 |
| P87 | `CRX3` | 5 |

Bench-confirmed on the Linux side (channel2/channel3 register and accept
`bitrate 500000 dbitrate 2000000 fd on`).  Sections 1.2 (Table 1.2-3) and
the PFC chapter give the register-level view.

## Interrupts

The CAN-FD sources are **not** fixed SPIs on the CM33.  They are "SELECT
(CM33)" events: software picks which event feeds each CM33 SPI through
`ICU_INTM33SELk` (k = 0..42, `<ICU_base> + 0x0200 + 4k`, three 10-bit
fields `M33SPIk_SEL0..2`; the field value is the event's `SELnnn` number;
`0x3FF` is the reset value, no source selected).  CM33 SPI number for field j of register k is
`353 + 3k + j`, so the 127 slots are SPI 353..479 (register 42 has only
field 0).  Source: 4.6.1.4, Table 4.6-19 and the `ICU_INTM33SELk`
register description.

Event numbers (Table 4.6-23, "List of Input Events", CANFD rows):

| Event | `SEL` number | CA55 SPI (fixed) |
|---|---|---|
| `can_cherr_int_0..5` (channel error) | 354 + n | 697 + n |
| `can_comfrx_int_0..5` (common RX FIFO / TXQ) | 360 + n | 703 + n |
| `can_glerr_int` (global error) | 366 | 709 |
| `can_rxf_int` (RX FIFO) | 367 | 710 |
| `can_tx_int_0..5` (channel TX) | 368 + n | 711 + n |

For channels 2 and 3 that is `SEL356/357` (error), `SEL362/363`
(common RX / TXQ), `SEL370/371` (TX), plus the two global lines
`SEL366` and `SEL367`: eight events, i.e. eight of the 127 CM33 slots.
The 20 numbers agree with the `IRQSELn_Type` range 354..373 in
hal_renesas `bsp_irq_id.h`.  The rows were read from a text extraction of
the manual's table; confirm the cherr/comfrx order against the PDF before
committing DT `interrupts` cells.

Which CM33 slots CAN-FD claims is still a project decision (see below):
`adc0` (403) and `gpt0` (406..408) already sit in this range in
`r9a09g056.dtsi`.  There are also 14 DMA-request events
(`can_rf_dmareq_0..7`, `can_cf_dmareq_0..5`) that need no SPI slot.

## Clocks, reset, bus

| Item | Value | Manual |
|---|---|---|
| Clock gates | `CPG_CLKON_9` (offset `0x0624`), ON bits 12 `CGC_CANFD_0_pclk`, 13 `CGC_CANFD_0_clk_ram`, 14 `CGC_CANFD_0_clkc`; write-enable bits are 28/29/30 (`CPG_CLKON_m` has ON in 15:0, write-enable in 31:16); monitor `CPG_CLKMON_4` (`0x0810`) bits 28/29/30 | 4.4.4.8, Table 4.4-15, Table 4.4-20 |
| Resets | `CPG_RST_10` (offset `0x0928`): bit 1 `CANFD_0_RSTP_N` (pclk domain), bit 2 `CANFD_0_RSTC_N` (clkc domain) | Table 4.4-24 |
| Bus stop | `CPG_BUS_10_MSTOP` (offset `0x0D24`), `MSTOP14_ON` (reset value has it set, i.e. stopped) | Table 4.4-38 |
| Unit clock supply | shared by all six channels; there is no per-channel gate | 4.4 |

CAN-FD is off after reset: `CPG_CLKON_9` initial `0x00000000`, `CPG_RST_10` initial `0x00000060` (bits 1/2 = 0, held in reset), `CPG_BUS_10_MSTOP` initial `0x0000DDEF` (bit 14 set, bus stopped).

## What is still missing on the CM33 side

1. **No FSP source.**  hal_renesas has no `fsp/src/rzv/r_canfd/`; only
   `canfd_iodefine.h` / `canfd_iobitmask.h` exist for RZ/V.  Upstream
   `CAN_RENESAS_RZ_CANFD` selects `USE_RZ_FSP_CANFD`, which compiles
   that missing path, so the driver fails at build before any DT is
   read.  `r_canfd` exists only under `fsp/src/rzg` and `drivers/ra`.
2. **No DT node.**  `r9a09g056.dtsi` has no CAN-FD node; only RZ/G3S
   (`r9a08g045.dtsi`) instantiates the binding, and its numbers
   (`canfd-global@400c0000`, IRQ 373/374, 375..380) are for the wrong part.
3. **No SEL-slot allocation** for the eight events above.
4. **Core ownership.**  CANFD2/3 must belong to one core per image; the
   A55 already claims P84-P87 (`metadata/e1m_modules/v2n/core-ownership.yaml`).

Register base, events, clock, reset, bus stop and PFC data are all
known from the manual; what remains is code (item 1) and two decisions
(items 3 and 4).
