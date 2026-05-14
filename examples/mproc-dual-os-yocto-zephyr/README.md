# mproc-dual-os-yocto-zephyr

> `[UNTESTED]` -- v0.5 paper-correct.  Zephyr/M33 half builds clean
> under `native_sim`; HiL bring-up on V2N + Yocto gates on v0.8.

Heterogeneous-compute flagship: **Yocto Linux on the V2N's
Cortex-A55 cluster, Zephyr RTOS on the same V2N's Cortex-M33**,
talking through a shared-memory ring buffer.  One SoM, real-time
plus Linux, no second board.

## What it shows

- The **M33/Zephyr producer** (`m33/main.c`) reads a sensor via
  `<alp/peripheral.h>`, stages each sample into a shared ring, and
  rings a doorbell over `alp_mbox_send`.
- The **A55/Yocto consumer** (`linux/main.c`) opens the same
  `<alp/mproc.h>` region from user space, drains the ring, and
  prints to stdout.
- The **same C API** -- both binaries `#include <alp/mproc.h>`; the
  backend just differs (Zephyr mbox driver on M33, Linux mailbox
  controller + mmap'd carve-out on A55).

## Memory map

| Range                    | Owner    | Notes                              |
|--------------------------|----------|------------------------------------|
| `0x0`        – `0x10MB`  | M33      | Zephyr image + .data + .bss        |
| `0x10MB`     – `END`     | A55      | Linux kernel + rootfs              |
| `0x10MB-128KB` – `0x10MB`| **IPC**  | Shared ring; named `alp_dualos_ring` |

The IPC carve-out sits at the boundary between the M33 and A55
regions and is reserved in the DT under both OSes' views so neither
side maps it as ordinary memory.  V2N's coherency unit handles
cache traffic when enabled; if it is gated off, the SDK's mproc
backend falls back to explicit DMA-flush on every push (see
`backends/mproc/v2n_*.c` -- TBD wiring under §C.48).

## Build flow

Two binaries, two build systems:

```
bitbake alp-image-dual-os         # builds BOTH halves
  ├─ recipes-bsp/mproc-dual-os-m33      # cross-compiles m33/main.c
  │                                       under Zephyr SDK, bakes
  │                                       the firmware into the
  │                                       image rootfs.
  └─ recipes-apps/mproc-dual-os-linux   # invokes linux/CMakeLists.txt
                                          under the A55 sysroot,
                                          installs to /usr/bin.
```

Either half builds standalone:

```bash
# M33 / Zephyr half (this example's top-level CMakeLists.txt):
west build -b native_sim/native/64 examples/mproc-dual-os-yocto-zephyr

# A55 / Yocto half (plain CMake under linux/):
source /opt/poky/4.0/environment-setup-cortexa55-poky-linux
cmake -S linux -B build-linux
cmake --build build-linux
```

## Boot order

The M33 firmware starts first (right out of reset, from xSPI), waits
~1 s for the A55 cluster to come up under U-Boot + Linux, then begins
pushing samples.  The A55 consumer is launched by `systemd` once
the rootfs is mounted.

## Reference

- [`<alp/mproc.h>`](../../include/alp/mproc.h) -- mailbox + shmem API.
- [`examples/mproc-mailbox/`](../mproc-mailbox/) -- single-SoC version
  of the same pattern (AEN M55-HP <-> M55-HE).
- `docs/v1.0-readiness.md` §4 -- flagship app list; this entry is
  the heterogeneous-OS counterpart to `mproc-mailbox`.
