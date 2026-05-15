# rpmsg-v2n

> `[UNTESTED]` -- v0.6 paper-correct.  Both halves declared in one
> `board.yaml` v2; the orchestrator fans out Yocto + Zephyr in
> parallel.  HiL bring-up on V2N + Yocto gates on v0.8.

Heterogeneous-compute flagship: **Yocto Linux on the V2N's Cortex-A55
cluster, Zephyr RTOS on the same V2N's Cortex-M33 system-manager**,
talking over a framed RPMsg channel.  One SoM, real-time plus Linux,
one declarative source of truth.

```
examples/rpmsg-v2n/
├── board.yaml          (v2; declares a55_cluster + m33_sm + ipc)
├── README.md           (this file)
├── linux/              (a55_cluster's Yocto slice)
│   ├── CMakeLists.txt
│   └── src/main.c      (consumer using <alp/rpc.h>)
└── m33_sm/             (m33_sm's Zephyr slice)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c      (producer using <alp/rpc.h>)
```

## What changed vs v0.5

Prior to v0.6 the dual-OS framing lived in two places that had to
stay in sync by hand: this directory's `board.yaml` covered the
Zephyr/M33 half, and the Yocto/A55 half hid behind a separate
bitbake recipe that didn't consume the same config.  v0.6's
orchestrator (`scripts/alp_orchestrate.py`) reads **one**
`board.yaml`, fans out per-core slices, and emits a system manifest
that the image-bundle + flash + OTA tooling consume.

## What it shows

- The **M33-SM / Zephyr producer** (`m33_sm/src/main.c`) reads a
  sensor via the portable `<alp/peripheral.h>` and publishes one
  `temperature` event per second through `<alp/rpc.h>`.
- The **A55 / Yocto consumer** (`linux/src/main.c`) opens the
  matching RPC channel, subscribes to `temperature`, and prints
  every sample to stdout.
- The **same C API** -- both binaries `#include <alp/rpc.h>` and
  `<alp/system_ipc.h>`.  The header is auto-emitted by the
  orchestrator from the project's `ipc:` block; both slices share
  identical endpoint ids and the carve-out address.

## Memory map

The orchestrator resolves the `alp_default_rpmsg` carve-out
deterministically from E1M-V2N101's `memory_map:` block.  The
default non-cacheable region is `ocram_low` (512 KiB at
`0x00010000`).  A 512 KiB carve-out reserves the entire region;
re-runs of `west alp-build` produce byte-identical placements
(spec §6.1).

| Range                       | Owner       | Notes                                                |
|-----------------------------|-------------|------------------------------------------------------|
| `0x48000000 + 0x000`        | A55 (DDR)   | Linux kernel + rootfs (LPDDR4X main memory).         |
| `0x00010000 – 0x00090000`   | **IPC**     | `alp_default_rpmsg` -- ocram_low, no-cache.          |
| `0x80000000 + 0x000`        | M33-SM      | M33 TCM (Zephyr image + .data + .bss).               |

The generated `<alp/system_ipc.h>` carries the resolved address +
size + endpoint ids; neither side hand-writes them.

## Boot order

The V2N101 preset's `boot_order:` is copied verbatim into the
system manifest.  In summary:

1. A55 cluster reads U-Boot from xSPI, hands off to Linux.
2. systemd reaches its basic target.
3. The remoteproc driver loads
   `/lib/firmware/alp/E1M-V2N101/m33_sm.elf` into the M33-SM core
   and starts it.
4. Both sides run the `alp_default_rpmsg` name-service handshake
   over OpenAMP; `alp_rpc_open()` returns on the A55 side first,
   then on the M33.

The M33 firmware lands in the rootfs via the orchestrator's bbappend
to `meta-alp-sdk` (spec §6.5).

## Build

```bash
cd alp-workspace
west alp-build alp-sdk/examples/rpmsg-v2n
```

That single command:

1. Reads `board.yaml`, resolves the V2N101 preset's topology.
2. Fans out two slices in parallel:
   - `build/a55_cluster-yocto/` (bitbake against
     `MACHINE = e1m-v2n101-a55`)
   - `build/m33_sm-zephyr/` (Zephyr against
     `BOARD = alp_e1m_v2n101_m33_sm`)
3. Emits `build/generated/alp_system_ipc.h` +
   `build/generated/dts-reservations.dtsi` -- the shared IPC
   contract.
4. Writes `build/system-manifest.yaml` recording every slice's
   binary, the carve-out resolution, the helper-MCU firmwares, and
   the boot order.

Iteration:

```bash
# Rebuild only the M33 slice (skips Yocto's hour-long rebuild):
west alp-build alp-sdk/examples/rpmsg-v2n --core m33_sm
```

Image + flash:

```bash
west alp-image     # -> build/image-bundle/alp-system.zip + .swu
west alp-flash     # walks boot_order: from the manifest
```

## Reference

- [`docs/heterogeneous-builds.md`](../../docs/heterogeneous-builds.md)
  -- end-to-end app-developer walk-through.
- [`<alp/rpc.h>`](../../include/alp/rpc.h) -- framed RPMsg surface
  (spec §6.6).
- [`examples/mproc-mailbox/`](../mproc-mailbox/) -- single-SoC
  variant of the same pattern (AEN M55-HP <-> M55-HE).
- [`examples/heterogeneous-offload/`](../heterogeneous-offload/) --
  flagship demo that delegates FFT to the M-class peer.
- [`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`](../../docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md)
  -- full design rationale.
