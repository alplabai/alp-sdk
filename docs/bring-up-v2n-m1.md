# Bench bring-up — E1M-X V2N-M1 delta

Picks up where [`bring-up-v2n.md`](bring-up-v2n.md) ends.  Covers
the **delta** introduced by the V2N-M1 variant: three DEEPX-rail
PMIC instances on `BRD_I2C`, the `M1_RESET` line, the two passive
PCIe muxes, and the DEEPX kernel runtime hand-off.

> Confirm V2N base bring-up is clean before reading this doc —
> rail debugging the DEEPX path on a V2N that isn't itself
> healthy is a recipe for blaming the wrong thing.

## What's different

| Aspect                       | V2N base                                      | V2N-M1                                                                              |
|------------------------------|-----------------------------------------------|-------------------------------------------------------------------------------------|
| DEEPX silicon                | absent                                        | populated (DX-M1 BGA, on-module)                                                    |
| DA9292 CH2                   | disabled (0.75 V DEEPX rail unused)            | sequenced to 0.75 V + confirmed power-good by U-Boot's `board_late_init()` before Linux or the CM33 image ever starts |
| TPS628640 instances on BRD_I2C | 1 optional (LPD4x_0V6 @ 0x4D)                | 4 total (adds `deepx_lpddr_0v85` @ 0x48 / 0x44 / 0x4F for DEEPX rails, all bench-confirmed) |
| PCIe muxes                   | not applicable                                | 2 × PI3DBS12212A; PD on Renesas P80, SEL on P95                                     |
| `M1_RESET` line              | not applicable                                | Renesas PA6 -- driven by host firmware via `chips/deepx_dxm1/`                      |
| DEEPX kernel runtime         | not applicable                                | `dx_rt_npu_linux_driver` + `libdxrt.so` from upstream `meta-deepx-m1` Yocto layer  |

## Step-by-step

### 1. Confirm DEEPX rail PMICs ACK on BRD_I2C

```c
tps628640_t t44, tlpddr, t4f;
tps628640_init(&t44, brd_i2c, 0x44, 1050);       /* DDR5_VDD       */
/* deepx_lpddr_0v85 is bench-confirmed at 0x48 (#1163, #1845) -- see
 * docs/soms/v2n-m1.md's "deepx_lpddr_0v85 strap is resolved" section.
 * Only ACKs after U-Boot's step 2 drives P64 (DEEPX_CORE_0P75_EN)
 * high. */
tps628640_init(&tlpddr, brd_i2c, 0x48, 850);      /* VDD0V85_LPDDR  */
tps628640_init(&t4f, brd_i2c, 0x4F, 500);        /* DDR5_VDDQ_0V5  */
```

Each `_init` must return `ALP_OK` (NOT `ALP_ERR_NOT_READY`).  All
three rails self-regulate to their factory OTP voltages with no
host writes -- firmware just confirms the parts are populated.

If any one returns `ALP_ERR_NOT_READY`: probe the rail directly.
A missing population shows up as "buck instance not on the bus";
a populated buck that won't ACK is either powered down (check
EN line) or address-strapped wrong.

### 2. DA9292 DEEPX rail (CH2) -- owned by U-Boot, nothing to do here

