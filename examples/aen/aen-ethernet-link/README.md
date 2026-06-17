# aen-ethernet-link

Bring up the Ensemble **E8 GMAC** (the upstream `eth_dwmac` core + the alp-sdk
`alif,ethernet` glue) on the E1M-AEN801 (M55-HE) and report the network-interface
state, using the Zephyr **net-if API**.

## The block

`ethernet@48100000` (eth_dwmac core, IRQ 148, clock `ALIF_ETHERNET_CLK`, RMII,
fixed-link PHY) already lives in `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` as
`status="disabled"` — the MAC driver and Tier-1.5 glue are committed. This example
just **enables** the node (board overlay) with the RMII pin group + a MAC address,
and exercises the interface. No new `zephyr/` code.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-ethernet-link
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

The board overlay enables `&ethernet` with the **authoritative SoM RMII route**
(`metadata/e1m_modules/aen/alif-ethernet-phy.tsv`, not the fork reference route the
first cut wrongly used): REFCLK=P11_0, RXD0=P11_3, RXD1=P1_1, CRS_DV=P6_7,
TXD0=P10_4, TXD1=P10_5, TXEN=P1_5 — with `input-enable` on the RX inputs. It also
creates `gpio11` + `lpgpio` so main() can drive the PHY control pins.

## What it shows

1. Powers the PHY **early** (SYS_INIT pri 50, before the eth driver's refclk probe):
   `E_PHY_PWRDWN`=P15_4 (lpgpio.4) + `E_PHY_RESET`=P11_6 (gpio11.6).
2. Reads back `ETH_CTRL` bit 4 (the RMII ref-clock source the AUTO probe locked).
3. `net_if_up()` + `net_dhcpv4_start()` → DHCP lease test off the bench switch.

## Status

**Headline blocker RESOLVED + bench-validated:** the "Ethernet needs a power
enable" the bench called out **is** the Alif `E_PHY_PWRDWN` = **P15_4** line (an
**lpgpio** pin — *not* a CC3501E line as first assumed; confirmed against the SoM
TSV). Driving it **before** the eth driver's RMII ref-clock AUTO probe flips the
probe result from internal-PLL fallback to the **external 50 MHz oscillator**
(`ETH_CTRL` bit 4: `1 → 0`, self-reported by the app + SWD-confirmed) — the PHY is
now clocked and the MAC DMA-reset completes on the external osc.

Also fixed here: the RMII pins were wrong (fork route) → corrected to the SoM TSV
route; `input-enable` added to the RX input pads (the upstream pad REN bit, via the
standard property — the fork's `read-enable` does not exist upstream).

The example now reads the PHY over **raw MDIO** (DWMAC `MAC_MDIO` regs) to diagnose
the link directly. Bench result (cable connected to the switch):

```
[eth] MDIO PHY@0 id=2000a140 (DP83825I=2000a140)   <- PHY powered + MDIO alive
[eth] PHY regs: ANAR=01e1 ANLPAR=0000 PHYSTS=4002 RCSR=0061
[eth] PHY link DOWN after 8s (BMSR=7849)
```

**RESULT PARTIAL — everything on the SoC/PHY side works, but the PHY never forms a
media-side link.** Verified working: MAC + driver + PHY power (P15_4) + MDIO (PHY ID
`0x2000a140` @ addr 0) + RMII config + both `RCSR` clock modes + long reset timing.
The decisive symptom is **`ANLPAR=0x0000`** — the PHY receives **no** auto-negotiation
from the switch (no link-partner ability), even with the cable connected. Auto-neg
never completes and the link stays down.

Further isolation (with the cable connected):
- **PHY→RJ45 wiring CONFIRMED CORRECT** (from the schematics): PHY `TD±`→`ETH0_DA±`
  →`TRD1±` (transmit pair) and `RD±`→`ETH0_DB±`→`TRD2±` (receive pair), polarity
  preserved — so it is **not** a wiring swap.
- **MAC TX works** — internal-loopback run transmitted 291 bytes (`stats.bytes.sent`
  rose). The MAC→RMII transmit side is good.
- **RX side is the fault** — both the media RX (`ANLPAR=0`, no auto-neg from the
  switch) and the PHY internal loopback failed to deliver received frames.

> **Prime suspect: the PHY's RECEIVE data path / 50 MHz reference clock.** MDIO works
> (it's MDC-clocked, independent), and the MAC transmits — but the PHY's PCS (which
> needs the reference clock) isn't recovering RX in *either* the media path *or*
> internal loopback. Scope the **PHY `REF_CLK` pin for a clean 50 MHz** (and the
> PHY's crystal if it's the RMII master), plus the RX-side RMII pins (`RXD0`=P11_3,
> `RXD1`=P1_1, `CRS_DV`=P6_7) and the switch port LED. The media wiring + the magnetics
> RX winding (`TRD2`→RJ45 pins 3/6) are the other things to confirm on the EVK
> schematic — this analog path is NOT in the alp-sdk metadata (only the SoC↔PHY
> digital pins are).

See [[project_pending_hw_configs]]. Run on the bench switch (dnsmasq,
192.168.10.50–150); a `[eth] DHCP lease = …` line is the full-link PASS.
