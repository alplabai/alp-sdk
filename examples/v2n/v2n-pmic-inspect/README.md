# v2n-pmic-inspect

Inspector for the V2N SoM's on-module power chips, run from a
Linux/Yocto user-space app on the V2N Cortex-A55 cluster. It is
read-only unless you pass `--write` together with one action flag.

| Address     | Chip                  | What is shown                                                   |
|-------------|-----------------------|-----------------------------------------------------------------|
| 0x25 / 0x26 | ACT88760 (`act8760`)  | Buck1..7 and LDO1..6: net, enable, mV, POK, OV/ILIM; GPIO1..11: net, MODE byte, polarity, MUX, push-pull, level, IRQ mask |
| 0x1E        | DA9292 (`da9292`)     | identity (DEV_ID/REV_ID/CFG_REV), CH1/CH2: enable, active mV, VSTEP/VSEL, PG, UV/OV/OC; latched EVENT_00/01 (peeked, not cleared) |
| 0x44 / 0x48 / 0x4F | TPS628640      | DEEPX-side bucks on V2N-M1: VOUT, CONTROL; skipped when absent (they only answer once `DEEPX_CORE_0P75_EN` is high) |
| 0x4D        | TPS628640             | LPDDR4X 0.6 V buck, assembly option; skipped when absent         |

Rail net names, guard windows, critical flags, the expected DA9292
identity and the expected ACT88760 MODE4 byte all come from the
generated `<alp/chips/v2n_power_tree.h>`
(`scripts/gen_power_tree.py` <- `metadata/e1m_modules/v2n/power-tree.yaml`).
The app does not hard-code a window. It hands the generated tables to the
drivers, and the drivers enforce them.

## Usage

```sh
v2n-pmic-inspect                      # text dump, family v2n-m1 (a superset for reading)
v2n-pmic-inspect --family v2n --json  # machine-readable
v2n-pmic-inspect --include-clear-on-read
```

Exit status:

| Code | Meaning |
|------|---------|
| 0 | Silicon matches the metadata. |
| 1 | Silicon and metadata disagree: a DA9292 identity byte differs, ACT88760 MODE4 is not `0x08`, a power chip is missing, or a present rail's live voltage reads outside its power-tree.yaml window (presence alone is not "matches metadata"). |
| 2 | Usage error, or `/dev/i2c-8` could not be opened. |
| 3 | A write action was refused or failed. |

### Clear-on-read registers

These registers are never read unless you pass `--include-clear-on-read`:

- ACT88760 ADD1 `0x00` (VSYS latches)
- ACT88760 ADD1 `0x04` (GPIO1..8 toggles)
- ACT88760 ADD1 `0x2B` (GPIO9..11 toggles). GPIO9..11 level and IRQ mask are also in this register, so without the flag those columns show `?`.
- TPS628640 `0x05` (STATUS)

`act8760_init()` reads `0x00` as its presence probe, so the VSYS latches are consumed even without the flag. The DA9292 EVENT registers are write-1-to-clear, so they are always peeked.

### Kernel-owned addresses

Every address is first checked with an `I2C_SLAVE`-based probe. If a
kernel driver is bound to it (`EBUSY`), it is reported as
`kernel-owned` and never touched.
[`v2n-brd-i2c-bringup`](../v2n-brd-i2c-bringup/) does the same check.

## Write actions

Every write needs three things:

- `--write`
- exactly one action flag
- an explicit `--family v2n|v2n-m1`, which selects the guard tables

The app also refuses to write when silicon disagrees with the metadata.
The one exception is the GPIO4 defect, when `--fix-gpio4-polarity` is the action.

