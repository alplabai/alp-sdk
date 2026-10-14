# aen-sdcard-file

Write, read back and list a file on the **E1M-EVK microSD card** (slot J7)
with **FatFs**, from either M55 of any E1M-AEN SoM with a board tree:
the M55-HE or M55-HP of the **E1M-AEN801/AEN803** (Ensemble E8) and the
M55-HP of the **E1M-AEN401** (Ensemble E4).

```
[sd] CC3501E bridge bring-up -> 0
[sd] SDIO mux enabled (IO20 low)
[fs] mount /SD: -> 0
[fs] wrote 4096 B, sync -> 0
[fs] read 4096 B, match=1
[fs] /SD: contents:
  ...
  f alp_sdcard_file.txt              4096
[fs] 114132 MiB free of 122208 MiB
RESULT PASS: file
```

## How the pieces fit

| Piece | Where it comes from |
|---|---|
| SD host `sdhc0` + the Zephyr disk `"SD"` | the [`e1m_evk_microsd`](../../../zephyr/boards/shields/e1m_evk_microsd) shield, added in `CMakeLists.txt` |
| FatFs | `libraries: [fatfs]` in `board.yaml`; exFAT + long names in `prj.conf` |
| SDIO mux enable (E1M `IO20`, active low) | `src/main.c`, through the CC3501E GPIO proxy |

On the E1M-AEN SoMs `IO20` is wired to the on-module CC3501E, so the app
brings the CC3501E bridge up first, then drives `IO20` low, then mounts the
card. The SD host driver does not touch the card before the mount, so having
`sdhc0` enabled from boot is safe.

The file buffers are ordinary statics in DTCM. The SD host's DMA engine
cannot reach DTCM; the driver copies through a buffer in SRAM0
(`CONFIG_SDHC_DWC_BOUNCE_BUFFER`, on by default for Ensemble), so the app
needs no special placement.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_hp` here, in `build/m55_hp-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

`board.yaml` sets `som.sku: E1M-AEN801`. The bench verification below ran on an E1M-AEN803; to build for it, set `som.sku: E1M-AEN803` (it has the same `m55_he` core, and the example ships an AEN803 overlay).

## Building for another SoM or core

`board.yaml` names the E1M-AEN801 M55-HE. For another target, change `som.sku`
and the core key, then build with tan, which regenerates `generated/alp.conf`
for that SoC:

| Target | `som.sku` | core key | Notes |
|---|---|---|---|
| AEN801 / AEN803 M55-HE | `E1M-AEN801` / `E1M-AEN803` | `m55_he` | bridge SPI on DMA0 |
| AEN801 / AEN803 M55-HP | `E1M-AEN801` / `E1M-AEN803` | `m55_hp` | bridge SPI on DMA0 |
| AEN401 M55-HP | `E1M-AEN401` | `m55_hp` | bridge in PIO mode (`boards/*aen401*.conf`); drop the `a32_cluster` entry, the E4 has no A32 |

Each target has its own overlay in `boards/` (the CC3501E link, the EEPROM
bus) and the `e1m_evk_microsd` shield has one per board. The twister
scenarios in `testcase.yaml` build all five.

## Hardware

- E1M-EVK 2626-R2 whose SDIO mux parts U38/U39 are the **reworked 74LV3257**
  bus switches. The stock 74LVC157 parts drive their outputs at all times and
  fight the SoC's SD pins while the mux is enabled; do not run this example
  on an unreworked carrier.
- Header **P18 open** (selects the microSD slot, not the M.2 E-key card).
- A microSD card in J7, FAT32 or exFAT. The example never formats the card;
  it creates `alp_sdcard_file.txt` and deletes it again.

## Verified

2026-10-13, E1M-AEN803 2026W36-0002 on an E1M-EVK 2626-R2 with the reworked mux, 128 GB exFAT card,
4-bit at 25 MHz, M55-HE RAM-run with the data cache on: `RESULT PASS: file`.
A 48 KB variant (6 split 8 KB bounce transfers per read) also passed, and a
96-sector raw read matched 96 single-sector reads. The same shield and driver
mounted and listed the card from the M55-HP with Zephyr's `fs_sample`.
On 2026-10-12 the CC3501E bridge, the EEPROM and a 4-bit card at 25 MHz
worked on an E1M-AEN401 M55-HP from a bench app with the same overlay values.
This example's own HP and AEN401 builds are compile-checked, not yet run.
