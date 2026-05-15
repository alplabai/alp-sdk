# rpmsg-imx93

> `[UNTESTED]` -- v0.6 structural draft.  Board.yaml + sources are
> shape-correct; the build will fail at carve-out resolution until
> the user supplies authoritative iMX93 memory-map values in
> `metadata/e1m_modules/E1M-NX9101.yaml`.  The metadata is TBD by
> design (project memory note: don't invent HW values); the
> declarative structural files are the contract that lets the
> orchestrator land cleanly the moment those values are filled in.

Heterogeneous compute on **E1M-NX9101** (NXP i.MX 93):

- The 2-core **Cortex-A55 cluster** boots Yocto Linux from eMMC and
  runs the consumer under `linux/`.
- The **Cortex-M33** core boots from OCRAM after the kernel's
  remoteproc driver loads its firmware blob, and publishes a
  `temperature` event per second over `<alp/rpc.h>`.

Same pattern as [`rpmsg-v2n`](../rpmsg-v2n/), but for the NXP i.MX 9
heterogeneous SoC.

```
examples/rpmsg-imx93/
├── board.yaml          (v2; declares a55_cluster + m33 + ipc)
├── README.md           (this file)
├── CMakeLists.txt
├── linux/              (a55_cluster's Yocto slice)
│   ├── CMakeLists.txt
│   └── src/main.c
└── m33/                (m33's Zephyr slice)
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c
```

## Memory map

The iMX93 SoM preset (`metadata/e1m_modules/E1M-NX9101.yaml`)
currently carries:

```yaml
memory_map:
  - { name: ddr_main, base: TBD, size_mib: TBD,
      accessible_from: [a55_cluster, m33], cacheable: true  }
  - { name: ocram,    base: TBD, size_kib: TBD,
      accessible_from: [a55_cluster, m33], cacheable: false }
```

Both regions are reachable from both cores, but the orchestrator
prefers the non-cacheable region (`ocram`) for the default carve-out
because the iMX93's M33 has no cache (spec §6.8).

Until the user supplies real addresses + sizes, `west alp-build`
exits with:

```
OrchestratorError: ipc 'alp_default_rpmsg': memory_map.base is TBD
for region 'ocram' (E1M-NX9101).  Update
metadata/e1m_modules/E1M-NX9101.yaml with authoritative values
before building.
```

## Boot order

| Stage | Core         | Action                                |
|-------|--------------|----------------------------------------|
| 1     | a55_cluster  | U-Boot → Linux kernel from eMMC        |
| 2     | a55_cluster  | systemd reaches basic target           |
| 3     | m33          | remoteproc loads m33.elf into OCRAM    |
| 4     | RPMsg        | Name-service handshake completes       |

## Build

```bash
west alp-build alp-sdk/examples/rpmsg-imx93
```

The orchestrator fans out:

- `build/a55_cluster-yocto/` (bitbake against `MACHINE = e1m-nx9101-a55`).
- `build/m33-zephyr/` (Zephyr against `BOARD = alp_e1m_nx9101_m33`).

## Reference

- [`examples/rpmsg-v2n/`](../rpmsg-v2n/) -- the V2N counterpart.
- [`examples/rpmsg-aen/`](../rpmsg-aen/) -- the AEN counterpart.
- [`docs/heterogeneous-builds.md`](../../docs/heterogeneous-builds.md)
  -- per-core build walk-through.
