<!-- SPDX-License-Identifier: Apache-2.0 -->
# CC3501E GPIO-proxy bench validation (warm boot)

How to validate the **CC3501E GPIO proxy** — synchronous GPIO write/read plus
camera-enable — on the bench, end to end, from the Alif (E8) host. The CC3501E
drives several E1M IO pads and the camera-LDO enables off its own GPIOs; the host
(Alif) reaches them over the inter-chip SPI bridge (`docs/cc3501e-bridge.md`). This
procedure exercises that path with the host example `examples/aen/aen-cc3501e-gpio`
and asserts a machine-checkable serial contract.

The turnkey harness is `firmware/cc3501e/ti/validate_gpio_bench.ps1`: it builds +
warm-flashes the CC3501E firmware, builds + flashes the host example, captures the
host console, and exits `0` only on an all-pass summary.

## Boot path: WARM only

This validates the **warm** (debug-attached / host-driven-reset) boot path. The
host app drives `WIFI_EN` + `nRESET`, which brings the coprocessor up through the
path that **skips the cold Chain-of-Trust**.

The current bench unit was activated with `vendor_sbl_container_enable=1` but with
**no** TI vendor SBL programmed, so a cold power-on POR never launches the image
(the missing SBL breaks the chain). **Cold boot is gated by that fuse and is out of
scope here** — see `docs/cc3501e-production.md` ("Unit activation — must be
cold-bootable"). The GPIO proxy behaves identically on warm vs cold; warm is
sufficient to validate the wire contract and the firmware GPIO/camera bodies on
this unit.

## Scope: synchronous write / read / cam only

The current E1M-AEN board rev wires **no host-IRQ line**, so the asynchronous GPIO
interrupt path (`CMD_GPIO_SET_INTERRUPT` → `EVT_GPIO_INTERRUPT`, an async push from
the coprocessor to the host) **cannot be delivered to the host**. Async GPIO-IRQ is
therefore an **r2 (next board rev) limitation**.

The example still *arms* an IRQ (the `gpio_irq_arm` step) to prove the configure
command round-trips and is accepted, but it does **not** assert an edge or wait for
an event. This procedure validates only the **synchronous** GPIO configure / write /
read and the camera enable/disable.

## Pad map under test

Routes are from `metadata/e1m_modules/aen/inter-chip.tsv`. The GPIO proxy carries
the **raw CC3501E pad index**; the logical E1M-IO → raw mapping lives host-side in
board metadata.

| Role under test | E1M IO   | CC3501E pad | Proxy argument          |
|-----------------|----------|-------------|-------------------------|
| GPIO loopback   | IO13     | GPIO13      | pad index **13**        |
| Camera enable   | —        | GPIO1       | `CAM_EN_LDO0`, cam id **0** |

`IO13 → GPIO13` is the safe-to-toggle write→read loopback pad. The camera enable
toggles `CAM_EN_LDO0`, which is CC3501E `GPIO1` (cam id `0`); the second camera LDO
(`CAM_EN_LDO1`) is `GPIO0` (cam id `1`) and is not exercised here. (Per the E1M-AEN
BDE-BW35N netlist: pin54 `GPIO_1` = `CAM_EN_LDO0`, pin55 `GPIO_0` = `CAM_EN_LDO1` —
the firmware `cc3501e_hw_cam_enable` map is authoritative; the SWRU626-era note that
had these reversed is wrong.)

## Prerequisites

- An **activated** CC3501E bench unit on the SoM (warm path; cold-boot fuse caveat
  above), reachable over an XDS110 probe, and an Alif J-Link for the host.
- The TI build toolchain for the CC3501E firmware (ticlang + the SimpleLink CC35xx
  SDK + SysConfig) — same install `firmware/cc3501e/ti/build_ti.ps1` uses.
- The SimpleLink Wi-Fi toolbox (`simplelink-wifi-toolbox.exe`) for the FIB/sign/
  program steps, plus the bench **Alp VALIDATION** signing key + module, the
  `cc35xx-conf.bin` memory config, and a `tool_settings.json` that references
  `primary_vendor_image.sign.bin`. The validation key is for bench/staging only;
  those units are **not** production-shippable (rooted to the validation key, not
  the HSM) — see `docs/cc3501e-production.md`.
- A West/Zephyr environment on PATH (the host example builds with `west`), and the
  host carrier console on a known COM port (the E1M edge "UART0", 115200 8N1).
- The host example present at `examples/aen/aen-cc3501e-gpio`, emitting the
  `GPIO_TEST:` contract below.

## Warm-boot validation steps

The harness performs these in order; you can also run them by hand.

1. **Build the CC3501E firmware** — `firmware/cc3501e/ti/build_ti.ps1` (default
   radio-free bridge build; the GPIO proxy + camera enables are in the default
   build, no Wi-Fi/BLE link needed to exercise them).
2. **Warm-flash the CC3501E** — FIB-wrap the ELF as a versioned GPE vendor image,
   sign it with the **Alp VALIDATION** key, and program it over the XDS110. The
   image `--version` must be **monotonic** (≥ the version on the unit). Note: the
   `sign` step names its output after the input base name, so the signed image is
   copied to `primary_vendor_image.sign.bin` (the name `tool_settings.json`
   references) before programming. A transient `-1141` SECAP reject is retried once.
3. **Build the host example** — `west build -p always -b
   alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-gpio`
   (full qualified board target so the per-board overlay auto-applies).
4. **Flash the host example** — `west flash` over the Alif J-Link. Flashing the
   host app is what warm-resets and re-powers the CC3501E (the host drives
   `WIFI_EN` + `nRESET`), so this is the coprocessor's warm boot.
5. **Capture the console + assert the contract** — listen on the carrier console
   and check the `GPIO_TEST:` lines below.

Example invocation (override every path — nothing is hardcoded to a user dir):

```
firmware/cc3501e/ti/validate_gpio_bench.ps1 `
    -ToolboxExe     <simplelink-wifi-toolbox.exe> `
    -PublicKey      <alp_validation_pub.pem> `
    -SigningModule  <sign.py> `
    -ConfBin        <cc35xx-conf.bin> `
    -ToolSettings   <tool_settings.json> `
    -CcXdsSerial    <XDS110_SN> `
    -HostSerialPort COM7
```

## Expected serial output (the GPIO_TEST contract)

The example prints one line per step, in this exact order, followed by a summary:

```
GPIO_TEST: gpio_config_out PASS
GPIO_TEST: gpio_write_high PASS
GPIO_TEST: gpio_write_low PASS
GPIO_TEST: gpio_config_in PASS
GPIO_TEST: gpio_read PASS
GPIO_TEST: cam_enable PASS
GPIO_TEST: cam_disable PASS
GPIO_TEST: gpio_irq_arm PASS
GPIO_TEST: SUMMARY pass=8 fail=0
```

Step meanings:

| Step              | What it checks                                                       |
|-------------------|---------------------------------------------------------------------|
| `gpio_config_out` | Configure CC3501E GPIO13 as an output via the proxy.                 |
| `gpio_write_high` | Drive GPIO13 high.                                                   |
| `gpio_write_low`  | Drive GPIO13 low.                                                    |
| `gpio_config_in`  | Reconfigure GPIO13 as an input.                                      |
| `gpio_read`       | Read GPIO13 back over the proxy.                                     |
| `cam_enable`      | Enable `CAM_EN_LDO0` (cam id 0).                                     |
| `cam_disable`     | Disable `CAM_EN_LDO0` (cam id 0).                                    |
| `gpio_irq_arm`    | Arm an edge IRQ on GPIO13 — proves the configure command round-trips; **no async edge is delivered on this rev** (r2 limitation). |

## Pass criteria

**PASS iff `GPIO_TEST: SUMMARY pass=N fail=0`** — i.e. zero failed steps. The harness
exits `0` on an all-pass summary, `1` on `fail≥1`, and `2` if no summary line is seen
within the capture window (treat as "no link / wrong port / app not running").

`gpio_irq_arm` passing means only that the firmware accepted the IRQ-configure
command; it does **not** mean an asynchronous interrupt was delivered (that requires
the r2 host-IRQ line).

## Validation record

**PASS — silicon-validated 2026-06-20 on E1M-AEN801 (Alif E8, M55-HE).** All eight
proxy ops round-tripped over the 3-wire SPI bridge to the CC3501E:

```
[cc3501e-gpio] cc3501e bridge bring-up -> 0
GPIO_TEST: gpio_config_out PASS   GPIO_TEST: gpio_write_high PASS
GPIO_TEST: gpio_write_low  PASS   GPIO_TEST: gpio_config_in  PASS
[cc3501e-gpio] pad 13 reads HIGH  GPIO_TEST: gpio_read   PASS
GPIO_TEST: cam_enable PASS        GPIO_TEST: cam_disable PASS
GPIO_TEST: gpio_irq_arm PASS      GPIO_TEST: SUMMARY pass=8 fail=0
```

This also validates the SPI1 data path (P14_4/5/6 pinctrl + the `spi@48104000` base
+ `clock-frequency` workaround) and the CC3501E cold-boot Puya double-boot. Flash flow
used: the example is MRAM-slot0-linked (`CONFIG_FLASH_LOAD_OFFSET=0x10000`, reset
vector `0x8001xxxx`), so it is flashed with the **two-blob** `flash-jlink-mramxip.sh`
(app → `0x80010000` + signed ATOC), **not** the single-blob ITCM `flash-jlink.sh`.
`bring-up -> 0` confirms the CC3501E launched and answered in lockstep. (`gpio_read`
sensing HIGH is the configured internal pull-up; no external loopback is wired.)

## Known limitation: async GPIO-IRQ (r2)

The asynchronous GPIO interrupt push (`EVT_GPIO_INTERRUPT`) is not validated here and
cannot work on the current board rev — there is **no host-IRQ line** for the
coprocessor to signal the host out of band. The next board rev adds a CS line and a
host-IRQ line, which removes the CS-less framing fragility and enables async events
(BLE / Wi-Fi / GPIO-IRQ push). Until then, the GPIO proxy is synchronous request →
reply only, and this procedure covers exactly that surface.