**U-Boot performs this step outside its `cm33_boot` pre-handoff window; no
firmware you write needs to.**  In `a55_boot` mode (and in `a55_boot`'s
share of `cm33_boot` mode, after the CM33 hands the CA55 off), before
this DEEPX step the same `board_late_init()` also runs the on-module
clock-generator fixup, unconditionally on every boot of any V2N/V2M
SKU (not just V2N-M1) --
`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0007-rzv2n-dev-ALP-E1M-clkgen-otp-fixup.patch`,
logged as `ALP: 5L35023B clock: ...` ahead of the `ALP: DA9292 ...` /
`ALP: DEEPX rail ...` lines below. See [`docs/soms/v2n.md`'s "On-module
clock-generator fixup"](soms/v2n.md#on-module-clock-generator-fixup)
for what it does; it is unrelated to the DEEPX sequencing and does not
gate it.

U-Boot's `board_late_init()`
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
sequences CH2 to 0.75 V and confirms power-good over RIIC8/BRD_I2C
BEFORE Linux or the CM33 image ever starts, and only then releases
`M1_RESET` (step 3 below).  RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
in `a55_boot` mode
(`metadata/e1m_modules/v2n/core-ownership.yaml`) -- but ownership is
TIME-SLICED, not a blanket exclusion: in `cm33_boot` mode the CM33 masters
RIIC8 first and runs this same sequence itself
(`examples/v2n/v2n-cm33-deepx-rail`, see below) before it releases the
CA55, at which point U-Boot's copy of the sequence runs again as a
warm/idempotent verify, not a re-sequence. "No CM33 code path" was true
before #2045 and is stale now.

At the U-Boot prompt (or in its serial log) you should see:

```
ALP: DA9292 programmed CTRL_01=0x01 VOUT_CH2=0x96/0x96
ALP: DEEPX rail 0.75V up (PG)
```

(`CTRL_01=0x01` is CH1_EN=1 / CH2_EN=0 at the end of the program
phase -- CH2_EN is only set afterward, in the enable phase, once P65
is confirmed high.)

If instead you see `ALP: DA9292 ...` followed by `abort` or `DEEPX
rail not enabled` / `DEEPX rail disabled`, the rail failed to
sequence -- read the printed register bytes (they name exactly which
check failed: DEV_ID mismatch, STATUS_01 not clean, a CTRL_01/VOUT_CH2
readback mismatch, or CH2 not reaching power-good) and `M1_RESET`
stays asserted, so nothing past this point in the guide will work
until it's fixed.

**If the line reads `... DEEPX rail not enabled (CH2 may require
EN2/P64 high before PG -- see bring-up doc)`:** CH2_EN was written
over I2C and PG never asserted within the 20 ms poll. On this DA9292
wiring, EN2 (the CH2 hardware-enable
pin) is tied to P64 (`DEEPX_CORE_0P75_EN`) -- if EN2 gates CH2 the
same way EN1 gates CH1, the register-side `CH2_EN=1` alone is not
enough to bring CH2 up; P64 has to go high too. The sequence already
drives P64 high in step 10, but only *after* the PG poll in step 8 --
so on EN2-gated silicon, step 8 will always see PG=0 and abort before
step 10 ever runs. This is a real hardware-wiring question this patch
does not resolve on its own: if you see this message on real
silicon, confirm with a scope whether CH2 tracks P64 or the I2C
`CH2_EN` bit, and file a follow-up against #2045 rather than assuming
a rail fault.

The same sequence exists as an OS-agnostic driver function,
`da9292_ch2_sequence()` in `chips/da9292/` (caller-opened GPIOs + a delay
callback; it also checks CH2 OV / OC after P64 goes high, because the OTP
masks OV out of PG).  **CM33-boot mode now runs it too, time-sliced against
the CA55** (`examples/v2n/v2n-cm33-deepx-rail`): the boot-CPU choice is a
hardware strap, not a software decision -- RZ/V2N HW manual
R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table 1.9-1, pin `BOOTSELCPU`
(LOW = CM33 cold boot, HIGH = CA55 cold boot, driven by ACT88760 GPIO5 net
`V2N_BOOT_CPU_SEL`, which the CMI drives HIGH by default ~8.6 ms after
`MODULE_EN`).  CM33-cold-boot supports only xSPI/SCIF download boot
sources and the CM33 always boots first and releases the CA55 later, so
ownership is time-sliced, not concurrent: in `cm33_boot` mode the CM33
masters RIIC8 and drives P64/P65 UNTIL it releases the CA55; the A55/Linux
takes over exclusively after that (and for the whole of `a55_boot` mode),
same as before.  `boot_modes:` in `metadata/e1m_modules/v2n/power-tree.yaml`
records `cm33_boot: bus_master: cm33, deepx_sequence_owner: cm33` plus a
`handover` note; `core-ownership.yaml`'s new `boot_mode_core` qualifier on
RIIC8_SCL8/SDA8 and P64/P65 backs it per boot mode, and
`gen_power_tree.py`'s `cross_check()` (reached from `validate_metadata.py`)
still rejects any boot mode naming `cm33` that qualifier doesn't back --
a real dual-master config still hard-fails.  Releasing the CA55 itself is
NOT yet implemented (no confirmed RZ/V2N CPU-reset-control register) --
tracked in alp-sdk#2289.

The DA9292-AROVx OTP enables the EN2 / VSEL2 pin functions (PMC_CFG_00
`0x0E` = `0xFF`; `da9292_ch2_sequence()` reports it in `res.pmc_cfg_00`).
Anything that drives the EN2 pin before the sequence finishes brings CH2 up
at the OTP default (VSTEP=1, 1.80 V) on the 0.75 V DEEPX core.  Resolve the
EN2 / VSEL2 nets before running a bench build that touches them.  A bench diagnostic app may use the read-only
calls (`da9292_get_status()`, `da9292_get_channel_state()`,
`da9292_peek_events()`) at any time; every control write is refused unless a
limits table is installed with `da9292_set_limits()`.

### 3. Sequence M1_RESET + PCIe muxes

**Also already done by U-Boot** (same patch as step 2, right after the
rail step above succeeds) by the time this guide's steps 1-2 finish --
the C snippet below is the driver-level illustration of what
`alp_deepx_pcie_bringup()` in that patch does with raw register pokes,
useful for understanding the sequence or reusing the chip driver on a
platform where a portable caller genuinely owns `M1_RESET`, but not
something you run again on V2N-M1's CM33 (which never releases
`M1_RESET` in normal boot flow):

