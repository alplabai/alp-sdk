# E1M-X V2N module pinout

Pin-to-function mapping for the E1M-X V2N family of SoMs
(`E1M-V2N101`, `E1M-V2N102`, `E1M-V2N103` -- Renesas RZ/V2N-based
modules without the DEEPX DX-M1 NPU).

## Files

| File                              | Schema                                     |
|-----------------------------------|--------------------------------------------|
| `renesas-peripheral-map.tsv`      | `peripheral \t renesas_pad`                |
| `renesas-peripheral-map.csv`      | `row, peripheral, renesas_pad`             |
| `gd32-io-mcu-map.tsv`             | `peripheral \t gd32_pad`                   |
| `gd32-io-mcu-map.csv`             | `row, peripheral, gd32_pad`                |
| `hw-revisions.yaml`               | Per-rev SDK-version compatibility window   |
| `core-ownership.yaml`             | `core_ownership:` FIXED `(peripheral, pad) -> core` facts, cited; `assignable:` per-product choices by E1M instance; `a55_only_resources` for blocks with no TSV row |
| `supervisor-links.yaml`           | `supervisor-links-v1`                      |

Not in `gd32-io-mcu-map.*` (they are not bridge-controllable GPIOs): E1M IO26 and
E1M IO15 are physically routed to GD32 PC2 and PB4, but the bridge firmware has no
bitmap bit for them yet, so the SDK does not expose them through the bridge. E1M IO24
is not a GD32 pad at all (driven by the DX-M1 on V2M, undriven on V2N).

## Two MCUs on the module

The V2N module's E1M-edge peripherals split across two silicon
sources:

- **Renesas RZ/V2N** (the application SoC) -- owns I²C, SPI, UART,
  I²S, classic RGMII Ethernet, CAN, SD/eMMC, xSPI NOR, PDM, and the
  DRP-AI3 accelerator.  Renesas drives **none** of the eight E1M
  PWM channels directly; all PWMs are GD32-driven.
