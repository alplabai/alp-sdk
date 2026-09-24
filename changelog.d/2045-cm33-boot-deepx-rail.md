### Added — CM33-boot mode sequences the DEEPX 0.75 V rail itself, time-sliced against the CA55 (#2045)

`metadata/e1m_modules/v2n/power-tree.yaml`'s `cm33_boot` boot mode was
`status: blocked`: the 2026-09-24 decision that made CA55/Linux the sole
RIIC8/BRD_I2C master left no owner for a CM33-cold-boot session at all, so
`da9292_ch2_sequence()` — already OS-agnostic and Zephyr-buildable — had no
caller on that core, and `examples/v2n/v2n-m1-deepx-inference`'s claim that
the rail "happens automatically on SYS_INIT" was simply false on
`alp_e1m_v2m101_m33_sm` (#2045's original report).

New hardware facts settle it: RZ/V2N HW manual R01UH1071EJ0110 Rev.1.10
Sec.1.9 Table 1.9-1 — pin `BOOTSELCPU` selects **LOW = CM33
cold boot, HIGH = CA55 cold boot** (internal pull-down), driven by ACT88760
GPIO5 (net `V2N_BOOT_CPU_SEL`; the CMI drives it HIGH by default, ~8.6 ms
after `MODULE_EN`, so CA55-cold-boot is the power-on default) and also
routed to the E1 connector as an external override net.
CM33-cold-boot supports only xSPI or SCIF download boot sources, and the
CM33 always boots first and releases the CA55 later. So ownership of
RIIC8/BRD_I2C and P64 (`DEEPX_CORE_0P75_EN`)/P65 (`DEEPX_PWR_EN_REQ`) is
**time-sliced, never concurrent**: in `cm33_boot` mode the CM33 masters the
bus and runs the DEEPX sequence UNTIL it releases the CA55; after that (and
for the whole of `a55_boot` mode) the A55/Linux is the sole master, exactly
as before. U-Boot 0004
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
is already idempotent on its warm path (CH2 already at 0.75 V / VSTEP=0
skips the program phase, zero `CTRL_01` writes), so it runs safely again on
the A55 in `cm33_boot` mode as a verify, not a re-sequence.

`metadata/e1m_modules/v2n/power-tree.yaml`: `boot_modes.cm33_boot` drops
`status: blocked` — `bus_master: cm33` and `deepx_sequence_owner: cm33`,
plus a new free-text `handover` field describing the CA55-release
boundary. The `vdd_0p75` rail's `owner.cm33_boot` changes `none` -> `cm33`.
The comment block documents the BOOTSELCPU facts above.

`metadata/schemas/power-tree-v1.schema.json`: `$defs.boot_mode` gains the
optional `handover` string property.

`metadata/e1m_modules/v2n/core-ownership.yaml`: RIIC8_SCL8, RIIC8_SDA8,
DEEPX_CORE_0P75_EN and DEEPX_PWR_EN_REQ each gain an optional
`boot_mode_core: { a55_boot: "a55", cm33_boot: "m33" }` qualifier. The flat
`core: "a55"` on all four rows is UNCHANGED — it stays the steady-state
value `scripts/gen_pinmux_capability.py` projects into
`metadata/pinmux/v2n.yaml` and `scripts/check_amp_pad_claims.py` checks a
Linux devicetree claim against, since Linux never runs concurrently with
the CM33's pre-handoff window. `boot_mode_core` is read only by
`scripts/gen_power_tree.py`'s `cross_check()` (`_check_boot_modes()`,
via a new `_core_for()` helper), which still rejects a boot mode naming
`cm33` as bus master or DEEPX owner unless that mode's qualifier (or, for
an unqualified row, the flat `core`) actually says `"m33"` — a real
dual-master (non-time-sliced) config still hard-fails
(`tests/scripts/test_gen_power_tree.py`:
`test_cm33_bus_master_needs_boot_mode_core_backing`,
`test_cm33_bus_master_with_a55_only_boot_mode_core_fails`).

New example `examples/v2n/v2n-cm33-deepx-rail` (V2M101, `m33_sm`): opens
BRD_I2C (`alp_i2c_open`, bus 0) and P64/P65 through its own 2-entry
`alp,pin-array` (these are SoM-internal pads, not E1M edge pins), then
calls `da9292_init()` / `da9292_set_limits()` /
`da9292_ch2_sequence()` — the SAME portable chip-driver function U-Boot
0004 mirrors — with a delay callback wrapping the portable `alp_delay_us()`.
Its board overlay
(`boards/alp_e1m_v2m101_m33_sm_r9a09g056n48gbg_cm33.overlay`) re-enables
`&i2c8` and defines the app's own `alp-pins` node — scoped to THIS app
only; the generated default AMP board files (`alp_e1m_v2n101_m33_sm` /
`alp_e1m_v2m101_m33_sm`) are untouched and still leave `&i2c8` disabled
for every other CM33 app. The overlay does NOT just restore the
pre-2026-09-24 board file's Fast-mode config verbatim: it also carries the
two RIIC8/BRD_I2C fixes U-Boot's own `board_late_init()` needed on this
same bus (`0006-rzv2n-dev-i2c-rzg2l_riic-p06-p07-pullup-clock-fix.patch`,
see `changelog.d/2045-v2m-deepx-rail-uboot.md`) — the SoC-internal pull-up
on P06/P07 (`bias-pull-up`, on top of the external pull-ups already fitted
on this heavily-loaded bus) and Standard-mode `clock-frequency`
(`I2C_BITRATE_STANDARD`, not the pre-move board file's Fast mode) — since
`alp_i2c_open()` on Zephyr calls `i2c_configure()` at runtime, the app's
own bitrate config (`ALP_I2C_CONFIG_DEFAULT`'s 100 kHz) is left as-is too,
so it does not silently override the overlay's Standard-mode default back
to Fast. Twister: `native_sim/native/64`, `build_only: true` (same pattern
as every other V2N CM33 example — native_sim has no RZ/V2N RIIC8 or this
app's overlay).

The example deliberately stops after a successful rail-up without
releasing the CA55: that needs the actual RZ/V2N CPU-reset-control
register(s), not confirmed against the hardware manual or bench-verified.
Tracked as a follow-up in
[alp-sdk#2289](https://github.com/alplabai/alp-sdk/issues/2289).

`meta-alp-sdk/recipes-bsp/u-boot/u-boot_%.bbappend`: documents that 0004's
warm path (skip-on-already-programmed) makes it safe to run again in
`cm33_boot` mode's A55 tail, with a follow-up note that 0004 should read
the boot-CPU state and log verify-only mode explicitly instead of relying
on the warm path's silence (U-Boot itself is not rebuilt in this change).

`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`: the
BRD_I2C pull-up comment now records BOTH pull-up sources — external
2.2 kOhm pull-ups to `VDD1G_1P8` (per the SoM netlist) ARE fitted, and the
SoC-internal pull-up is additionally enabled because of the heavily-loaded
bus (13 branches).

Docs: `docs/bring-up-v2n-m1.md`, `docs/soms/v2n-m1.md`, `docs/soms/v2n.md`
and `examples/v2n/v2n-pmic-inspect/README.md` no longer say `cm33_boot` is
blocked or that the boot-CPU-select GPIO5 level is unrecorded.
