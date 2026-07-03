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

**RESULT PASS — Ethernet works end-to-end on the E8 (bench-validated, both sides).**

```
[eth] RMII refclk source = EXTERNAL osc (PHY clocked -- power-enable worked) (ETH_CTRL bit4=0)
[eth] MDIO PHY@0 id=2000a140 (DP83825I=2000a140)
[eth] PHY link UP after 1750 ms (BMSR=786d ANLPAR=dde1)
[eth] admin_up=1 carrier_ok=1 wire_link=1 rx_bytes=1268 dhcp_bound=1
[eth] DHCP lease = 192.168.10.137
[eth] RESULT PASS: wire link UP + DHCP lease acquired = Ethernet fully WORKING
```
Confirmed on the **server** too: `dnsmasq` handed `192.168.10.137` to the SOM MAC
`02:01:56:78:43:21`, the SOM shows `REACHABLE` in the switch-port host's ARP table,
and the server NIC RX counter moved off 0 — the SOM is discoverable on the wire.

> Note: that bench run used the then-fixed DT `local-mac-address`. The SoC dtsi
> now defaults to `zephyr,random-mac-address` (per-boot random
> locally-administered `02:01:56:xx:xx:xx`, so two boards never collide), and
> this example inherits it — expect a different MAC in the dnsmasq lease each
> boot. Per-unit provisioning pins a fixed MAC by deleting the random flag in
> the overlay (see `boards/alp_e1m_aen801_m55_he.overlay`).

### Three independent things had to be right

1. **PHY power + clock — `E_PHY_PWRDWN` = P15_4 (lpgpio), `E_PHY_RESET` = P11_6
   (gpio11).** These are **Alif** pins (per the SoM TSV), *not* CC3501E lines as the
   first cut assumed. main() drives P15_4 high (enable) and pulses P11_6 (reset)
   **early** (SYS_INIT pri 50, before the eth driver's RMII ref-clock AUTO probe).
   Powering the PHY first makes the external 50 MHz appear, so the probe locks the
   **external oscillator** (`ETH_CTRL` bit 4: `1 → 0`) instead of the internal-PLL
   fallback. Self-reported by the app + SWD-confirmed.
2. **RMII pins + clock mode.** RMII pads = the authoritative SoM TSV route (not the
   fork reference route the first cut used), `input-enable` on the RX/REFCLK/MDIO
   pads (the upstream pad REN bit, via the standard property). The PHY's `RCSR`
   (0x17) bit 7 `REF_CLK_SEL` is set to **1** (50 MHz reference, DP83825 RMII mode):
   bench-confirmed bit7=1 forms a media link (BMSR bit2, `0x786d`) and bit7=0 does
   not (`0x7849`) — this board feeds the PHY an external 50 MHz, so the PHY is the
   RMII slave.
3. **DMA-buffer placement — the decisive fix.** The generated M55-HE board sets
   `zephyr,sram = &dtcm`, so the GMAC's descriptor rings *and* the net_buf packet
   pool landed in the M55 **DTCM** (CPU-local alias `0x20000000`). DTCM is
   tightly-coupled and **not on the GMAC DMA's AXI path** — so the DMA fetched
   garbage descriptors and wrote received frames nowhere. The MAC, MDIO, PHY power,
   clock and even the media link all came up, but **no frame moved in either
   direction** (`rx_bytes=0` on the SOM *and* `RX=0` on the server NIC). The overlay
   now sets **`zephyr,sram = &sram0`** (the global on-chip SRAM @ `0x02000000`, the
   same bank the NPU uses): globally addressed so CPU addr == DMA addr, which is what
   the upstream DWMAC core needs (it hands the raw CPU pointer to the DMA, no
   `local_to_global`). With `CONFIG_DCACHE=n` the CPU and DMA share a coherent view.
   This closes the eth_dwmac glue's documented Tier-1.5 placement gap
   (`eth_dwmac_alif_ensemble.c` header).

> The earlier PARTIAL write-up blamed the "PHY RX data path / REF_CLK" and the
> `ANLPAR=0` symptom. That was a red herring caused by (a) a bad cable masking the
> media link in the first round, and (b) the DTCM placement starving the data plane.
> Once the buffers moved to SRAM0, `ANLPAR` populated (`0xdde1` = the switch's base
> page) and DHCP completed on the first try. No scope was needed.

See [[project_pending_hw_configs]]. Run on the bench switch (dnsmasq,
192.168.10.50–150); the `[eth] DHCP lease = …` line is the full-link PASS.
