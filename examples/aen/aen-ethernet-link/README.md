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

**RESULT PARTIAL** (no DHCP lease yet): with the PHY clocked + powered + pins
corrected, a full bidirectional link is not yet up. Remaining (needs a scope or the
schematic, not inventable):
> - **PHY enable/reset polarity** — `PWRDWN`=high (enable) made the osc run; both
>   `RST_N` polarities were bench-tried without a link, so the gating detail is
>   elsewhere.
> - **TX path / auto-negotiation** — DHCP DISCOVER must transmit and the DP83825I
>   must auto-neg with the switch; a managed-PHY (MDIO) driver would let us read PHY
>   link status directly.
> - **Exact EVK PHY wiring** vs the SoM TSV.

See [[project_pending_hw_configs]]. Run on the bench switch (dnsmasq,
192.168.10.50–150); a `[eth] DHCP lease = …` line is the full-link PASS.