| Action | What it does |
|--------|--------------|
| `--set-mv <rail> <mV>` | Programs a setpoint. The driver rounds down to the chip's grid in the rail's live range and refuses (`ALP_ERR_OUT_OF_RANGE`) unless the encoded value is inside the rail's window. |
| `--enable <rail>` / `--disable <rail>` | Changes the enable bit. Disabling a critical rail (every ACT88760 rail, DA9292 CH1, TPS628640 0x4D) always returns `ALP_ERR_NOSUPPORT`. `--disable` of any DA9292 / TPS628640 rail is refused by the app: DEEPX rails only go down in order, through the `--deepx-rail-sequence` shutdown. |
| `--fix-gpio4-polarity` | Acts only when MODE4 reads exactly `0x88` (inverted OTP; it holds the GD32 in reset). Writes `0x08` and reads it back. If MODE4 already reads `0x08` it prints a no-op message. Any other value is refused. The fix is volatile: repeat it after every power cycle. |
| `--deepx-rail-sequence` | V2N-M1 only. Runs `da9292_ch2_sequence()` with P65 `DEEPX_PWR_EN_REQ` as the input and P64 `DEEPX_CORE_0P75_EN` as the output, both through `/dev/gpiochip*`. `M1_RESET` is not touched. |

`<rail>` is an id (`buck1`..`buck7`, `ldo1`..`ldo6`, `da9292.ch1`,
`da9292.ch2`, a TPS628640 net) or a net name such as `VDD09_CA55`. Buck5/
Buck6's own net is `TBD` (channel<->net attribution not independently
verified, see `power-tree.yaml`) -- use their `buck5`/`buck6` id instead.

### `--deepx-rail-sequence` safety rules

- **It refuses when CH2 is already enabled.** That state means the boot owner has already run the sequence. A new line request starts as an input, so claiming P64 would drop `DEEPX_CORE_0P75_EN` under a live DEEPX.
- **It refuses when P64 or P65 is held by another consumer, or when P64 is already an output.**
- **Lines are found by pin controller.** The app uses the pinctrl gpiochip (override it with `--gpiochip N`) and the assumed `pinctrl-rzg2l` numbering `port*8+pin`, so P64 = 52 and P65 = 53. It refuses unless the kernel names those lines `P64`/`P6_4` and `P65`/`P6_5`.
- **The process stays running while the rail is up.** If the sequence succeeds, it keeps holding the lines, because the RZ pin controller returns a released line to input. SIGINT/SIGTERM/SIGHUP powers the rail down in order: P64 goes low first, then CH2_EN is cleared. Killing the process uncleanly (SIGKILL) releases P64, which then floats under a live CH2.

**Boot mode.** The boot CPU is a hardware strap: pin `BOOTSELCPU`
(RZ/V2N HW manual R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table 1.9-1) selects
LOW = CM33 cold boot, HIGH = CA55 cold boot, driven by ACT88760 GPIO5
(`V2N_BOOT_CPU_SEL` -- the CMI drives it HIGH by default, ~8.6 ms after
`MODULE_EN`, so A55-boot is the power-on default). In A55-boot mode,
U-Boot owns the DEEPX sequence and CA55/Linux is the sole BRD_I2C
(RIIC8) master. In CM33-boot mode the CM33 masters RIIC8 and owns the
sequence itself, time-sliced BEFORE it releases the CA55 -- see
`examples/v2n/v2n-cm33-deepx-rail`
(`boot_modes:` in `metadata/e1m_modules/v2n/power-tree.yaml`,
`boot_mode_core` in `metadata/e1m_modules/v2n/core-ownership.yaml`).
This app itself still only reads GPIO5's level for display; it does not
gate on it.

Nothing here has been run on silicon yet.

## Build (Yocto SDK)

```sh
# Source the SDK that includes meta-alp-sdk (libalp_sdk.so + libalp_chips.a
# with the act8760, da9292 and tps628640 PACKAGECONFIGs, all on by default):
. /opt/poky/<ver>/environment-setup-aarch64-poky-linux

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

Copy `build/v2n-pmic-inspect` to the target and run it as a user that can
open `/dev/i2c-8` (and `/dev/gpiochip*` for the DEEPX action).
