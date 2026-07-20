# aen-cc3501e-gpio

Host (Alif) side of the on-module **TI CC3501E** GPIO proxy + camera-enable
demo on the **E1M-AEN801** (Alif Ensemble E8) SoM.

This app runs on the Alif **M55-HE** core. It brings the inter-chip SPI
bridge up exactly like
[`examples/aen/aen-cc3501e-bringup`](../aen-cc3501e-bringup) (one call to
`cc3501e_bridge_bringup()`), then exercises a different slice of the
coprocessor firmware: it **configures, drives, and reads a proxied CC3501E
GPIO** and **enables/disables the camera LDOs** over the bridge, using only
the portable `cc3501e_*` host API.

The CC3501E peer firmware is ALP-authored and lives in this repo at
[`firmware/cc3501e/`](../../../firmware/cc3501e) (embedded, per ADR 0015 —
like the gd32-bridge).

## What is the GPIO proxy?

Some E1M edge IO pads on the AEN SoM are **not** wired to an Alif pin —
they are wired to a **CC3501E** GPIO. So when an application asks for
"IO13", the SDK cannot toggle an Alif register; it asks the CC3501E
firmware to toggle the pad on its behalf, over the SPI bridge. That
indirection — host op → bridge frame → CC3501E drives the pad — is the
GPIO **proxy**.

Two ways to use it:

1. **Portable** (what an application normally writes):
   ```c
   alp_gpio_t *io = alp_gpio_open(ALP_E1M_GPIO_IO13);
   alp_gpio_configure(io, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
   alp_gpio_write(io, true);
   ```
   With `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y` and the route table in
   [`src/cc3501e_gpio_routes.c`](src/cc3501e_gpio_routes.c), the SDK
   silently routes `ALP_E1M_GPIO_IO13` over the bridge to raw CC3501E
   `GPIO_13` — the app never names the coprocessor.

2. **Direct** (what this demo does, to make each bridge op visible in the
   teaching log):
   ```c
   cc3501e_gpio_configure(&fw, 13, ALP_CC3501E_GPIO_DIR_OUTPUT,
                          ALP_CC3501E_GPIO_PULL_NONE, 100);
   cc3501e_gpio_write(&fw, 13, true, 100);
   ```
   Here `pad` is the **raw** CC3501E GPIO index — the same wire op the
   portable path emits underneath.

The route table ties `ALP_E1M_GPIO_IO13` → raw CC3501E GPIO 13, so "pad 13" in
this demo is the same pad a portable `alp_gpio_open(ALP_E1M_GPIO_IO13)` reaches.

## The proxied pads

From [`src/cc3501e_gpio_routes.c`](src/cc3501e_gpio_routes.c) (E1M-AEN
BDE-BW35N netlist):

| E1M IO        | raw CC35 GPIO | carrier net      |
|---------------|---------------|------------------|
| `ALP_E1M_GPIO_IO8`  | 30 | I2S_EN |
| `ALP_E1M_GPIO_IO9`  | 12 | PCIE_IO_EXP.RST |
| `ALP_E1M_GPIO_IO10` | 35 | PCIE0_I2C.EN |
| `ALP_E1M_GPIO_IO11` | 2  | USB2_SELECT |
| `ALP_E1M_GPIO_IO13` | 13 | I2S_SELECT (← **demo pad**) |
| `ALP_E1M_GPIO_IO15` | 14 | S_BMI323.INT1 |
| `ALP_E1M_GPIO_IO16` | 17 | EN_W_DIS2n (open-drain) |
| `ALP_E1M_GPIO_IO18` | 18 | M2E_SDIO_WAKEn |
| `ALP_E1M_GPIO_IO19` | 19 | M2E_UART.WAKEn_L |
| `ALP_E1M_GPIO_IO20` | 26 | MUX_EN |

The two camera-enable LDOs are `which=0` → CAM_EN_LDO0 (CC35 GPIO_1) and
`which=1` → CAM_EN_LDO1 (CC35 GPIO_0).

## The r2 host-IRQ caveat (read before using interrupts)

This HW rev uses hardware SS0 for SPI framing and a READY input for per-phase
gating, but it still does not expose GPIO edge events as portable application
callbacks. The Alif is always master; the CC3501E is always slave. Without a
dedicated async event delivery path, the slave cannot spontaneously tell the
master "an edge happened on a GPIO".

