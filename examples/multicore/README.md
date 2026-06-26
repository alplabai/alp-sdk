# Multicore examples

Heterogeneous-compute examples for Alp SDK SoMs — one project, multiple cores,
built by a single `west alp-build` invocation.

| Example | SoM default | Cores | Transport | Status |
|---------|-------------|-------|-----------|--------|
| [`rpmsg-aen/`](rpmsg-aen/) | E1M-AEN801 | A32 Linux + M55-HP Zephyr | RPMsg over MHUv2 | `[UNTESTED]` structural draft; carveout resolves; M55-HP firmware baked locally |
| [`rpmsg-v2n/`](rpmsg-v2n/) | E1M-V2N101 | A55 Linux + M33-SM Zephyr | RPMsg over SCI-UART | `[UNTESTED]` structural draft |
| [`rpmsg-imx93/`](rpmsg-imx93/) | E1M-NX9101 | A55 Linux + M33 Zephyr | RPMsg | `[UNTESTED]` structural draft |
| [`mproc-mailbox/`](mproc-mailbox/) | — | native_sim | Mailbox loopback | Zephyr CI only |
| [`heterogeneous-offload/`](heterogeneous-offload/) | — | A + M | Offload demo | `[UNTESTED]` |

## AEN801 multicore (SP3)

`rpmsg-aen/` is the primary AEN801 multicore example.  As of SP3 (2026-06-26):

- `board.yaml` defaults to **E1M-AEN801** (Alif Ensemble E8: dual A32 + M55-HP).
- The orchestrator resolves the **256 KiB `alp_default_rpmsg` carveout** from
  `sram0` (0x023c0000) — grounded in `metadata/e1m_modules/E1M-AEN801.yaml`
  `memory_regions`.
- The carrier DTS (`e1m-aen801-evk.dts` + `aen801-dts-reservations.dtsi`) carries
  the carveout, MHUv2 TX/RX, and `remoteproc-m55-hp` nodes.
- `alp-remoteproc` is baked into `alif-tiny-image` via `IMAGE_INSTALL:append` in
  `e1m-aen801-a32.conf`; the systemd unit starts the M55-HP firmware on boot.
- `aen-m55-hp-fw` (the prebuilt M55-HP ELF) is `SKIP_RECIPE`-gated in the public
  layer; supply the ELF locally or via `alp-sdk-internal` and clear the skip to
  bake.

AEN701 (`board-aen701.yaml`) remains an alternate; it is **blocked** until
`mailbox.controller: TBD` in `metadata/e1m_modules/E1M-AEN701.yaml` is filled.

## Build

Default (AEN801):

```bash
cd alp-workspace
west alp-build alp-sdk/examples/multicore/rpmsg-aen
```

See [`rpmsg-aen/README.md`](rpmsg-aen/README.md) for the per-SKU and per-slice
build options.

## Reference

- [`docs/heterogeneous-builds.md`](../../docs/heterogeneous-builds.md) — full
  dual-OS build walk-through.
- [`<alp/rpc.h>`](../../include/alp/rpc.h) — framed RPMsg surface.
