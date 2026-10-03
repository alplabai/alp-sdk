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
3. TF-A BL2 starts the M33-SM core from the xSPI image at power-on
   (`docs/rzv2n-m33-secure-boot.md`).  With the opt-in remoteproc
   (`ALP_V2N_REMOTEPROC = "1"`) Linux then attaches to the running core
   and can stop, start and reload it (`/lib/firmware/m33_sm.elf`).
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

## Bench: one RPC round trip over the UIO backend (V2N / V2M)

Proves alp-sdk #2374's last item: an A55 `<alp/rpc.h>` call reaching an
IPC-enabled CM33 image through `yocto_uio_drv.c`.  The CM33 window is
`0x9f700000` (CM33-NS view) = `0x4f700000` (A55 view), 9 MiB.

1. **Build the CM33 image** (V2M shown; use
   `alp_e1m_v2n101_m33_sm/r9a09g056n48gbg/cm33` on V2N):

   ```bash
   west build -p always -b alp_e1m_v2m101_m33_sm/r9a09g056n48gbg/cm33        examples/multicore/rpmsg-v2n/m33_sm
   ```

2. **Pad and flash to mtd1 @ 0x1a0000** (from the board, after copying
   `zephyr.bin` over; size the erase from the file):

   ```sh
   head -c 12288 /dev/zero > /tmp/m33_fw.bin; cat /tmp/zephyr.bin >> /tmp/m33_fw.bin
   SZ=$(stat -c %s /tmp/m33_fw.bin); BLKS=$(( (SZ + 4095) / 4096 ))
   flash_erase /dev/mtd1 0x1a0000 $BLKS
   mtd_debug write /dev/mtd1 0x1a0000 $SZ /tmp/m33_fw.bin
   mtd_debug read  /dev/mtd1 0x1a0000 $SZ /tmp/rb.bin && md5sum /tmp/rb.bin /tmp/m33_fw.bin
   ```

   The two md5s must match.  Restarting the CM33 without a SoC reboot
   needs the dev-only remoteproc stop/reload (`ALP_V2N_CM33_SRAM_NS = "1"`, `docs/rzv2n-m33-secure-boot.md`,
   "Lifecycle", bench-pending); otherwise do a full SoC reboot (or PSU
   cold-cycle).
   The board must boot in DSW1 mode 2 (xSPI BL2): under the mode 1
   eMMC-boot BL2 the CM33 never starts.

   **Attach / detach without a cold cycle (#2586, not yet
   bench-verified).**  With this firmware (beacon version 2) and the
   matching `yocto_uio_drv.c`, every `alp_rpc_open()` after the first
   one in a CM33 boot runs an attach reset: the A55 writes the
   resource table's `vdev.status = 0`, kicks the CM33, and waits up to
   500 ms for the attach-epoch word to change; the CM33 stops its
   responder, drops queued frames, tears down its virtio device,
   bumps the epoch, and waits for the next attach.  Until a bench run
   confirms it, keep cold-cycling between runs.  A CM33 still running
   the version-1 firmware cannot reset: the second open in the same
   CM33 boot now fails with `ALP_ERR_BUSY` and a message naming the
   reason, instead of the old first-call timeout and one-call-late
   replies.

   **CM33 beacon map** (top of `rsctbl`, A55 `0x4f700ff0`, CM33-NS
   `0x9f700ff0`; read with `devmem`).  The layout is defined once in
   `include/alp/protocol/amp_beacon.h` (offsets from the end of the rsctbl page);
   the page itself is the SoC metadata's `openamp_carveout.regions.rsctbl`:

   | Offset  | A55 address  | Word                                                              |
   |---------|--------------|-------------------------------------------------------------------|
   | `+0xFF0`| `0x4f700ff0` | magic `0xA10D0683`                                                |
   | `+0xFF4`| `0x4f700ff4` | version: `1` = RPC firmware without attach reset, `2` = with it; `>= 0x100` = image without RPC (`0x100` = idle stock shim *with the heartbeat beacon*) |
   | `+0xFF8`| `0x4f700ff8` | ~1 Hz heartbeat counter                                           |
   | `+0xFFC`| `0x4f700ffc` | attach epoch (version 2): `0` at boot; **odd = CM33 bound to a session, even = waiting for an attach** |

   `alp_rpc_open()` fails with `ALP_ERR_NOT_READY` when the magic is
   missing (CM33 not running, or a stock shim without the heartbeat
   beacon) and with `ALP_ERR_NOSUPPORT` on a version `>= 0x100`, without
   writing anything the CM33 reads.  A second process opening while
   another holds the link gets `ALP_ERR_BUSY` (the backend takes
   `flock(LOCK_EX | LOCK_NB)` on the `rsctbl` UIO node).

3. **Check the A55 half**: `cat /sys/class/uio/uio*/name` lists `rsctbl`,
   `mhu-shm`, `vring-ctl0`, `vring-ctl1`, `vring-shm0`, `vring-shm1`,
   `mhu-uio`; `/proc/iomem` shows `4f700000-4fffffff : reserved`.

4. **Run the round trip.**  Either the HIL spec
   (`tests/hil/v2m103-x-evk/v2m103-rpmsg-echo-uio.yaml`, binary at
   `<artifact-dir>/linux`) or, with the static bench binary from
   `tests/yocto/build_rpc_uio_bench_aarch64.sh`, run it directly on the
   board.  Pass = `[rpmsg-v2n] done (4/4 round trips verified)`; the
   `/proc/interrupts` `mhu-uio` count rises.

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
