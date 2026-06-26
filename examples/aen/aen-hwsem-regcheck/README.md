# aen-hwsem-regcheck -- HWSEM (alif,hwsem) take/give readout

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app binds **HWSEM instance 0** at `hwsem@4902e000` through the
alp-sdk in-tree `alif,hwsem` driver's **take/give** API and proves the
AMP mutual-exclusion latch **acquires and releases** on silicon -- both
in firmware (take -> count nonzero -> give -> count 0) and over J-Link
(`mem32 0x4902E004`, the `REL` register).

## What HWSEM is

The Alif Ensemble **HWSEM** is a hardware semaphore: a bank of **16**
independent atomic request/release latches the two RTSS cores
(**M55-HE** and **M55-HP**) use to lock a shared resource under AMP. It
needs **no** shared-SRAM spin and survives the absence of cache
coherency between the cores. Each latch is keyed by a per-core **MASTER
ID** (the core's CPUID), so a *release* only frees the latch for its
*owner*.

Each instance is just three 32-bit registers:

| Register | Offset | Role |
|----------|--------|------|
| `REQ` | `+0x00` | write `master_id` to **request** (take); readback == granted owner id |
| `REL` | `+0x04` | write `master_id` to **release** (give); read = current count (0 == free) |
| `RST` | `+0x08` | write `1` to **reset** (force-free, recovery only) |

## SINGLE-CORE smoke vs the fuller DUAL-CORE test

This regcheck runs on **one core**. Acquire/release on one core is a
**valid** HWSEM proof: the `REQ` register grants ownership, the `REL`
count increments, and a `REL` write by the owner frees it. The
in-firmware sequence is:

1. read `REL` count -- expect **0** (free);
2. `take` -- expect ownership granted;
3. read `REL` count -- expect **nonzero** (owned);
4. `give` -- release;
5. read `REL` count -- expect **0** (free again).

It does **NOT** exercise cross-core **arbitration**, which is the fuller
test and needs a real second-core peer + a dual-core SES boot:

- **HE** (`master_id = 0x410FD222`) takes the latch.
- **HP** (`master_id = 0x410FD221`) calls `take` against the **same**
  latch and must get **`-EBUSY`** (HE owns it).
- HE `give`s; HP's next `take` must then **succeed**.

That mirrors the dual-core gap already documented for the MHUv2
doorbell (a real HP-core sender + active SESS pairs are required, J-Link
debug-AP writes from one side do not arbitrate the other). The
single-core smoke here is the buildable, single-image proof; the
dual-core arbitration test is **TBD** (needs the dual-core boot).

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| HWSEM0 node | `hwsem0: hwsem@4902e000` `alif,hwsem` | dtsi `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` |
| reg | `0x4902E000` size `0x10` | DFP `Device/soc/AE822FA0E5597/include/rtss_he/soc.h` (`HWSEM0_BASE 0x4902E000`, stride `0x10`) |
| `REQ` (request/take) offset | `+0x00` | DFP `drivers/include/hwsem.h` `struct HWSEM_Type.HWSEM_REQ_REG (@0x00)` |
| `REL` (release/count) offset | `+0x04` -> J-Link `mem32 0x4902E004` | DFP `drivers/include/hwsem.h` `HWSEM_REL_REG (@0x04)` |
| `RST` (reset) offset | `+0x08` (write `0x1`) | DFP `drivers/include/hwsem.h` `HWSEM_RST_REG (@0x08)` + `hwsem_reset()` |
| HE MASTER ID | `0x410FD222` | DFP `Device/soc/AE822FA0E5597/include/rtss_he/core_defines.h` `HWSEM_MASTERID` |
| HP MASTER ID | `0x410FD221` | DFP `Device/soc/AE822FA0E5597/include/rtss_hp/core_defines.h` `HWSEM_MASTERID` |
| Driver | `alif,hwsem`, `DT_DRV_COMPAT alif_hwsem`, no Zephyr class | `zephyr/drivers/misc/alif_hwsem/hwsem_alif.c` |
| Kconfig | `HWSEM_ALIF`, `depends on DT_HAS_ALIF_HWSEM_ENABLED` | `zephyr/Kconfig` |

The register **offsets** and **master ids** were transcribed
**clean-room** (value only, no DFP source copied) from the Alif DFP
HWSEM register header -- **no register value invented**.

## ADR 0017 tier

The Alif HWSEM IP has **no** upstream Zephyr driver, **no** sdk-alif
fork Zephyr driver, and **no** hal_alif library to consume -- only a
bare DFP register header. So it does not fit Tier-1/1.5/2/3 cleanly and
is authored from spec as a last resort (the same posture as the
in-tree `alif,mhuv2-mbox` doorbell driver). **vendor-ext,
BENCH-UNVERIFIED.**

This is the first real `alif,hwsem` binding in alp-sdk: the SDK
previously referenced HWSEM only as an **mproc concept** (the portable
`alp_hwsem_*` surface in `include/alp/mproc.h`, backed by an intra-core
`k_sem` fallback). This driver gives that concept a real per-SoC block;
wiring `alp_hwsem_*` onto it (cross-core) is a follow-on.

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-hwsem-regcheck -d build/aen-hwsem-regcheck -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

The board overlay (`boards/alp_e1m_aen801_m55_he.overlay`) auto-applies
by the fully-qualified board name -- no `DTC_OVERLAY_FILE` needed.

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-hwsem-regcheck/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run. The overlay retargets `zephyr,flash = &itcm` for the RAM-run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x4902E004` (HWSEM `REL`) across the three
   readback windows must read `0` (free) -> nonzero (taken) -> `0`
   (given).
5. `RESULT PASS:` = take/give tracked the count. `RESULT FAIL:` ->
   read `REL` (`0x4902E004`) over J-Link to localise.

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017 vendor-native custom (no upstream/fork/hal driver to consume).
