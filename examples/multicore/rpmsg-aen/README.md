# rpmsg-aen

> `[UNTESTED]` -- v0.6 structural draft.  Board.yaml + sources are
> shape-correct.  AEN801 (the default) resolves its RPMsg carve-out
> from sram0 (0x02000000); the full west build is pending silicon
> bring-up of the MHUv2 mailbox driver.  AEN701 remains blocked on
> its `mailbox.controller: TBD` in the SoM preset.

Heterogeneous compute on **E1M-AEN801** (Alif Ensemble E8):

- The 2-core **Cortex-A32 cluster** boots Yocto Linux from MRAM and
  runs the consumer under `linux/`.
- The **Cortex-M55 HP** core boots from MRAM, reads the board's
  on-board BMI323 IMU + BMP581 barometer, and publishes one
  `imu_sample` event per second over `<alp/rpc.h>`.
- The **Cortex-M55 HE** core stays at the SoM topology default
  (stock-shim Zephyr image) -- alive for future low-power offload,
  not part of this demo's RPMsg channel.

```
examples/multicore/rpmsg-aen/
├── board.yaml          (AEN801 default; declares a32_cluster + m55_hp + ipc)
├── board-aen701.yaml   (AEN701 alternate; blocked until mailbox lands)
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

## Board SKUs

| File                | SoM SKU      | Silicon | Status                              |
|---------------------|--------------|---------|-------------------------------------|
| `board.yaml`        | E1M-AEN801   | E8      | Default; carve-out resolves         |
| `board-aen701.yaml` | E1M-AEN701   | E7      | Alternate; blocked (mailbox TBD)    |

`linux/CMakeLists.txt` and `m55_hp/CMakeLists.txt` both hardcode `../board.yaml`,
so the default build always targets AEN801.  To build for the AEN701 alternate
(once its mailbox metadata is filled in), pass `--input board-aen701.yaml`
explicitly to `west alp-build`.

> **Known issue:** the M55-HP producer source comment references `LSM6DSO`, but
> the `e1m-evk` preset populates `bmi323` instead.  This is a pre-existing draft
> mismatch in the source comment -- the code is correct; only the comment is
> stale.  Board-gated, out of SP3 scope; flagged for a follow-up cleanup.

## Memory map

The AEN801 resolves its `alp_default_rpmsg` carve-out from sram0
(0x02000000, 4096 KiB, accessible from all three cores, cacheable).
The 256 KiB carve-out is placed at 0x023c0000 by the top-down allocator.

| Range                        | Owner      | Notes                                        |
|------------------------------|------------|----------------------------------------------|
| `sram0` (0x02000000, 4 MiB)  | All cores  | On-die SRAM, cacheable.  Holds RPMsg carve-out. |
| `mram_main` (base TBD)       | All cores  | On-die MRAM; base not yet grounded in e8.json. |

For AEN701, the `alp_default_rpmsg` carve-out is blocked because
`mailbox.controller` in `metadata/e1m_modules/E1M-AEN701.yaml` is still `TBD`.
Once that field is filled with the authoritative Zephyr binding name (expected:
`alif_mhuv2`), the carve-out will resolve on the next orchestrator run.

## Boot order

AEN boots the M55-HP core first (out of reset from MRAM).  The
A32 cluster comes up second, brought online by a small bootloader
running on M55-HP.  RPMsg name-service handshake completes once
the A32 has reached the Linux user-space stage and opened its
side of `alp_default_rpmsg`.

| Stage | Core         | Action                               |
|-------|--------------|--------------------------------------|
| 1     | m55_hp       | Reset, run Zephyr early-boot         |
| 2     | m55_he       | Stock-shim Zephyr (idle wait)        |
| 3     | a32_cluster  | M55-HP-driven A32 bootloader → Linux |
| 4     | RPMsg        | Name-service handshake on both sides |

## Build

Default (AEN801):

```bash
cd alp-workspace
west alp-build alp-sdk/examples/multicore/rpmsg-aen
```

The orchestrator fans out:

- `build/a32_cluster-yocto/` (bitbake against `MACHINE = e1m-aen801-a32`).
- `build/m55_hp-zephyr/` (Zephyr against `BOARD = alp_e1m_aen801_m55_hp`).

Iterate on the M-side only:

```bash
west alp-build alp-sdk/examples/multicore/rpmsg-aen --core m55_hp
```

AEN701 alternate (blocked until mailbox metadata is filled in):

```bash
west alp-build alp-sdk/examples/multicore/rpmsg-aen --input board-aen701.yaml
```

### Manual M55-HP build (without the orchestrator)

The M55-HP slice can be built standalone using `west` from a Zephyr 4.4+
workspace with the Zephyr SDK (`arm-zephyr-eabi`) installed:

```bash
cd ~/zephyrproject          # west workspace containing zephyr 4.4+
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
EXTRA_ZEPHYR_MODULES=/path/to/alp-sdk \
west build -b 'alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp' \
  /path/to/alp-sdk/examples/multicore/rpmsg-aen/m55_hp -p always
```

The board qualifier uses Zephyr 4.x slash format (`board/soc/variant`).  The
build emits orphan-section warnings for `alp_backends_*`; these are benign --
GNU ld auto-emits `__start_`/`__stop_` bracket symbols for the C-identifier-
named sections, so all backends register correctly.  See
`meta-alp-sdk/recipes-firmware/aen-m55-hp-fw/aen-m55-hp-fw_0.6.bb` for the
recipe that packages the resulting ELF for remoteproc.

## Reference

- [`examples/multicore/rpmsg-v2n/`](../rpmsg-v2n/) -- V2N counterpart of this
  AEN setup.
- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- per-core build walk-through.
- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- framed RPMsg surface.
