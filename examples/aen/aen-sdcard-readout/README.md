# aen-sdcard-readout

Bring up the Ensemble **E8 SD Host Controller** on the E1M-AEN801 (M55-HE) through
the vendored `snps,dwc-sdhc` driver + the Zephyr SDMMC disk, and probe a microSD
card via the standard **disk-access API** (`disk_access_init` /
`disk_access_ioctl`).

## The block

The E8 SD/SDIO controller is `sdhc@48102000` (`snps,dwc-sdhc`, IRQ 102/103).
Upstream Zephyr v4.4 and `hal_alif` ship **no** DesignWare SDHC driver, so the
controller driver is vendored **verbatim** from the Apache-2.0 `zephyr_alif` fork
(`drivers/sdhc/sdhc_dwc.c` + `sdhc_dwc.h`) as an **ADR 0017 Tier-2** copy. Data is
moved by the card's **internal ADMA2/SDMA** engine (no Alif system-DMA
dependency). The optional CPU↔bus DMA address translation — which would pull in
the `hal_alif` `soc_memory_map.h` and the `itcm/dtcm` `global_base` props absent
from our dtsi (the same gap the NPU work hit) — is left **off**, so the driver
uses identity addressing.

One porting patch vs the fork: `bus_4_bit_support` moved from
`sdhc_host_caps` to `sdhc_host_props` between the fork's `sdhc.h` and upstream
v4.4 — the driver now sets `props->bus_4_bit_support`.

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-sdcard-readout
# flash + run per docs/aen-bench-bringup.md, then read ram_console_buf over SWD.
```

## What it shows

1. `cc3501e_bridge_bringup()` → power up the on-module CC3501E coprocessor and
   bring the inter-chip SPI1 bridge up (~900 ms).
2. Assert the SDIO mux ENABLE (E1M `IO20` → CC3501E `GPIO_26`, active-low)
   over the CC3501E GPIO proxy, so the card is actually connected to the SoC.
3. `disk_access_init("SD")` → SDHC controller + SD card enumeration.
4. On success, read the card geometry (`GET_SECTOR_COUNT` / `GET_SECTOR_SIZE`).

This app is now self-sufficient: it no longer needs `aen-evk-demo`'s other
phases to reach the card, only steps 1-2 above, run before step 3. **Still
read-only by construction** — no `CONFIG_FILE_SYSTEM`, no `fs_*` call, no
`disk_access_write`, no `mkfs` anywhere in this app.

`RESULT PASS` requires a card to enumerate. A bridge or mux failure is its own
`RESULT FAIL`, reported before `disk_access_init` is even attempted — a disk
error measured with the mux undriven is not a real measurement. A clean
bridge + mux + controller bring-up where the card still does not enumerate is
reported **PARTIAL**.

## Status

**Controller/driver path PROVEN on E8, and the bridge + mux path now runs
too (RESULT PARTIAL):** `cc3501e_bridge_bringup()` powers the coprocessor and
brings SPI1 up; the mux ENABLE write + read-back both run over the GPIO
proxy; the `snps,dwc-sdhc` driver builds, the device inits, and
`disk_access_init` runs. No card enumerates, for a different and still-open
reason (see below) — not because the mux is undriven any more.

> **What used to block a card from being reachable at all, and is now
> resolved by this app itself:**
> 1. ~~A CC3501E GPIO-proxy write that routes the mux~~ — the coprocessor
>    firmware always implemented the opcode (`CMD_GPIO_WRITE` 0x51 →
>    `handle_gpio_write` in `cc3501e-bridge-firmware:src/protocol.c`,
>    firmware `0.4.0`, protocol version `5`); this app now calls it via
>    `alp_gpio_write(ALP_E1M_GPIO_IO20, false)` over the proxy.
> 2. ~~The inter-chip SPI1 link brought up in this app~~ — `main()` now calls
>    `cc3501e_bridge_bringup()` (copied verbatim from `aen-evk-demo`'s
>    `src/cc3501e_bridge.{c,h}`) before touching the disk at all.
> 3. **SD pad route** — this overlay wires the **B** route (`CLK=P14_1`,
>    `CMD=P14_0`, `D0..D3=P13_0..P13_3`), confirmed against the module
>    schematic and corroborated by five metadata sources plus two netlists
>    (see the overlay's header comment). Unchanged by this revision.
>
> **What is STILL bench-set by hand, not by this app:** the mux **SELECT**
> (E1M `IO21`) is not software-drivable on this module at all — on r2 it is
> physically open, on r1 driving it would contend with the P18 header
> jumper — so it is set by a jumper on header **P18** (see
> `aen-evk-demo`'s phase 9 header comment for the full netlist trace). A
> wrong or missing jumper still reads as `DISK_STATUS_NOMEDIA` from this app,
> indistinguishable from an empty slot.
>
> **What is open now that the bridge and mux are proven driven, and is NOT
> this app's job to fix:** the card still does not enumerate. `SW_RST_R` at
> `0x4810202F` stays `0x02`, so `SW_RST_CMD` never self-clears and
> `disk_access_init` returns `-116`. That is a controller/card-handshake
> question, under separate investigation, not a mux or bridge one — this
> app's job was to stop measuring that question with the card electrically
> disconnected, which it now has.

So on this bench the SDHC **controller + driver are proven**, the **bridge +
mux path is proven driven by this app alone**, and the remaining gap to a
card actually enumerating is the `SW_RST_CMD` handshake above. Tier-2 retires
onto the opt-in fork once a card is actually read.