So `cc3501e_gpio_set_interrupt()`:

- **arms** the edge on the CC3501E's own GPIO controller — real, it latches
  edges in the coprocessor;
- but the async `ALP_CC3501E_EVT_GPIO_INTERRUPT` frame is not a portable callback
  path in this example. Poll `cc3501e_gpio_read()` when you need to observe the
  level today.

Until then the host **must poll** `cc3501e_gpio_read()` to observe a level
change; the armed interrupt does not call you back. Do not build a design
that depends on `EVT_GPIO_INTERRUPT` delivery on this rev.

(`EDGE_BOTH` is unsupported on the CC35xx controller — arm RISING or
FALLING, never both.)

## What it does

1. `cc3501e_bridge_bringup()` — open the bridge SPI + WIFI_EN/nRESET,
   attach the proxy, power + reset the coprocessor.
2. `gpio_config_out` — configure pad 13 as `DIR_OUTPUT` / `PULL_NONE`.
3. `gpio_write_high` — drive pad 13 high.
4. `gpio_write_low` — drive pad 13 low.
5. `gpio_config_in` — reconfigure pad 13 as `DIR_INPUT` / `PULL_UP`.
6. `gpio_read` — sample pad 13 and print the level.
7. `cam_enable` / `cam_disable` — enable then disable camera LDO0.
8. `gpio_irq_arm` — arm a rising-edge interrupt on pad 13 (arms only — see
   the r2 caveat).

## Bench contract

Each step prints exactly one line a sibling bench script greps:

```
GPIO_TEST: <step> <PASS|FAIL>
```

with the `<step>` tokens, in order: `gpio_config_out`, `gpio_write_high`,
`gpio_write_low`, `gpio_config_in`, `gpio_read`, `cam_enable`,
`cam_disable`, `gpio_irq_arm`, followed by:

```
GPIO_TEST: SUMMARY pass=<n> fail=<n>
```

On silicon with the CC3501E firmware up, expect `pass=8 fail=0`.

## Run under native_sim (CI gate)

```sh
west twister -T examples/aen/aen-cc3501e-gpio -p native_sim/native/64
```

`native_sim/native/64` has only an **emulated** SPI controller with no
CC3501E attached (see [`boards/native_sim_native_64.overlay`](boards/native_sim_native_64.overlay)),
so every bridge op times out and the contract lines print **FAIL** there —
that is expected. native_sim proves the example **builds** (twister
`build_only`); the PASS/FAIL contract is a **silicon** signal, read on real
hardware.

The native_sim build mechanism mirrors aen-cc3501e-bringup:

- the planner (`scripts/alp_orchestrate`) derives this core's `alp.conf`
  from `board.yaml` and wires it into `west build` via
  `-DEXTRA_CONF_FILE=...` (see `docs/adr/0020-sdk-owns-build-execution.md`);
- `boards/native_sim_native_64.conf` pulls in `CONFIG_EMUL` / `CONFIG_SPI_EMUL`;
- `boards/native_sim_native_64.overlay` provides the emulated SPI controller
  + the `alp,pin-array` so `alp-spi1` and the control pins enumerate;
- the silicon-only bridge code compiles against the emulated bus.

## Build + flash (bench)

Two J-Links: one on the Alif, one on the CC3501E.

```sh
# 1. Build + flash the CC3501E peer firmware first (see firmware/cc3501e).
#    The CC3501E reads VTref=0V until this app powers it, so flash it with
#    the Alif app already powering WIFI_EN, or hold WIFI_EN externally.

# 2. Build + flash this Alif app (use the FULL qualified board target so the
#    per-board overlay boards/<target>.overlay is auto-applied):
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-cc3501e-gpio
west flash                      # over the Alif J-Link (AE822FA0E5597LS0_M55_HE)
```

> The per-board overlay is named for the **full** board target
> (`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`); Zephyr's
> auto-discovery matches the qualified target, not the bare board name.

Watch the carrier console (Alif **UART5**, P3_4/P3_5, 115200 8N1 — the E1M
edge "UART0") for the `[cc3501e-gpio]` log and the `GPIO_TEST:` lines. If the
link wedges, cold-cycle via the bench PSU CH2.
