# Troubleshooting

Common issues firmware engineers hit when working with the SDK,
organised by where the symptom shows up.  Each entry includes the
exact error text and a fix.

> **Didn't find your issue here?**  Ask on
> [**community.alplab.ai**](https://community.alplab.ai/) — most
> questions get answered there before they become tracked issues,
> and the community search often surfaces a peer who hit the same
> thing first.  If you have a concrete reproducer, file a bug
> directly on
> [github.com/alplabai/alp-sdk/issues](https://github.com/alplabai/alp-sdk/issues).

## Build-time errors

### `alp_project: no preset for SKU <X> at ...`

The `som.sku` in your `board.yaml` doesn't match a per-SKU manifest.
Confirm one of the released SKUs (see [`metadata/e1m_modules/`](../metadata/e1m_modules/)
for the catalogue):

```yaml
som:
  sku: E1M-V2N101    # or E1M-AEN801, E1M-V2M101, E1M-NX9101, etc.
```

### `alp_project: schema violation at <loc>: ...`

The validator caught a `board.yaml` problem.  The error message
names the JSON-pointer location (e.g. `peripherals/2` for the
third entry).  Common slip-ups:

* `cores.<id>.peripherals:` entries must be lowercase strings from
  the allowed set (`adc`, `can`, `counter`, `emmc`, `ethernet`,
  `flash`, `gpio`, `i2c`, `i2s`, `pwm`, `rtc`, `sensor`, `spi`,
  `uart`, `usb`, `watchdog`).  Note: under v2 `peripherals:` lives
  per-core under `cores.<id>:`, not at top level; and `watchdog`
  (not `wdt`) is the canonical name.
* `som.sku` follows `E1M-{AEN,V2N,V2M,NX9}\d{3}` (case-sensitive).
* the `preset:` (or use an inline board) must point at a preset under
  `metadata/boards/<name>.yaml` OR carry an inline
  `board.populated:` block.

Full schema reference: [`docs/board-config-schema.md`](board-config-schema.md).

### `west: command not found` / `pip install west` fails

Installing and running `west` itself needs Python 3.10+ (the SDK's own
support floor).  On macOS:

```bash
brew install python
pip3 install --user west
export PATH="$HOME/.local/bin:$PATH"
```

On Windows PowerShell:

```powershell
winget install -e --id Python.Python.3.12
pip install west
```

See [`docs/getting-started.md`](getting-started.md) §1 for the
full per-host walkthrough.

### `west build` fails at CMake configure citing a Python version

`west` itself runs fine on 3.10+, but *building* needs more: Zephyr
v4.4.1's own `cmake/modules/python.cmake` hardcodes
`PYTHON_MINIMUM_REQUIRED 3.12` and refuses `find_package(Python3)`
below it, regardless of what `west`/`bootstrap.sh`/`bootstrap.ps1`
accepted earlier.  Ubuntu 22.04 and Debian 12's system `python3`
(3.10 and 3.11 respectively) can't reach 3.12 via `apt-get install
python3` -- install a newer interpreter alongside the system one,
e.g. the `deadsnakes` PPA (`sudo add-apt-repository
ppa:deadsnakes/ppa && sudo apt-get install python3.12`) or `pyenv`,
and point `west`/the venv at it.  See
[`docs/cross-platform-setup.md`](cross-platform-setup.md) §1.1 for how Python
Tan enforces the effective floor during bootstrap and doctor.

### `CMake Error: Could not find package configuration file Zephyr`

Either `ZEPHYR_BASE` is not set, or your workspace is malformed.

```bash
cd alp-workspace
export ZEPHYR_BASE="$PWD/zephyr"
# OR:
west zephyr-export
```

### `CMake Error: ... You probably need to select a different build tool` (missing `ninja`)

```
CMake Error: CMake was unable to find a build program corresponding to "Ninja".  CMAKE_MAKE_PROGRAM is not set.  You probably need to select a different build tool.
CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
```

Despite what the first line suggests, the fix is not to pick a
different build tool -- `ninja` is Zephyr's build generator on every
host.  `scripts/bootstrap.sh` / `bootstrap.ps1` and `tan doctor`
both check for it and FAIL with an install command when it's
missing; if you hit the raw CMake error above instead, check whether
you resolved the `hostPrerequisites` finding from `tan doctor`. Installing it
clears all three lines above (the
two compiler errors are downstream of the same missing generator):

```bash
# Linux (Debian / Ubuntu)
sudo apt install -y ninja-build

# macOS
brew install ninja

# Windows (PowerShell)
winget install -e --id Ninja-build.Ninja
```

See [`docs/cross-platform-setup.md`](cross-platform-setup.md) §2.1 /
§3.2 / §4.1 for the base-toolchain block this belongs to.

### Compile error: `'alp_<thing>_t' undeclared`

You haven't included the right header.  Check the chip's manifest
at `metadata/chips/<chip>.yaml` for the canonical header path:

```yaml
# metadata/chips/lsm6dso.yaml
kconfig:
  zephyr: ALP_SDK_CHIP_LSM6DSO
```

Then include the matching public header:

```c
#include <alp/chips/lsm6dso.h>
```

If the Kconfig symbol isn't set (`ALP_SDK_CHIP_<chip>=y`), the
driver source isn't compiled in.  Add the chip name to your
`board.yaml`:

```yaml
chips:
  - lsm6dso
```

## Runtime errors (return codes)

The SDK returns negative `alp_status_t` values; positive 0 is
success.  Decode tips:

| Return                  | Meaning                                                            | First thing to check                                  |
|-------------------------|--------------------------------------------------------------------|-------------------------------------------------------|
| `ALP_OK` (0)            | Success.                                                           | —                                                     |
| `ALP_ERR_INVAL` (-1)    | Invalid argument (NULL pointer, out-of-range value).               | Function args + caller's input validation.            |
| `ALP_ERR_NOT_READY` (-2)| Peripheral not initialised or chip not ACKing.                     | Was `_open` / `_init` called?  Bus / address correct? |
| `ALP_ERR_BUSY` (-3)     | Peripheral busy.                                                   | Concurrent access?  DMA still running?                |
| `ALP_ERR_TIMEOUT` (-4)  | Transfer timed out.                                                | Slave not responding -- physical wiring?              |
| `ALP_ERR_IO` (-5)       | Bus / line error.                                                  | CRC mismatch (GD32 bridge), I2C NACK, SPI mode wrong. |
| `ALP_ERR_NOSUPPORT` (-6)| Backend lacks this feature.                                        | Driver status: stub?  Kconfig opt-in?                 |
| `ALP_ERR_NOMEM` (-7)    | Handle pool exhausted.                                             | Increase `CONFIG_ALP_SDK_MAX_*_HANDLES`.              |
| `ALP_ERR_OUT_OF_RANGE` (-8)| Config exceeds documented HW caps.                              | Check `<alp/soc_caps.h>` for the active SoC's limits. |

`alp_last_error()` carries the most recent error on the current
thread, useful when a `*_open` returns NULL.

### `alp_i2c_open` returns NULL with `ALP_ERR_NOT_READY`

The bus is configured but the device-tree label isn't pointing at
a real I2C controller.  Check:

* The studio-generated DT alias points at the right node.
* `CONFIG_I2C=y` in your `prj.conf` (or auto-selected via
  `peripherals: [i2c]` in `board.yaml`).
* The board overlay defines the bus pins.

### `<chip>_init` returns `ALP_ERR_NOT_READY`

The chip isn't ACKing on its expected address.  Causes:

* Wrong I2C bus -- check the SoM preset (`E1M-<MPN>.yaml`) for which bus the chip is on
  (e.g. V2N's PMICs are on `brd_i2c`, not `e1m_i2c0`).
* Wrong slave address -- confirm against the SoM preset's
  `metadata/e1m_modules/<SKU>.yaml` `i2c_devices:` block (V2N/V2M/AEN
  families all carry one; each entry has its own `address_7bit`).
* Power not yet on the chip -- some chips need their REG_ON pin
  pulled high first (e.g. Murata Wi-Fi/BT module).

### `gd32g553_init` returns `ALP_ERR_NOSUPPORT`

The GD32 firmware's `GET_VERSION` reply reported a `major` that
doesn't match the host driver's `GD32G553_HOST_PROTOCOL_MAJOR`.
Either:

* The GD32 firmware hasn't been flashed yet -- attach SWD probe
  and flash `gd32-bridge.elf`.
* The firmware is from a different protocol epoch -- rebuild
  the firmware from the matching alp-sdk commit.

### `da9292_v2n_m1_enable_deepx_rail` returns `ALP_ERR_TIMEOUT`

The DEEPX rail (DA9292 CH2 to 0.75 V) isn't reaching power-good.
Likely a downstream short on the 0.75 V plane.  Probe:

* CH2 output pin on the DA9292 -- should reach 0.75 V within a
  few ms.
* DEEPX rail load on the silicon side.

### `<chip>_set_voltage_mv` returns `ALP_ERR_NOSUPPORT`

Stub-status driver.  Check the chip's `metadata/chips/<chip>.yaml`
`driver_status` field.  `stub` drivers expose only init + raw R/W;
high-level helpers wait for a follow-up implementation.

## Hardware-related issues

### E1M-AEN console prints nothing at all (not even the banner)

Zero bytes on the UART -- no `*** Booting Zephyr OS build ... ***`, no
Alp SDK banner, no log lines -- looks exactly like a bad flash or a dead
board, and usually isn't.  If your `main()` never yields (the
`for (;;) { k_busy_wait(1000); }` shape the AEN bench procedure calls
for, so the Secure Enclave doesn't gate the DAP and the SE-UART), then
under `CONFIG_LOG_MODE_DEFERRED` the log processing thread never gets
scheduled and `CONFIG_LOG_PRINTK` takes the banner down with it.

Distinguish it from a fault in one SWD attach: halt and read `PC`,
`IPSR`, and `CFSR` at `0xE000ED28`.  A `PC` inside `z_impl_k_busy_wait`
with `IPSR = 000` and `CFSR = 00000000` is a healthy, running,
log-starved board.

Every `zephyr/boards/alp/e1m_aen*` board tree now defaults to
`LOG_MODE_MINIMAL`, which prints from the calling context and cannot be
starved this way; you only hit this on an application that overrides it
back to `CONFIG_LOG_MODE_DEFERRED=y`.  Full write-up, including what
minimal mode costs you, in
[`debugging-aen.md` §6](debugging-aen.md#6-the-console-prints-nothing-and-the-board-is-fine).

### Module powers up but Renesas / Alif silicon doesn't boot

* Check the primary PMIC's nRESET line -- if it stays low, the
  PMIC didn't complete its sequence.  Probe `ACT88760_nRESET`
  test point.
* Check core rails directly with a scope: `VDD_0V8`, `VDD_3V3`,
  `VDD_1V8` should all be at their CMI / strap targets within
  a few ms of `V_IN` rising.

On an E1M-AEN module, rule the rails out first as above -- but if they
are healthy *and* SWD is alive (SW-DP IDR reads `0x4C013477`, memory
reads and writes work) while `VTOR` stays `0` and the cores never
start, the supply is not your problem and the Secure Enclave is
probably not damaged either.  That combination usually means the SES
has nothing valid to boot -- either no valid application TOC, or MCUboot
rejecting the image in slot0 (`E: Unable to find bootable image` /
`E: Bad image magic`) -- which is a reflash rather than a dead module,
and several of the observations that feel like evidence of a dead Secure
Enclave are not evidence at all.  See
[`debugging-aen.md` §7](debugging-aen.md#7-the-secure-enclave-boots-nothing-at-all--cores-parked-vtor-0),
which also covers how to tell that case apart from a genuine SE-side
fault with one passive SEUART capture.

### Ethernet PHY doesn't link

* MDIO probe should read `PHYID1 == 0x001C` (Realtek OUI).  If it
  doesn't, the PHY isn't reachable -- check MDC/MDIO routing +
  the 1 kΩ pull-up to `VDD_1V8`.
* If MDIO works but no link, run `rtl8211fdi_soft_reset` +
  `rtl8211fdi_restart_autoneg` and confirm `BMSR.link_status`
  flips.  Make sure your link partner supports a speed the PHY
  advertises.

### GD32 bridge `PING` succeeds but `GET_VERSION` returns bad CRC

The GD32 firmware ISR is too slow to populate its TX FIFO before
the host reads the reply.  See
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §4.1
for the timing window.  The host driver returns `ALP_ERR_IO` and
the caller can retry (commands are idempotent).

### `alp_hw_info_read` returns `ALP_ERR_NOT_PROVISIONED`

The EEPROM reads back blank/unprogrammed -- no `ALPH` magic at
offset 0.  Factory programming hasn't run on this module yet.
Inspect with:

```c
uint8_t raw[128];
eeprom_24c128_read(&ee, 0, raw, sizeof(raw));
// Dump raw bytes; expect wire bytes 0x48 0x50 0x4C 0x41 ("HPLA"
// in a hexdump) at offset 0 on a programmed module.
```

Run `scripts/program_eeprom.py` against the module.

### `alp_hw_info_read` returns `ALP_ERR_IO`

The manifest's magic is present but `schema_version` or the
CRC-32 disagrees -- the manifest is corrupt (partial write, bit
flip). Re-run `scripts/program_eeprom.py` against the module.

### `alp_hw_info_read` returns `ALP_ERR_NOSUPPORT`

The EEPROM-side hw_info reader isn't configured.  Set
`CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` in `prj.conf` to the bus
id carrying the on-module 24C128.  On V2N / V2N-M1 this is the bus
matching `ALP_E1M_I2C0` (Renesas RIIC0, `P31`/`P30`); on AEN it's
SoC I2C2 (DesignWare `i2c_dw`, `P5_6`/`P5_7`, bridge/DNP-selected --
NOT the slave-only LPI2C0 / BRD_I2C).

## CI / tooling issues

### `pr-twister.yml` fails with `west-commands: invalid in module.yml`

You're using a Zephyr release older than the SDK's pin.  Bump to v4.4.1 per
[`docs/zephyr-version-policy.md`](zephyr-version-policy.md).

### `clang-format` CI reports diffs you can't reproduce

CRLF line endings.  Run `git config --global core.autocrlf input`
on Windows; the repo's `.gitattributes` pins LF on every source
file but a misconfigured global setting can override that.

## Where to file bugs

* SDK metadata, schema, portable API, or reference-emitter bug: [`github.com/alplabai/alp-sdk/issues`](https://github.com/alplabai/alp-sdk/issues)
* Tan planner, executor, or command bug: [`github.com/alplabai/tan-cli`](https://github.com/alplabai/tan-cli). Python Tan owns build/run/flash/size/image/clean and the relocated planner; only `migrate`, `lock`, and `quality` still forward to west.
* Chip driver bug: file against alp-sdk; include the `driver_status` from the
  chip's metadata yaml.

Include in every report:

* Output of `git rev-parse HEAD` for alp-sdk.
* Your `board.yaml`.
* The full `tan build` + `west build` log.
* If real-silicon: which board + SoM SKU.
