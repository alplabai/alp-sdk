# Firmware engineer quickstart

This guide takes a firmware engineer from "I have an E1M-X module
on the bench" to a working application built against the SDK.  It
sits **alongside** the general [`docs/getting-started.md`](getting-started.md)
walkthrough (which covers workspace setup + the gpio-button-led
example end-to-end); this doc focuses on the choices and patterns
that matter when you're targeting a specific SoM and writing
real firmware.

## Who this is for

You're writing Zephyr or bare-metal C against an E1M-X System-on-
Module (AEN, V2N, V2N-M1, or N93 family) and you want:

* A clear picture of what the SDK gives you per-SoM.
* Idiomatic patterns for the on-module chips (PMICs, RTC, Wi-Fi/BT
  module, Ethernet PHYs, DEEPX NPU on V2N-M1).
* Pointers into the rest of the doc tree for deeper topics.

The hand-written firmware path is a first-class consumer of
alp-sdk — co-equal with the alp-studio codegen path, not a
fallback.  alp-sdk is self-contained: every file this guide
references (board.yaml schema, SoM presets at
`metadata/e1m_modules/<SKU>.yaml`, chip drivers, the loader)
ships in this repo.  alp-studio is optional; if you want
visual block-based codegen on top of the same SDK, follow its
own quickstart.

## 1. Pick a target

| If your hardware is...                      | `som.sku` to declare | Board default     | One-pager                                         |
|---------------------------------------------|----------------------|---------------------|---------------------------------------------------|
| E1M-AEN3..801 SoM on E1M EVK                | `E1M-AEN801` (etc.)  | `E1M-EVK`           | [`docs/soms/aen.md`](soms/aen.md)                 |
| E1M-X V2N101 / V2N102 SoM on E1M-X-EVK      | `E1M-V2N101`         | `E1M-X-EVK`         | [`docs/soms/v2n.md`](soms/v2n.md)                 |
| E1M-X V2N-M1 (V2M101 / V2M102) SoM          | `E1M-V2M101`         | `E1M-X-EVK`         | [`docs/soms/v2n-m1.md`](soms/v2n-m1.md)           |
| E1M-NX9101 (NXP i.MX 93)                    | `E1M-NX9101`         | `E1M-EVK`           | [`docs/soms/imx93.md`](soms/imx93.md)             |

The per-SoM one-pager covers what's populated, which examples
target it, the bring-up flow, and common gotchas.  The full
per-SKU populated-parts list lives in
[`metadata/e1m_modules/<SKU>.yaml`](../metadata/e1m_modules/);
the loader reads this file at `tan build` time (via alp-sdk's
`alp_orchestrate`) to decide which chip drivers to compile in.

## 2. Workspace setup

If you haven't already, follow the workspace bootstrap in
[`docs/getting-started.md`](getting-started.md) §1-3.  That gets
you a `alp-workspace/` with `alp-sdk/`, `zephyr/`, and the standard
modules.

For the rest of this doc, all paths are relative to `alp-workspace/`.

## 3. Your first `board.yaml`

The application's `board.yaml` is a single declarative file that
selects the target SoM, board, and per-core runtime + peripherals
+ chip drivers your app uses.  Example for a V2N101 single-core
Zephyr-on-M33 application that exercises the on-module GD32 bridge:

```yaml
som:
  sku: E1M-V2N101

preset: e1m-x-evk
cores:
  a55_cluster:
    os: "off"            # explicit override -- skip the Linux slice for this app
  m33_sm:
    app: ./src           # os: omitted -- M-cores default to zephyr per topology
    peripherals: [spi, i2c]

chips:
  - gd32g553

diagnostics:
  log_level: info
```

`tan build` validates this, fans out into per-core slices, and
emits `build/system-manifest.yaml`.  See
[`docs/board-config-schema.md`](board-config-schema.md) for the full schema and
[`docs/heterogeneous-builds.md`](heterogeneous-builds.md) for the
multi-core flagship walkthrough.

## 4. Pick a starting example

The `examples/` tree carries reference apps that double as
tutorials.  Comment density is ~50% so the source teaches by
itself.

### Cross-family examples (work on any SoM)

| Example                          | What it shows                                              |
|----------------------------------|------------------------------------------------------------|
| `gpio-button-led`                | Open a GPIO, set it up as input + output, basic patterns.  |
| `i2c-scanner`                    | Walk the I2C bus, report devices that ACK.                 |
| `pwm-led-fade`                   | PWM channel open + duty sweep.                             |
| `adc-voltmeter`                  | Sample an ADC channel; convert to millivolts.              |
| `uart-echo` / `uart-rx-ringbuf`  | UART TX / RX, with optional byte-granular ring buffer.     |
| `rtc-clock`                      | Set + read the SoC RTC.                                    |
| `wdt-feed`                       | Watchdog open + feed cadence.                              |

### V2N / V2N-M1-specific examples

