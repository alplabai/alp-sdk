# aen-dma-regcheck — ARM PL330 DMA M2M memcpy on the E8

On-silicon validation of the **ARM PL330 DMA controller** on the E1M-AEN801
(Alif Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.

Unlike the bind-only `*-regcheck` siblings (CAN / camera / ISP, which are
HW-blocked on missing external wiring), this app performs a **real
memory-to-memory copy** through the PL330 and verifies the bytes: the PL330
needs no pins or transceiver, so it actually runs.

## What it validates

1. **Binding (Tier-1, upstream-native).** The `dma2` DT node binds to UPSTREAM
   Zephyr v4.4's ARM PL330 driver (`drivers/dma/dma_pl330.c`, compatible
   `arm,dma-pl330`) at the E8's core-local secure DMA2 base `0x400C0000`, with
   8 execution channels. No vendored or forked code (ADR 0017 Tier-1).
2. **Alias.** The portable `alp_dma0` alias resolves to that same node.
3. **Instantiation.** `DEVICE_DT_GET` resolves and `device_is_ready()` is true.
4. **Transfer.** A 1000-byte `MEMORY_TO_MEMORY` copy via `dma_config()` +
   `dma_start()`, then a `memcmp()` of source vs destination.

The `RESULT PASS` line gates on the real copy: bind + alias + ready + a
byte-for-byte verified M2M transfer.

## Why it can run synchronously

The upstream PL330 driver is a **polling** engine.
`dma_pl330_transfer_start()` → `dma_pl330_submit()` → `dma_pl330_xfer()` →
`dma_pl330_wait()`, which spins on the channel-status `CS0` register until the
channel goes idle. So when `dma_start()` returns `0` the copy has already
completed — no interrupt, no completion callback (the `arm,dma-pl330` binding
declares no `interrupts`). The app verifies right after `dma_start()`.

## The #1 bench risk: DMA reachability (global SRAM0 only)

The PL330 is an **AXI bus master**. It fetches its generated channel microcode
*and* reads/writes the copy buffers over its own AXI master, so **every address
it touches must be global (AXI-visible)** — not a core-local TCM alias. The
M55's DTCM (local `0x20000000`, where the generated board puts default RAM) is
core-private and is **not** on the DMA path.

The overlay + app handle this:

- `src_buf` / `dst_buf` are tagged `section("SRAM0")` in `main.c`, landing them
  in the `sram0` linker region at `0x02000000` (global on-chip SRAM, 4 MiB).
- The PL330 `microcode` scratch is pointed at a carve-out at the **top** of that
  same bank (`0x023FE000`, 8 KiB = 8 channels × `MICROCODE_SIZE_MAX` 0x400).
- The `sram0` node is shrunk by that top 8 KiB so the buffers never overlap the
  microcode region.
- `CONFIG_DCACHE=n` (prj.conf) keeps the CPU and DMA views coherent with no
  cache maintenance (the upstream PL330 driver issues no clean/invalidate).

Unlike the NPU/eth paths, the upstream PL330 driver does **no** Alif
local→global remap — it programs the addresses verbatim — so the SRAM0
addresses the CPU hands it *are* the global addresses the engine fetches.

## E8 DMA topology (Alif AE822 DFP)

Each M55 cluster has its own PL330 at the same core-local window:

| Core    | Name | Secure base  | Non-secure base |
| ------- | ---- | ------------ | --------------- |
| RTSS-HE | DMA2 | `0x400C0000` | `0x400E0000`    |
| RTSS-HP | DMA1 | `0x400C0000` | `0x400E0000`    |

The shared/cross-core **DMA0** at `0x49080000` (sec) / `0x490A0000` (ns) is not
declared in the SoC overlay. This app/board targets the RTSS-HE local DMA2 at
the secure base — the address Alif's own PL330 instance prints
(`Device dma2@400c0000 initialized`).

### Per-peripheral request lines

For peripheral DMA (memory↔peripheral with hardware handshakes), the
**per-peripheral request-line map** is authoritative in the DFP at
`Device/soc/AE822FA0E5597/include/soc_dma_map.h` (e.g. `UART0_DMA_RX_PERIPH_REQ
= 8`, `I2C0_DMA_RX_PERIPH_REQ = 24`, `SPI0_DMA_TX_PERIPH_REQ = 20`, each with a
`*_GROUP` selecting one of the PL330 channel groups and `*_HANDSHAKE_ENABLE`).
**This M2M memcpy uses none of them** — memory-to-memory transfers have no
handshake peripheral, so no `dma_slot`/request line is programmed. A future
peripheral-DMA example wiring a UART/SPI/I2C through `dmas = <&dma2 N>` would
transcribe the relevant `*_PERIPH_REQ` from that file.

## Build (bench, serial-driven by the maintainer)

RAM-run for the RTSS-HE core; the board overlay
`boards/alp_e1m_aen801_m55_he.overlay` auto-applies for the fully-qualified
board:

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-dma-regcheck -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

Flash/run is the bench RAM-run (J-Link `loadbin` + go to ITCM); read
`ram_console_buf` over SWD for the `RESULT` line. Expected:

```
=== aen-dma-regcheck ===
dma2  : dma@400c0000
        bound=1 compat=arm,dma-pl330 base=0x400c0000 (exp 0x400c0000) channels=8 (exp 8)
alias : alp_dma0 -> dma@400c0000 (resolves_to_dma2=1)
dma2  device : READY (arm,dma-pl330 driver instantiated)
xfer  : dma_config rc=0 dma_start rc=0 memcmp_match=1 (1000 bytes M2M)
RESULT PASS: PL330 DMA WORKS -- dma2 binds to arm,dma-pl330 at 0x400c0000 ...
```

## Notes / caveats

- **BENCH-UNVERIFIED.** The node + transfer are authored against the pinned
  Zephyr 4.4 PL330 driver and the DFP addresses; not yet run on the E8.
- The `microcode` carve-out address (`0x023FE000`) and the SRAM0 shrink are an
  alp-sdk layout choice (top 8 KiB of the 4 MiB bank), not a DFP-fixed address.
- The user-microcode helper API from the sdk-alif `dma_user_mcode` sample
  (`dma_pl330_start_with_mcode()`, `<.../dma_pl330_opcode.h>`) is a **fork
  addition not present in pinned Zephyr 4.4**. This example deliberately uses the
  standard upstream `dma_config()` + `dma_start()` path, where the driver
  generates the M2M microcode internally — the Tier-1 upstream-native surface.
```
