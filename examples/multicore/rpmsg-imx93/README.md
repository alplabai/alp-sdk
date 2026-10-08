# rpmsg-imx93

> `[UNTESTED]` -- v0.6 structural draft, targeting **E1M-NX9101**
> (NXP i.MX 93), which is **not yet buildable**.  Its only hw_rev,
> `metadata/e1m_modules/imx93/hw-revisions.yaml`'s `r1`, is
> `status: tbd` -- the hw_rev-buildable gate
> ([#1025](https://github.com/alplabai/alp-sdk/issues/1025)) refuses
> it outright, before the orchestrator ever reaches carve-out
> resolution against the iMX93 memory map, which
> `metadata/e1m_modules/E1M-NX9101.yaml` cannot resolve either
> (`silicon_variant: TBD`).  Both are TBD by design
> (project memory note: don't invent HW values) pending real NX9101
> silicon; this example is excluded from the build/emit-snapshot/
> build-plan/system-manifest/parity-oracle gates for the same reason
> (see those gates' own `#1025` comments) but its source and this
> README stay -- the declarative structural files are the contract
> that lets the orchestrator land cleanly the moment a real hw_rev
> status lands.

Heterogeneous compute on **E1M-NX9101** (NXP i.MX 93):

- The 2-core **Cortex-A55 cluster** boots Yocto Linux from eMMC and
  runs the consumer under `linux/`.
- The **Cortex-M33** core boots from OCRAM after the kernel's
  remoteproc driver loads its firmware blob, and publishes a
  `temperature` event per second over `<alp/rpc.h>`.

Same pattern as [`rpmsg-v2n`](../rpmsg-v2n/), but for the NXP i.MX 9
heterogeneous SoC.

```
examples/multicore/rpmsg-imx93/
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
declares no `memory_map:` block at all -- a preset carries one only
for non-stock partitioning.  The stock layout is derived from the SoC
variant resolved via `silicon_variant:`, and that field is still TBD:

```yaml
silicon: nxp:imx9:imx93
silicon_variant: TBD
```

So `resolve_memory_map()` returns an empty region table for this SKU
and the orchestrator has no region to place the carve-out in.  Once
real regions land it will prefer the non-cacheable one for the
default carve-out, because the iMX93's M33 has no cache (spec §6.8).

Today `tan build` never even reaches this carve-out check: #1025's
hw_rev-buildable gate refuses `som.hw_rev: r1`'s `status: tbd` first
(see this file's top note), and exits with:

```
alp_project: SoM E1M-NX9101 hw_rev 'r1' exists but is not buildable
(status: 'tbd').
```

Once imx93 `r1` carries a real, buildable status, resolution reaches
this carve-out check next, and -- until the preset also supplies a
real `mailbox.controller:` and resolvable memory regions -- the rpmsg
entry lands in `system-manifest.yaml` with `status: blocked` and the
first unmet reason:

```
reason: SoM E1M-NX9101 mailbox controller is TBD; carve-out
  resolution requires authoritative mailbox metadata.  Fill
  `mailbox.controller:` in metadata/e1m_modules/E1M-NX9101.yaml with
  the vendor mailbox node name (e.g. `renesas_mhu`, `nxp_mu`,
  `alif_mhuv2`) or remove the rpmsg entries from board.yaml.
```

The slice-build step is what then fails on the blocked carve-out.

## Boot order

| Stage | Core         | Action                                |
|-------|--------------|----------------------------------------|
| 1     | a55_cluster  | U-Boot → Linux kernel from eMMC        |
| 2     | a55_cluster  | systemd reaches basic target           |
| 3     | m33          | remoteproc loads m33.elf into OCRAM    |
| 4     | RPMsg        | Name-service handshake completes       |

## Build

```bash
tan build --project examples/multicore/rpmsg-imx93
```

Tan's relocated planner fans out:

- `build/a55_cluster-yocto/` (bitbake against `MACHINE = e1m-nx9101-a55`).
- `build/m33-zephyr/` (Zephyr against `BOARD = alp_e1m_nx9101_m33`).

## Reference

- [`examples/multicore/rpmsg-v2n/`](../rpmsg-v2n/) -- the V2N counterpart.
- [`examples/multicore/rpmsg-aen/`](../rpmsg-aen/) -- the AEN counterpart.
- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- per-core build walk-through.