| Example                          | What it shows                                              |
|----------------------------------|------------------------------------------------------------|
| `v2n-gd32-bridge-ping`           | PING + GET_VERSION round-trip on both SPI + I2C transports |
| `v2n-board-id-readout`           | Read the SoM EEPROM manifest + assert SKU                  |

Build any of them as:

```bash
cd alp-workspace
tan build --native alp-sdk/examples/<name>    # native_sim
# or:
tan build --board <board> alp-sdk/examples/<name> && tan flash   # real silicon
```

## 5. Idiomatic patterns

### Open an I2C bus + talk to a chip

```c
alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
    // From <alp/board.h>; resolves to ALP_E1M_I2C0 on E1M EVKs
    // and ALP_E1M_X_I2C0 on E1M-X EVKs.
    .bus_id     = BOARD_I2C_SENSORS,
    .bitrate_hz = 400000u,
});
if (bus == NULL) {
    printf("[i2c] open failed: err=%d\n", (int)alp_last_error());
    return;
}

tmp112_t temp;
if (tmp112_init(&temp, bus, TMP112_I2C_ADDR_GND) != ALP_OK) {
    printf("[tmp112] init failed\n");
}
```

Every on-module chip driver follows the same shape:
`<chip>_init(&ctx, bus, addr_or_handle)` then per-feature getters /
setters.  Drivers under `chips/<part>/<part>.c`; public headers
under `<alp/chips/<part>.h>`.

### Use the GD32 bridge from V2N firmware

The V2N module's companion GD32G553 supervisor MCU owns every
E1M-standard analog + counter peripheral plus a fleet of side-channel
GPIOs: all eight PWM channels, the eight-channel ADC bank, both DAC
outputs, the four quadrature encoders, the Wi-Fi/BT REG_ON pins, the
OPTIGA reset, and the DA9292 INT/TW fault-pin forwarder (per the
2026-05-12 schematic decision).  Reach it via
[`<alp/chips/gd32g553.h>`](../include/alp/chips/gd32g553.h) over
SPI (fast path) or I2C (management path):

```c
alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
    .bus_id = 1u, .freq_hz = 25000000u,
    .mode = ALP_SPI_MODE_0, .bits_per_word = 8u,
    /* The platform SPI driver owns the chip-select on this SoM. */
    .cs_pin_id = ALP_SPI_NO_CS,
});
alp_i2c_t *brd_i2c = alp_i2c_open(&(alp_i2c_config_t){
    .bus_id = 0u, .bitrate_hz = 400000u,
});

gd32g553_t bridge;
gd32g553_init(&bridge, spi, brd_i2c, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);

uint32_t period_ns = 1000000u; // 1 kHz
uint32_t duty_ns   = 500000u;  // 50 %
gd32g553_pwm_set(&bridge, /* channel */ 0u, period_ns, duty_ns);
```

Full wire spec: [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md).
Firmware-tree overview: [`docs/gd32-bridge.md`](gd32-bridge.md).

### Monitor the PMIC fleet

On V2N + V2N-M1 the secondary PMIC's status reflects rail health.
Idiomatic supervisor task:

```c
da9292_t pmic;
da9292_init(&pmic, brd_i2c, DA9292_I2C_ADDR_V2N);

while (1) {
    da9292_status_t s;
    if (da9292_get_status(&pmic, &s) == ALP_OK) {
        if (s.temp_warn || s.vin_uvlo || !s.ch1_pg) {
            log_warning(&s);
        }
    }
    k_msleep(1000);
}
```

### Bring up the dual Ethernet on V2N

The V2N module's two RTL8211FDI PHYs (ET0 + ET1) sit on separate
MDIO buses on the Renesas side.  The driver takes callback
function pointers so it works against any MDIO controller:

```c
static int my_mdio_read(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user) {
    return zephyr_mdio_read(user, phy_addr, reg, val);
}
static int my_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user) {
    return zephyr_mdio_write(user, phy_addr, reg, val);
}

rtl8211fdi_t phy0;
rtl8211fdi_init(&phy0, /* phy_addr */ 0, my_mdio_read, my_mdio_write, mdio_dev);
rtl8211fdi_soft_reset(&phy0, 500000);
rtl8211fdi_restart_autoneg(&phy0);
```

Walk-through: [`docs/bring-up-v2n.md`](bring-up-v2n.md) §5.

### V2N-M1: bring DEEPX up before opening PCIe

Three steps in the V2N-M1 bring-up that V2N base skips:

1. `da9292_v2n_m1_enable_deepx_rail(&pmic, 50000)` -- the 0.75 V
   DEEPX rail on the secondary PMIC's CH2.
2. ACK-probe the three DEEPX TPS628640 instances at `0x44 / 0x48 /
   0x4F` to confirm population.
3. `deepx_dxm1_bring_up(&dxm1, DEEPX_DXM1_DEFAULT_BOOT_US)` -- the
   PCIe muxes + M1_RESET sequencer.

