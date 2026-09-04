# rpmsg-aen

> `[UNTESTED]` -- v0.6 structural draft.  Board.yaml + sources are
> shape-correct, but the end-to-end RPMsg path still needs AEN801
> bench validation.

Heterogeneous compute on **E1M-AEN801** (Alif Ensemble E8):

- The 2-core **Cortex-A32 cluster** boots Yocto Linux from MRAM and
  runs the consumer under `linux/`.
- The **Cortex-M55 HP** core boots from MRAM, reads the board's
  on-board LSM6DSO IMU + BMP581 barometer, and publishes one
  `imu_sample` event per second over `<alp/rpc.h>`.
- The **Cortex-M55 HE** core stays at the SoM topology default
  (stock-shim Zephyr image) -- alive for future low-power offload,
  not part of this demo's RPMsg channel.

```
examples/multicore/rpmsg-aen/
├── board.yaml          (v2; declares a32_cluster + m55_hp + ipc)
├── README.md           (this file)
├── CMakeLists.txt      (multi-slice project marker)
├── linux/              (a32_cluster's Yocto slice)
│   ├── CMakeLists.txt
│   └── src/main.c      (consumer subscribing to `imu_sample`)
└── m55_hp/             (m55_hp's Zephyr slice)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c      (producer reading sensors + publishing)
```

## Memory Map

The AEN801 preset resolves the mailbox controller and derives the
memory envelope from the E8 SoC variant.  The `alp_default_rpmsg`
carve-out lands in `mram_main`, accessible from all three cores.

Spec §6.8 says AEN defaults to cacheable carve-outs because the M55
cores have caches enabled — but the SDK emits no cache maintenance to
make that safe, so this example does **not** take that path.  The
`cacheable: true` opt-in is rejected on `kind: rpmsg` entries, and the
orchestrator emits `CONFIG_DCACHE=n` for the `m55_hp` slice instead, so
both sides see each other's writes without explicit clean/invalidate.
Tracked as #1088; see `docs/heterogeneous-builds.md`.

| Range                     | Owner                  | Notes                                                |
|---------------------------|------------------------|------------------------------------------------------|
| `mram_main`              | All cores              | On-die MRAM, cacheable. Holds the RPMsg carve-out.   |
| `sram_main`              | All cores              | On-die SRAM, non-cacheable scratch.                  |

## Boot order

AEN boots the M55-HP core first (out of reset from MRAM).  The
A32 cluster comes up second, brought online by a small bootloader
running on M55-HP.  RPMsg name-service handshake completes once
the A32 has reached the Linux user-space stage and opened its
side of `alp_default_rpmsg`.

| Stage | Core         | Action                              |
|-------|--------------|--------------------------------------|
| 1     | m55_hp       | Reset, run Zephyr early-boot         |
| 2     | m55_he       | Stock-shim Zephyr (idle wait)        |
| 3     | a32_cluster  | M55-HP-driven A32 bootloader → Linux |
| 4     | RPMsg        | Name-service handshake on both sides |

(Recorded verbatim into `system-manifest.yaml` once the SoM preset
ships the authoritative `boot_order:` block.)

## Build

```bash
cd alp-workspace/alp-sdk/examples/multicore/rpmsg-aen
tan build
```

Tan's relocated planner fans out:

- `build/a32_cluster-yocto/` (bitbake against `MACHINE = e1m-aen801-a32`).
- `build/m55_hp-zephyr/` (Zephyr against `BOARD = alp_e1m_aen801_m55_hp`).

`tan build` has no per-slice `--core` flag -- it rebuilds every slice
on each invocation.  To iterate on the M-side only, just re-run the
same command: the already-built Yocto slice is reused (bitbake
short-circuits an up-to-date tree) while the Zephyr slice rebuilds
incrementally in seconds. See
[`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md#iterating-on-one-slice).

## Reference

- [`examples/multicore/rpmsg-v2n/`](../rpmsg-v2n/) -- V2N counterpart of this
  AEN setup.
- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- per-core build walk-through.
- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- framed RPMsg surface.
