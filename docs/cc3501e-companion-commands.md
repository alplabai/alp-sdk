<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Alp Lab AB -->

# CC3501E companion command + API reference

The on-module **TI CC3501E** Wi-Fi 6 + BLE 5.4 coprocessor is driven from
the Alif host through the portable driver in
[`<alp/chips/cc3501e.h>`](../include/alp/chips/cc3501e.h)
(`chips/cc3501e/cc3501e.c`) over the inter-chip SPI bridge. Two ways in:

- **Interactively** — the `alp companion` command tree on the Zephyr shell
  (the companion console backend under `src/zephyr/console/`). This page is
  its reference.
- **From firmware** — the `cc3501e_*` C API directly (see the
  [`aen-cc3501e-companion-tour`](../examples/aen/aen-cc3501e-companion-tour)
  example, which walks the whole surface in sequence).

> The same `alp companion` group binds the **GD32** supervisor on V2N SoMs
> (`CONFIG_ALP_SDK_V2N_SUPERVISOR`) instead of the CC3501E; there it exposes
> `companion gpio read/write` rather than the Wi-Fi/BLE tree below. This page
> documents the **CC3501E (Alif)** binding.

---

## Enabling the console

```kconfig
CONFIG_ALP_SDK_CONSOLE=y               # the alp command tree
CONFIG_ALP_SDK_CONSOLE_CMD_COMPANION=y # the `alp companion` group
CONFIG_SHELL=y                         # Zephyr shell backend
```

The application registers its live CC3501E handle once at boot via
`alp_console_companion_set()` (after `cc3501e_bridge_bringup()`); until then the
commands report the bridge is not ready. See
[`console.md`](console.md) for the console's safety tiers
(`CONFIG_ALP_SDK_CONSOLE_UNSAFE` gates the write-side commands).

---

## Meta

| Command | What it does |
|---|---|
| `alp companion ver` | Read the companion firmware / protocol version. |
| `alp companion ping` | Liveness round-trip (cheapest is-it-alive probe). |
| `alp companion reset` | Soft-reset the CC3501E firmware in-band (the link drops; re-sync after). |
| `alp companion bench [n]` | Time `n` `GET_VERSION` round-trips over the bridge. |

## `alp companion wifi`

| Command | What it does |
|---|---|
| `wifi scan` | Scan for Wi-Fi APs and list SSID / channel / RSSI / security. |
| `wifi connect <ssid> [pass] [wpa3]` | Associate as a station (omit `pass` for open; `wpa3` selects SAE). |
| `wifi disconnect` | Tear down the STA association. |
| `wifi ap <ssid> [pass] [wpa3]` | Start a soft-AP (omit `pass` for an open AP). |
| `wifi ap-stop` | Stop the soft-AP. |
| `wifi status` | Show connection state + RSSI + IP. |

## `alp companion ble`

| Command | What it does |
|---|---|
| `ble enable` | Bring up the BLE controller + NimBLE host. |
| `ble disable` | Tear the BLE controller + host back down. |
| `ble scan` | Scan for advertisers (needs `ble enable` first). |
| `ble scan-stop` | Stop an in-progress scan. |
| `ble adv` | Start connectable advertising (fixed interval). |
| `ble adv-stop` | Stop advertising. |
| `ble connect <aa:bb:cc:dd:ee:ff> [random]` | Central-connect to a peer (`random` = random address type). |
| `ble disconnect` | Drop the active BLE connection. |
| `ble gatt register <hexbytes>` | Register an opaque GATT attribute table. |
| `ble gatt read <handle>` | Read a GATT attribute value. |
| `ble gatt write <handle> <hexbytes>` | Write a GATT attribute value. |
| `ble gatt notify <handle> <hexbytes>` | Send a GATT notification. |

## `alp companion diag`

| Command | What it does |
|---|---|
| `diag info` | Firmware version / reset cause / uptime / active role / free heap. |
| `diag stats` | Frame counters (frames answered OK / with an error). |
| `diag loglevel <0..255>` | Set the firmware log verbosity. |

## `alp companion sock`

| Command | What it does |
|---|---|
| `sock tcp-get <ip> <port> <path>` | Open a TCP socket, issue an HTTP/1.0 `GET`, and print the reply. |

The socket primitives underneath `tcp-get` — `open`, `connect`, `send`,
`recv`, `close` — are available from firmware as the `cc3501e_sock_*` API
(the console exposes only the composed `tcp-get` helper).

---

## Host-driver-only surfaces (no console command)

Two companion subsystems are driven from firmware through the `cc3501e_*` API
rather than the console:

### OTA firmware update (`cc3501e_ota_*`)

Stream a signed CC3501E vendor image over the bridge into the coprocessor's
non-primary slot, which it then swaps on reboot (PSA-FWU). Driven by the
device-side Mender contract, not interactively.

| API | What it does |
|---|---|
| `cc3501e_ota_update(ctx, image, len, tmo)` | Full cycle: `BEGIN` → chunked `WRITE` → `FINISH`. |
| `cc3501e_ota_begin` / `_write` / `_finish` | Granular streaming controls (`tcp-get`-style composition). |
| `cc3501e_ota_abort` | Cancel an in-flight session (back to IDLE). |
| `cc3501e_ota_status` | Session state + bytes-written cursor + declared total (for resume). |

See [`cc3501e-bridge.md`](cc3501e-bridge.md) "OTA" and
[`cc3501e-production.md`](cc3501e-production.md).

### GPIO proxy — via the portable `<alp/gpio>` API

The CC3501E fronts a set of E1M IOs (IO11 / IO13 / IO15..IO21) and the two
camera-enable LDOs. With `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y` and a populated
route table (`cc3501e_gpio_routes[]`), `alp_gpio_open(ALP_E1M_GPIO_IOxx)` for a
mapped IO routes over the bridge while every other pin delegates to the
platform GPIO driver — the application code is identical either way. The raw
`cc3501e_gpio_configure` / `_write` / `_read` / `_set_interrupt` +
`cc3501e_cam_enable` / `cc3501e_power_policy` calls are also available directly.
See [`cc3501e-gpio-bench.md`](cc3501e-gpio-bench.md).