- **GigaDevice GD32G553MEY7TR** (companion IO MCU) -- owns **all
  eight E1M PWM channels**, the encoder-input bank, the dual
  ADC + DAC bank, the camera-LDO enables, the Murata Wi-Fi/BT
  module's REG_ON pins, and the OPTIGA reset.  Reached from the
  Renesas side via the **GD32 bridge** -- see
  [`../../../docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md).

The two MCUs share a board-management I²C bus (BRD_I2C) plus a
dedicated SPI link.

Renesas RZ/V2N is itself AMP (Cortex-A55 running Linux, Cortex-M33
running Zephyr).  `core-ownership.yaml` attributes a small,
silicon-verified subset of Renesas-owned pads to whichever core
actually drives them; see
[issue #1157](https://github.com/alplabai/alp-sdk/issues/1157).

**Fixed vs assignable.**  `core_ownership:` holds only FIXED facts (the
GD32 SPI link is always the CM33, RIIC8/BRD_I2C always the A55, ...).  A
resource whose owner is a per-product choice is never a fixed row: it sits
under `assignable:`, keyed by E1M instance (`e1m_uart0`, `e1m_uart1`,
`e1m_spi0`, `e1m_can0`, `e1m_can1`), with a `default` (Linux/`a55`),
`candidates` (only cores that have a real backend today) and `rows` that
reference existing `(peripheral, pad)` pairs of `metadata/pinmux/v2n.yaml`.
A project may name an instance in `board.yaml`:

```yaml
ownership:
  e1m_uart0: a55       # core token: a55 | m33 (board.yaml core id m33_sm)
```

The loader rejects an unknown instance, a core outside `candidates`, a
`hw_blocked` instance, and a core that differs from the SoM default unless the
instance lists `m33` in `candidates` AND carries an `m33:` block (handing a
node to `a55` also needs `linux_enable`): both trees must be able to follow.
The Linux side of a per-project override is `alp_project.py --emit
linux-ownership-dts`; the kernel bbappend renders the same fragment from the
system-manifest `ownership:` (`ALP_SYSTEM_MANIFEST_PATH`).  The
resolved map is emitted as
`ownership:` in `--emit system-manifest`.  Caveats: `e1m_uart0` stays
`a55`-only until the P51 (UART0_RXD0) RX pull-up is bench-proven (a floating
RXD triggers the sci0 receive-error ISR on the CM33); `e1m_uart1` has no CM33
sci1 node; both CAN-FD instances are `a55`-only (no `r_canfd` in hal_renesas
rzv); `e1m_spi0` pads P90-P92 are not 3.3 V tolerant -- enable no rspi0 node
on either core until the carrier parts are confirmed safe.

An override naming a core the project does not declare under `cores:` is
rejected too.  Each entry names its `soc_instance`, the key of the SoC JSON
`linux_dt` map (`metadata/socs/renesas/rzv2n/n44.json`) that supplies the
Linux node `label`, the PFC function codes (`pinmux`, by row peripheral name;
the pad's port/pin is parsed from the row) and the CPG clock names
(`cpg_clocks`).  A function code absent there is never guessed: the generators
report a gap.  `hw_blocked: {reason}` keeps an instance disabled on EVERY core
(`e1m_spi0` today: P90-P92 are not 3.3 V tolerant).

Zephyr side: an entry may carry an `m33:` block (`dt_label`, `alias`,
`kconfig`, `pinctrl` names).  `gen_zephyr_board.py` then emits that node
`disabled` with the pinctrl group built from the rows + `linux_dt` codes, and
only a project whose resolved owner is `m33` gets it `okay` + the alias
+ the `kconfig` lines from `--emit dts-overlay` / `zephyr-conf`.  No entry
carries an `m33:` block yet: the PFC codes for the RSPI0 and CAN-FD pads and
the SPI_B CM33 interrupt routing are not in metadata, so an `m33` assignment
of an instance without one fails at emit with a message saying so.

Linux side: `scripts/gen_linux_ownership_dt.py` projects the SoM defaults into
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-ownership.dtsi`
(generated, included from `e1m-v2n-som.dtsi`, installed by the linux-renesas
bbappend).  An a55-owned instance gets `status =
"okay"` and a pinctrl group only if its entry has `linux_enable: true` (which
requires `linux_evidence`) and the SoC `linux_dt` has every PFC code; the
default is false, so the shipped image changes no behaviour (an alias-less
rz-rsci port would register as ttySC0 and steal the console -- bench
2026-09-26).  No instance is enabled today: each is a "left at the vendor
status" comment, `e1m_spi0` quotes its `hw_blocked` reason.  The fragment
follows the SoM default.  `--vendor-dtsi
<r9a09g056.dtsi>` verifies the referenced node labels exist in the kernel
tree.

CM33-owned clocks: the same fragment sets `renesas,cm33-owned-clocks` on the
CPG node -- the `linux_dt[...].cpg_clocks` of every enabled supervisor link
(RSCI7 for the GD32, always) and of each assignable instance whose SoM default
owner is the M33 (never a `hw_blocked` one).  The kernel patch
`0001-clk-renesas-rzv2h-cpg-cm33-owned-clocks.patch` makes the CPG driver keep
those clocks and their MSTOP gates on, so `clk_disable_unused` cannot stop a
peripheral the CM33 is using.  The generic `protected-clocks` property is not
honoured by rzv2h-cpg (and is index-based), hence the named vendor property.

The GD32 link (SCI7) stays enabled
on the board, not per project: moving it to the overlay would break every
plain `west build` that does not run the emitter.

## V2N-M1 vs V2N base

`E1M-V2M101` / `E1M-V2M102` / `E1M-V2M103` (the V2N-M1 family) reuses this base
map plus a small Renesas-side overlay for the DEEPX-specific
signals (`M1_RESET`, `PCIe.MUX_PD`, `PCIe.MUX_SEL`).  See
[`../v2n-m1/`](../v2n-m1/) for the overlay.
