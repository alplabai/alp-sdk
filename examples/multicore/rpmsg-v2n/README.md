# rpmsg-v2n

> **Status: the raw A55↔M33 transport is bench-proven; the two halves
> in this directory are now a matched pair (alp-sdk #1167).**
>
> - **Proven (#697), on E1M-X V2N-M1 silicon:** the raw OpenAMP
>   transport that `m33_sm/src/main.c` implements -- resource table,
>   vrings, MHU mailbox doorbell, rpmsg endpoint -- against real
>   RZ/V2N devicetree.  Attach + echo round-trip (1/4/16/64 B) and
>   the GHSA-xhm8 concurrent-close case all pass end-to-end, with
>   Renesas's `rpmsg_sample_client` over UIO as the reference Linux
>   peer.  `m33_sm` is `build_only: true` in CI (no native_sim: it
>   needs real V2N devicetree + the Renesas MHU/FSP/OpenAMP modules);
>   its transport code is unchanged by #1167.
> - **By design, the M33 slice still bypasses `<alp/rpc.h>`'s framed
>   convention** and speaks raw OpenAMP (alp-sdk #683 "Path B, Phase
>   1"): it echoes whatever bytes land on its fixed endpoint rather
>   than publishing a named method.  What changed in #1167 is the
>   **Linux side**: `linux/src/main.c` now drives that fixed endpoint
>   through `src/backends/rpc/yocto_uio_drv.c` -- the `<alp/rpc.h>`
>   backend that already targets this exact firmware (see that
>   backend's file header) -- and round-trips an `echo_test` request
>   instead of subscribing to a `temperature` push the M33 never
>   sent.  See `m33_sm/src/main.c`'s file header and
>   `linux/src/main.c`'s file header for the full rationale.

Heterogeneous-compute flagship: **Yocto Linux on the V2N's Cortex-A55
cluster, Zephyr RTOS on the same V2N's Cortex-M33 system-manager**,
talking over RPMsg.  One SoM, real-time plus Linux, one declarative
source of truth.

```
examples/multicore/rpmsg-v2n/
├── board.yaml          (v2; declares a55_cluster + m33_sm + ipc)
├── README.md           (this file)
├── linux/              (a55_cluster's Yocto slice)
│   ├── CMakeLists.txt
│   └── src/main.c      (consumer using <alp/rpc.h>)
└── m33_sm/             (m33_sm's Zephyr slice)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c      (raw OpenAMP echo slave; NOT <alp/rpc.h>)
```

## What changed vs v0.5

Prior to v0.6 the dual-OS framing lived in two places that had to
stay in sync by hand: this directory's `board.yaml` covered the
Zephyr/M33 half, and the Yocto/A55 half hid behind a separate
bitbake recipe that didn't consume the same config.  v0.6's
orchestrator (`scripts/alp_orchestrate/`) reads **one**
`board.yaml`, fans out per-core slices, and emits a system manifest
that the image-bundle + flash + OTA tooling consume.

## What it shows

- The **M33-SM / Zephyr slice** (`m33_sm/src/main.c`) stands up the
  raw OpenAMP rpmsg slave endpoint and **echoes** whatever bytes it
  receives -- the behaviour Renesas's `rpmsg_sample_client` verifies
  from Linux.  It drives no sensor and publishes no `temperature`
  event; it is the transport proof, adapted near-verbatim from the
  vendor sample, and #1167 leaves its transport code untouched.
- The **A55 / Yocto consumer** (`linux/src/main.c`) opens an
  `<alp/rpc.h>` channel via `src/backends/rpc/yocto_uio_drv.c`
  pointed at the M33's fixed endpoint address, then calls
  `alp_rpc_call(ch, "echo_test", ...)` in a loop and verifies the
  exact bytes come back.  This is the live, verifiable peer the M33
  side's echo behaviour was written for -- not a `temperature`
  subscriber the M33 never feeds.
- The **orchestrator's IPC contract** -- `<alp/system_ipc.h>` is
  auto-emitted from the project's `ipc:` block, so the carve-out
  address is declared once; the M33 endpoint address itself is a
  fixed constant on both sides (see `linux/src/main.c`'s header) since
  the M33 slice doesn't follow the generated endpoint-id convention.

## Memory map

The orchestrator resolves the `alp_default_rpmsg` carve-out
deterministically from E1M-V2N101's `memory_map:` block.  The
default non-cacheable region is `ocram_low` (512 KiB at
`0x00010000`).  A 512 KiB carve-out reserves the entire region;
re-runs of `tan build` produce byte-identical placements
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
4. Both sides bring up the rpmsg link over OpenAMP: the M33 slice
   creates its raw endpoint directly (`rpmsg_create_ept()`), not
   through `alp_rpc_open()`; the Linux side attaches to that fixed
   endpoint address via `alp_rpc_open()` (no name-service announce)
   -- see the status note at the top.

The M33 firmware lands in the rootfs via the orchestrator's bbappend
to `meta-alp-sdk` (spec §6.5).

## Build

```bash
cd alp-workspace/alp-sdk/examples/multicore/rpmsg-v2n
tan build
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

`tan build` has no per-slice `--core` flag -- it rebuilds every slice
on each invocation.  Just re-run it from the project directory: the
already-built Yocto slice is reused (bitbake short-circuits an
up-to-date tree) while the Zephyr M33 slice rebuilds incrementally in
seconds, skipping Yocto's hour-long rebuild:

```bash
tan build
```

Image + flash:

```bash
tan image     # -> build/image-bundle/alp-system.zip + .swu
tan flash     # walks boot_order: from the manifest
```

## Reference

- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- end-to-end app-developer walk-through.
- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- framed RPMsg surface
  (spec §6.6).
- [`examples/multicore/mproc-mailbox/`](../mproc-mailbox/) -- single-SoC
  variant of the same pattern (AEN M55-HP <-> M55-HE).
- [`examples/multicore/heterogeneous-offload/`](../heterogeneous-offload/) --
  flagship demo that delegates FFT to the M-class peer.
- [`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`](../../../docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md)
  -- full design rationale.
