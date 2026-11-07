# aen-cc3501e-companion-tour

The **capstone** full-surface example for the on-module **TI CC3501E**
Wi-Fi 6 + BLE 5.4 coprocessor, driven from the Alif **M55-HE** core on the
**E1M-AEN801** (Alif Ensemble E8) SoM.

Where [`aen-cc3501e-bringup`](../aen-cc3501e-bringup) is the *minimal* power +
`PING` soak, this app walks the portable wireless checkpoint plus the
diagnostic companion API in one linear sequence — as living documentation of
the hand-written-firmware path. Application-level Wi-Fi/BLE calls go through
[`<alp/iot.h>`](../../../include/alp/iot.h) and
[`<alp/ble.h>`](../../../include/alp/ble.h); bridge diagnostics go through
[`<alp/chips/cc3501e.h>`](../../../include/alp/chips/cc3501e.h). The app never
touches the raw wire protocol.

## The tour

```
init  ->  ping  ->  diag info
      ->  portable Wi-Fi open  ->  portable BLE open + scan
      ->  Wi-Fi scan  ->  Wi-Fi connect  ->  get IP
      ->  TCP socket: open -> connect -> send -> recv -> close
      ->  Wi-Fi disconnect
      ->  BLE enable  ->  BLE scan  ->  BLE disable
      ->  proxied-GPIO read (IO15 over the bridge)
```

Every step is **non-fatal**: a step that fails prints its status and the tour
continues. On a bench with no AP in range the connect times out; on a
firmware image built without BLE the enable returns `NOT_READY`; the example
still runs end to end and shows the shape of each call. A real application
would branch on these statuses instead of just logging them.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

`tan` builds one image per Zephyr core on the SoM: a core this `board.yaml` does not declare gets the SDK's idle `alp-stock-shim` image (on an E1M-AEN SoM, `m55_hp` here, in `build/m55_hp-zephyr/`); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

## Wi-Fi credentials + socket target (build-time)

Credentials are **deliberately empty by default** — never hardcode them in a
public example. Set them at build time without editing the source. The
`-DTOUR_*` defines below are passed through the west recipe, which works
only from an alp-sdk checkout:

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `gen_example_alp_conf.py` writes `generated/alp.conf` there.

```sh
# writes examples/aen/aen-cc3501e-companion-tour/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/aen/aen-cc3501e-companion-tour
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-cc3501e-companion-tour -- -DEXTRA_CONF_FILE=generated/alp.conf \
  -DEXTRA_CFLAGS="-DTOUR_WIFI_SSID=\\\"myssid\\\" -DTOUR_WIFI_PASS=\\\"mypass\\\""
```

When `TOUR_WIFI_SSID` is empty the connect / IP / socket steps are skipped
(they need an association) and the tour jumps to the BLE section. The TCP
destination defaults to `192.168.1.1:80` (`TOUR_TCP_A`..`TOUR_TCP_D` for the
four address octets and `TOUR_TCP_PORT` in `src/main.c`) — override them with
`-DTOUR_TCP_A=... -DTOUR_TCP_B=... -DTOUR_TCP_C=... -DTOUR_TCP_D=...` (and
`-DTOUR_TCP_PORT=...`) to point at a host reachable from your bench network.

## SoM bring-up is one call

The hardware setup (control pins + inter-chip SPI + power/reset sequence) is
`cc3501e_bridge_bringup()` in
[`src/cc3501e_bridge.c`](src/cc3501e_bridge.c) — the reusable SoM template
shared with the bringup example. The proxied-GPIO step uses the E1M-AEN route
table in [`src/cc3501e_gpio_routes.c`](src/cc3501e_gpio_routes.c) (built with
`CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y`): `alp_gpio_open(ALP_E1M_GPIO_IO15)`
routes over the bridge while every other pin delegates to the platform GPIO
driver.

The portable checkpoint prints stable `PORTABLE_WIRELESS:` lines. On real AEN
silicon, `wifi_open PASS` proves `alp_wifi_open()` selected the attached
CC3501E provider, and `ble_scan PASS count=N` proves `<alp/ble.h>` reached the
same radio path.

See [`aen-cc3501e-bringup`](../aen-cc3501e-bringup) for the inter-chip wiring
table, the bench build/flash recipe (two J-Links), and the
bench-unverified-overlay notes — they apply verbatim here.

## Command reference

For the full `alp companion` console command surface exercised here (wifi /
ble / sock / diag / ota / gpio), see
[`docs/cc3501e-companion-commands.md`](../../../docs/cc3501e-companion-commands.md).

## Builds verified (off-silicon)

- **native_sim/native/64** (`twister`, `build_only`) — against the emulated
  SPI controller + `alp,pin-array` in `boards/native_sim_native_64.overlay`.
