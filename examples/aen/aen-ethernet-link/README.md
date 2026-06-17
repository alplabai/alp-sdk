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

The board overlay enables `&ethernet`, supplies `pinctrl_eth` (the RMII "B" route:
REFCLK=P11_0, RXD0/1=P11_3/4, CRS_DV=P11_5, TXD0=P6_0, TXD1=P10_5, TXEN=P10_6),
and sets a `local-mac-address`.

## What it shows

1. `net_if_get_default()` → the GMAC interface + its MAC.
2. `net_if_up()` → start the MAC DMA (the eth_dwmac core's software reset runs).
3. Sample `admin_up` + `carrier_ok`.

## Status

**GMAC + interface PROVEN on E8 (RESULT PARTIAL):** the interface registers, the
MAC software-reset completes (so the on-module 50 MHz RMII ref-clock on P11_0 **is**
present — the driver does not hang), and `net_if_up` returns `-EALREADY` (the
fixed-link PHY brought it up at init).

> **`carrier_ok=1` is NOT a real link.** The node uses `ethernet-phy-fixed-link`,
> which reports carrier unconditionally. Real connectivity to the bench switch
> (TX/RX, a ping) still pends — none are driver bugs:
> 1. **PHY power-enable:** the on-module TI DP83825I needs a power-enable on a
>    **CC3501E**-side line (driven over the SPI bridge). Unpowered = no wire link.
> 2. **RMII ref-clock source-select:** `ETH_CTRL_REG`'s ref-clk-source bit has no
>    upstream clock-driver programming path (do NOT invent it).
> 3. **Pad read-enable:** the RMII *input* pads want `read-enable`, absent from the
>    upstream `alif,pinctrl` binding (the systemic gap shared with I2S/PDM/qenc).
> 4. A managed-PHY (MDIO) driver + a ping test would escalate this beyond PARTIAL.

See [[project_pending_hw_configs]]. The earlier "Ethernet PASS" was this same
driver/interface-init level; this example makes the fixed-link caveat explicit.
