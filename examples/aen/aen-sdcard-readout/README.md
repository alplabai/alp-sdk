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

1. `disk_access_init("SD")` → SDHC controller + SD card enumeration.
2. On success, read the card geometry (`GET_SECTOR_COUNT` / `GET_SECTOR_SIZE`).

`RESULT PASS` requires a card to enumerate. A clean controller bring-up where the
card is simply not reachable is reported **PARTIAL**.

## Status

**Controller/driver path PROVEN on E8 (RESULT PARTIAL):** the `snps,dwc-sdhc`
driver builds, the device inits, and `disk_access_init` runs and returns "no card"
cleanly. No card enumerates because the SD slot is not reachable on this bench:

> **For a card to enumerate (BENCH-BLOCKED) — NOT a driver bug, and NOT completable
> in M55 software on this bench.** The EVK microSD sits behind a 74LVC157 SDIO mux
> whose `EN`=**IO20**→CC3501E **GPIO_26** and `SEL`=**IO21**→CC3501E **GPIO_30** are
> *both on the CC3501E*. Reaching the card needs all of:
> 1. **CC3501E firmware support** — the v0.1 coprocessor firmware does **not**
>    implement the GPIO-proxy opcode (`CMD_GPIO_WRITE` 0x51 hits `default →
>    RESP_ERR_INVALID` in `firmware/cc3501e/src/protocol.c`). Driving the mux needs
>    NEW firmware on the CC3501E + a reflash — a task on the *other chip*, not the M55.
> 2. **A working inter-chip SPI1 link** — never round-tripped on silicon (latest
>    bench commit: "CC3501E drives no reply (MISO reads 0xFF)"; the host driver
>    header is marked `[UNTESTED]`). No `GPIO_WRITE` can reach the mux until PING
>    succeeds.
> 3. **SD pad route + DMA translate:** this overlay wires the **D** route (CLK=P4_1,
>    CMD=P4_2, D0..D3=P6_0..P6_3) as a documented default (confirm vs schematic; data
>    pads also want `input-enable`), and correct ADMA2 transfers need
>    `CONFIG_SDHC_DWC_DMA_ADDR_TRANSLATE` + the `itcm/dtcm` `global_base` dtsi props
>    ([[project_pending_hw_configs]]).

So on this bench the SDHC **controller + driver are proven** (builds, inits,
`disk_access_init` runs cleanly) but the card is **unreachable until the CC3501E
bridge + its GPIO-proxy firmware exist**. Tier-2 retires onto the opt-in fork once
a card is actually read (task #21).
