# rpmsg-v2n

> **Status: the raw A55↔M33 transport is bench-proven; this example's
> own two halves are not a matched pair.**
>
> - **Proven (#697), on E1M-X V2N-M1 silicon:** the raw OpenAMP
>   transport that `m33_sm/src/main.c` implements -- resource table,
>   vrings, MHU mailbox doorbell, rpmsg endpoint -- against real
>   RZ/V2N devicetree.  Attach + echo round-trip (1/4/16/64 B) and
>   the GHSA-xhm8 concurrent-close case all pass end-to-end, with
>   Renesas's `rpmsg_sample_client` over UIO as the Linux peer.
> - **Not proven:** the `linux/` + `m33_sm/` pairing *in this
>   directory*.  `linux/src/main.c` subscribes to a `temperature`
>   method that the M33 echo responder never publishes, so the two
>   slices as shipped do not talk to each other.  `m33_sm` is
>   `build_only: true` in CI (no native_sim: it needs real V2N
>   devicetree + the Renesas MHU/FSP/OpenAMP modules).
> - **By design:** the M33 slice **deliberately bypasses
>   `<alp/rpc.h>`** and uses raw OpenAMP (alp-sdk #683 "Path B,
>   Phase 1").  Its verified peer is Renesas's licence-gated
>   Multi-OS Package sample, which dials a resource table shaped
>   exactly like `resource_table.c` -- not whatever Zephyr's
>   `ipc_service` would generate.  See `m33_sm/src/main.c`'s file
>   header.  A v2n-specific `<alp/rpc.h>` backend is follow-up work.

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
  vendor sample.
- The **A55 / Yocto consumer** (`linux/src/main.c`) shows the
  intended portable shape: `alp_rpc_open()` then
  `alp_rpc_subscribe(ch, "temperature", ...)`.  Read it as the
  target API surface -- **not** as a client of the echo slave above,
  which never publishes that method.
- The **orchestrator's IPC contract** -- `<alp/system_ipc.h>` is
  auto-emitted from the project's `ipc:` block, so the carve-out
  address and endpoint ids are declared once rather than
  hand-written on either side.

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
4. Both sides run the `alp_default_rpmsg` name-service handshake
   over OpenAMP.  The M33 slice does this through raw OpenAMP
   (`rpmsg_create_ept()`), not `alp_rpc_open()` -- see the status
   note at the top.

The M33 firmware lands in the rootfs via the orchestrator's bbappend
to `meta-alp-sdk` (spec §6.5).

## Build

```bash
cd alp-workspace
tan build alp-sdk/examples/multicore/rpmsg-v2n
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
tan build alp-sdk/examples/multicore/rpmsg-v2n --core m33_sm
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
