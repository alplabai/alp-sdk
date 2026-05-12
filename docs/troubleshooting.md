# Troubleshooting

Common issues firmware engineers hit when working with the SDK,
organised by where the symptom shows up.  Each entry includes the
exact error text and a fix.

## Build-time errors

### `alp_project: no preset for SKU <X> at ...`

The `som.sku` in your `board.yaml` doesn't match a per-SKU manifest.
Confirm one of the released SKUs (see [`metadata/e1m_modules/`](../metadata/e1m_modules/)
for the catalogue):

```yaml
som:
  sku: E1M-V2N101    # or E1M-AEN701, E1M-V2M101, E1M-NX9101, etc.
```

### `alp_project: schema violation at <loc>: ...`

The validator caught a `board.yaml` problem.  The error message
names the JSON-pointer location (e.g. `peripherals/2` for the
third entry).  Common slip-ups:

* `peripherals:` entries must be lowercase strings from the allowed
  set (`i2c`, `spi`, `pwm`, `adc`, `uart`, `i2s`, `can`, `rtc`, `wdt`,
  `counter`).
* `som.sku` follows `E1M-{AEN,V2N,V2M,NX9}\d{3}` (case-sensitive).
* `carrier.name` must point at a preset under
  `metadata/carriers/<name>/board.yaml` OR carry an inline
  `carrier.populated:` block.

Full schema reference: [`docs/board-config.md`](board-config.md).

### `alp_project: SDK <V> is older than SoM hw_rev '<R>' minimum <M>`

Your `som.hw_rev` declares a rev that needs a newer SDK than this
checkout.  Either:

* Update the SDK (`west update`) to a version >= `<M>`, OR
* Pick an older `som.hw_rev` whose `min_sdk_version` window covers
  the current SDK.  The available revs for your family are listed
  in `metadata/e1m_modules/<family>/hw-revisions.yaml`.

### `west: command not found` / `pip install west` fails

The Zephyr meta-tool needs Python 3.10+.  On macOS:

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

### `CMake Error: Could not find package configuration file Zephyr`

Either `ZEPHYR_BASE` is not set, or your workspace is malformed.

```bash
cd alp-workspace
export ZEPHYR_BASE="$PWD/zephyr"
# OR:
west zephyr-export
```

### Compile error: `'alp_xxx_t' undeclared`

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

* Wrong I2C bus -- check `som.yaml` for which bus the chip is on
  (e.g. V2N's PMICs are on `brd_i2c`, not `e1m_i2c0`).
* Wrong slave address -- confirm against
  `metadata/e1m_modules/<SKU>/som.yaml` `i2c_devices` block.
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

### Module powers up but Renesas / Alif silicon doesn't boot

* Check the primary PMIC's nRESET line -- if it stays low, the
  PMIC didn't complete its sequence.  Probe `ACT88760_nRESET`
  test point.
* Check core rails directly with a scope: `VDD_0V8`, `VDD_3V3`,
  `VDD_1V8` should all be at their CMI / strap targets within
  a few ms of `V_IN` rising.

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

### `alp_hw_info_read` returns `ALP_ERR_IO`

CRC mismatch in the EEPROM manifest -- factory programming hasn't
run on this module, or the manifest is corrupt.  Inspect with:

```c
uint8_t raw[128];
eeprom_24c128_read(&ee, 0, raw, sizeof(raw));
// Dump raw bytes; expect "ALPH" (0x41 0x4C 0x50 0x48) at offset 0.
```

Re-run `tools/program_eeprom.py` against the module.

### `alp_hw_info_read` returns `ALP_ERR_NOSUPPORT`

The EEPROM-side hw_info reader isn't configured.  Set
`CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` in `prj.conf` to the
bus id matching `E1M_I2C0` on your board.

## CI / tooling issues

### `pr-twister.yml` fails with `west-commands: invalid in module.yml`

You're using Zephyr v3.6 or older.  Pin v3.7.0 LTS per
[`docs/zephyr-version-policy.md`](zephyr-version-policy.md).

### `clang-format` CI reports diffs you can't reproduce

CRLF line endings.  Run `git config --global core.autocrlf input`
on Windows; the repo's `.gitattributes` pins LF on every source
file but a misconfigured global setting can override that.

## Where to file bugs

* SDK bug: [`github.com/alplabai/alp-sdk/issues`](https://github.com/alplabai/alp-sdk/issues)
* `west alp-build` tooling bug: same.
* Chip driver bug: same; include the `driver_status` from the
  chip's metadata yaml.

Include in every report:

* Output of `git rev-parse HEAD` for alp-sdk.
* Your `board.yaml`.
* The full `west alp-build` + `west build` log.
* If real-silicon: which carrier + SoM SKU.
