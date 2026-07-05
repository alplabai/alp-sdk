# aen-cc3501e-companion-tour

The **capstone** full-surface example for the on-module **TI CC3501E**
Wi-Fi 6 + BLE 5.4 coprocessor, driven from the Alif **M55-HE** core on the
**E1M-AEN801** (Alif Ensemble E8) SoM.

Where [`aen-cc3501e-bringup`](../aen-cc3501e-bringup) is the *minimal* power +
`PING` soak, this app walks the **whole companion API** in one linear
sequence — as living documentation of the hand-written-firmware path. Every
call goes through the portable driver in
[`<alp/chips/cc3501e.h>`](../../../include/alp/chips/cc3501e.h) over the
inter-chip SPI1 bridge; the app never touches the raw wire protocol.

## The tour

```
init  ->  ping  ->  diag info
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

## Wi-Fi credentials + socket target (build-time)

Credentials are **deliberately empty by default** — never hardcode them in a
public example. Set them at build time without editing the source:

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
  examples/aen/aen-cc3501e-companion-tour -- \
  -DEXTRA_CFLAGS="-DTOUR_WIFI_SSID=\\\"myssid\\\" -DTOUR_WIFI_PASS=\\\"mypass\\\""
```

When `TOUR_WIFI_SSID` is empty the connect / IP / socket steps are skipped
(they need an association) and the tour jumps to the BLE section. The TCP
destination defaults to `93.184.216.34:80` (`TOUR_TCP_IP` / `TOUR_TCP_PORT` in
`src/main.c`) — change it to a host reachable from your bench network.

## SoM bring-up is one call

The hardware setup (control pins + inter-chip SPI + power/reset sequence) is
`cc3501e_bridge_bringup()` in
[`src/cc3501e_bridge.c`](src/cc3501e_bridge.c) — the reusable SoM template
shared with the bringup example. The proxied-GPIO step uses the E1M-AEN route
table in [`src/cc3501e_gpio_routes.c`](src/cc3501e_gpio_routes.c) (built with
`CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y`): `alp_gpio_open(ALP_E1M_GPIO_IO15)`
routes over the bridge while every other pin delegates to the platform GPIO
driver.

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
