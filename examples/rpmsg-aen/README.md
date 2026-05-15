# rpmsg-aen

> `[UNTESTED]` -- v0.6 structural draft.  Board.yaml + sources are
> shape-correct, but the build will fail at carve-out resolution
> until the user supplies authoritative AEN memory-map values in
> `metadata/e1m_modules/E1M-AEN701.yaml`.

Heterogeneous compute on **E1M-AEN701** (Alif Ensemble E7):

- The 2-core **Cortex-A32 cluster** boots Yocto Linux from MRAM and
  runs the consumer under `linux/`.
- The **Cortex-M55 HP** core boots from MRAM, reads the carrier's
  on-board LSM6DSO IMU + BMP581 barometer, and publishes one
  `imu_sample` event per second over `<alp/rpc.h>`.
- The **Cortex-M55 HE** core stays at the SoM topology default
  (stock-shim Zephyr image) -- alive for future low-power offload,
  not part of this demo's RPMsg channel.

```
examples/rpmsg-aen/
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

## Memory map

The AEN's memory_map currently carries `TBD` placeholders pending
the authoritative HW-config writeup.  Once filled in, the
`alp_default_rpmsg` carve-out lands in `mram_main` (cacheable,
accessible from all three cores).  Spec §6.8 dictates AEN defaults
to cacheable carve-outs because the M55 cores have caches enabled.

| Range                     | Owner                  | Notes                                                |
|---------------------------|------------------------|------------------------------------------------------|
| `mram_main` (TBD base)    | All cores              | On-die MRAM, cacheable.  Holds the RPMsg carve-out.  |
| `sram_main` (TBD base)    | All cores              | On-die SRAM, non-cacheable scratch.                  |

Until the SoM preset carries hard addresses, `west alp-build`
exits with:

```
OrchestratorError: ipc 'alp_default_rpmsg': memory_map.base is TBD
for region 'mram_main' (E1M-AEN701).  Update
metadata/e1m_modules/E1M-AEN701.yaml with hard values.
```

The structural files (this directory) are correct -- only the
metadata is TBD.

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
cd alp-workspace
west alp-build alp-sdk/examples/rpmsg-aen
```

The orchestrator fans out:

- `build/a32_cluster-yocto/` (bitbake against `MACHINE = e1m-aen701-a32`).
- `build/m55_hp-zephyr/` (Zephyr against `BOARD = alp_e1m_aen701_m55_hp`).

Iterate on the M-side only:

```bash
west alp-build alp-sdk/examples/rpmsg-aen --core m55_hp
```

## Reference

- [`examples/rpmsg-v2n/`](../rpmsg-v2n/) -- V2N counterpart of this
  AEN setup.
- [`docs/heterogeneous-builds.md`](../../docs/heterogeneous-builds.md)
  -- per-core build walk-through.
- [`<alp/rpc.h>`](../../include/alp/rpc.h) -- framed RPMsg surface.