Full sequence with code: [`docs/bring-up-v2n-m1.md`](bring-up-v2n-m1.md).

## 6. Chip driver opt-in

Every chip driver is gated on a `CONFIG_ALP_SDK_CHIP_<NAME>=y`
symbol so unused chips don't cost code size.  SDK-level block
helpers (see `blocks/README.md` -- currently `button_led` and
`pdm_mic`) live behind `CONFIG_ALP_SDK_BLOCK_<NAME>=y` instead;
the loader picks the right symbol for each populated slug, so
declarative `board.yaml` use is identical either way.  Two ways
to flip the flag:

* **Declarative** (recommended): add the slug to the top-level
  `populated:` block in `board.yaml` (`button_led: true`,
  `ssd1306: true`, ...) -- or, if you're using `preset:` to
  reference a shared board definition, add it to the project's
  `chips:` array.  The loader's per-core Kconfig emitter writes
  the appropriate `CONFIG_ALP_SDK_CHIP_<NAME>=y` /
  `CONFIG_ALP_SDK_BLOCK_<NAME>=y` on every Zephyr slice, plus
  the Zephyr subsystem (`CONFIG_I2C`, `CONFIG_GPIO`, ...) each
  enabled driver needs.
* **Manual**: add `CONFIG_ALP_SDK_CHIP_GD32G553=y` (or
  `CONFIG_ALP_SDK_BLOCK_BUTTON_LED=y`) etc. to your `prj.conf`.
  Necessary for parts your board preset doesn't cover (custom
  boards + Tier-2 community drivers).

The full chip catalogue is in `metadata/chips/` -- one yaml per
part, listing the driver status (`stub` / `partial` / `complete`),
which SoM families populate it, and the upstream datasheet
reference.

## 7. Build for real silicon

`tan build --board` figures out the cross-compile target from the
SoM's `silicon:` field.  Common boards:

```bash
# V2N (RZ/V2N)
tan build --board <renesas_rzv2n_board> alp-sdk/examples/v2n/v2n-gd32-bridge-ping
tan flash

# AEN (Alif Ensemble)
tan build --board <alif_ensemble_board> alp-sdk/examples/peripheral-io/gpio-button-led
tan flash
```

The exact `<board>` argument depends on whether you're using an
upstream Zephyr board file (e.g. `ensemble_e8_dk`) or one of the
in-tree Alp E1M board files under
[`zephyr/boards/alp/`](../zephyr/boards/alp/) (e.g.
`alp_e1m_aen801_m55_he`, `alp_e1m_v2n101_m33_sm`).  See
[`docs/architecture.md`](architecture.md) for the split.

The `tan` CLI covers the same flow in fewer keystrokes:
`tan build && tan flash` programs every slice + helper MCU in
`boot_order:` (see [`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)).
`alp monitor --port <port>` opens the board's serial console
afterwards (portless it lists the host's serial ports) -- `alp`
never builds, so it's unaffected by which executor you used.  If a
build machine misbehaves, `alp doctor` is the hardware-free
environment triage.  Verb reference: [`docs/cli.md`](cli.md).

## 8. Where to look next

| Topic                                            | Document                                          |
|--------------------------------------------------|---------------------------------------------------|
| Workspace + tooling deep-dive                    | [`docs/getting-started.md`](getting-started.md)   |
| `alp` CLI verb reference                         | [`docs/cli.md`](cli.md)                           |
| `board.yaml` schema reference                    | [`docs/board-config-schema.md`](board-config-schema.md) |
| Architecture (modules, wrappers, codegen split)  | [`docs/architecture.md`](architecture.md)         |
| SoM bring-up procedures                          | [`docs/bring-up-v2n.md`](bring-up-v2n.md), [`docs/bring-up-v2n-m1.md`](bring-up-v2n-m1.md) |
| GD32 bridge wire protocol                        | [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) |
| EEPROM manifest + hw-rev identification          | [`docs/board-id.md`](board-id.md)                 |
| Secure boot                                      | [`docs/secure-boot.md`](secure-boot.md)           |
| OTA (Mender device contract)                     | [`docs/ota.md`](ota.md), [`docs/ota-device-contract.md`](ota-device-contract.md) |
| Verification + release matrix                    | [`docs/test-plan.md`](test-plan.md)               |
| HiL runner contract                              | [`docs/ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md)          |
| Porting to a new SoM                             | [`docs/porting-new-som.md`](porting-new-som.md)   |

## 9. Getting help

* File an issue at <https://github.com/alplabai/alp-sdk/issues>
  with a reproducer.  Include the output of
  `tan --version`, your `board.yaml`, and the full `tan build` /
  `west build` log.
* For a chip driver bug: include the chip's `metadata/chips/<part>.yaml`
  driver status (`stub` chips return `ALP_ERR_NOSUPPORT` from
  many helpers by design).
* For build-tooling questions: see
  [`docs/testing.md`](testing.md) §"common build errors".