```c
alp_gpio_t *pd  = alp_gpio_open(/* pin_id for P80 */);
alp_gpio_t *sel = alp_gpio_open(/* pin_id for P95 */);
alp_gpio_t *rst = alp_gpio_open(/* pin_id for PA6 */);

alp_gpio_configure(pd,  ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
alp_gpio_configure(sel, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
alp_gpio_configure(rst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

pi3dbs12212_t mux;
pi3dbs12212_init(&mux, pd, sel);

deepx_dxm1_t dxm1;
deepx_dxm1_init(&dxm1, rst, &mux, /* deepx_path */ PI3DBS_STATE_PATH_0);
deepx_dxm1_bring_up(&dxm1, DEEPX_DXM1_DEFAULT_BOOT_US);
```

The sequencer routes the muxes to the DEEPX path, then releases
`M1_RESET`.  After `boot_us` elapses, the DEEPX silicon's internal
ROM has executed and PCIe link training can start.

> **`M1_RESET` polarity is ACTIVE-LOW on V2N-M1** -- this is the
> driver's default, so no `_set_reset_polarity` call is required.
> Boards on different DEEPX revisions can flip the
> polarity via `deepx_dxm1_set_reset_polarity` if the silicon's
> reset polarity ever changes.

### 4. Hand off to Linux

If the SoM is running Yocto with the `meta-deepx-m1` layer wired
into `e1m-v2m101-a55.conf`:

* `dx_rt_npu_linux_driver` opens the PCIe device at `lspci`-time.
* `dxrt_init()` from user-space succeeds; load a `.dxnn` model
  and run inferences.

If the kernel comes up but `dxrt_init()` returns an error, see the
upstream DEEPX troubleshooting docs at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).

## Bring-up regression checks

After every change in the bring-up flow, re-run these in order:

1. Power-on idle current under 500 mA (DEEPX adds ~150 mA static).
2. ACT88760 + DA9292 status: no `thermal_warning`, no event latches.
3. DA9292 CH2 in regulation: `da9292_get_status().ch2_pg == true`.
4. Three DEEPX TPS628640 instances ACK at their addresses.
5. `lspci` lists the DEEPX device.
6. `dxrt_init()` returns success.
7. A reference `dx_app` inference runs to completion.

## Common gotchas

* **DEEPX rails come up but PCIe link never trains.**  Almost
  certainly an `M1_RESET` polarity mismatch.  Toggle the polarity
  flag in [`<alp/chips/deepx_dxm1.h>`](../include/alp/chips/deepx_dxm1.h)
  and retest.

* **PCIe link trains but the kernel driver reports BAR errors.**
  The muxes may be on the wrong path (E1M edge instead of DEEPX);
  check `PI3DBS_STATE_PATH_0` matches the board's silk-screen.

* **U-Boot logs `ALP: DEEPX rail 0.75V up (PG)` but DEEPX silicon is
  flaky under load.**  Check the three TPS628640 rails -- the
  factory OTP voltages assume a specific load envelope.  Verify
  with a scope, then retune with `tps628640_set_voltage_mv()`.
  The VSET decode it needs is implemented (TI SLVSEI1C): the
  helper range-checks against `TPS628640_VOUT_BASE_MV` (400 mV)
  and `TPS628640_VOUT_MAX_MV` (1675 mV), encodes
  `(mv - 400) / 5` and writes `TPS628640_REG_VOUT1`.  Nothing
  in-tree calls it at boot, so the four instances still come up
  on their factory OTP voltages -- program them from the
  bring-up path if a rail needs to differ.

## See also

* [`vendors/deepx-dxm1/README.md`](../vendors/deepx-dxm1/README.md) --
  upstream cross-link + Yocto integration.
* `metadata/e1m_modules/v2n-m1/README.md` --
  authoritative V2N-M1 pinout delta.
