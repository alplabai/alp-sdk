<!-- SPDX-License-Identifier: Apache-2.0 -->
# alp-console — the Alp SoM interactive console (+ Wi-Fi / BLE / RGB demo)

A single, shippable slot0 application that brings up the **`alp` interactive
console** on the SoM's UART and — on the E1M-AEN801 — wires it to the on-module
**CC3501E Wi-Fi/BLE coprocessor** and a live **RGB status LED**. One app, three
things to show: a Linux-like shell, on-air connectivity, and a "board is alive"
indicator.

The console itself is SDK infrastructure: setting `CONFIG_ALP_SDK_CONSOLE=y`
registers the whole `alp` command tree on the Zephyr shell at link time, so the
app gets `board` / `gpio` / `i2c` / `adc` / `pwm` / `mem` / `clk` / `reboot` /
`companion` diagnostics for free. `main()` registers **no** commands — it only
binds the companion chip and spawns the RGB thread.

## What it demonstrates

| Surface | Command(s) | Backed by |
|---|---|---|
| Interactive shell | `alp …` (Tab to explore) | `CONFIG_ALP_SDK_CONSOLE` |
| SoM identity | `alp board` | HW_INFO + the on-module EEPROM |
| Companion liveness | `alp companion ver` / `ping` | CC3501E bridge (Alif) / GD32 supervisor (V2N) |
| **Wi-Fi scan** | `alp companion wifi scan` | CC3501E `Wlan_Scan` |
| **Wi-Fi connect** | `alp companion wifi connect <ssid> [pass] [wpa3]` | CC3501E STA join |
| **BLE enable** | `alp companion ble enable` | CC3501E NWP controller + NimBLE host |
| **BLE scan** | `alp companion ble scan` | NimBLE `ble_gap_disc` |
| **Soft-AP** | `alp companion wifi ap <ssid> [pass]` | CC3501E AP role + its DHCP server |
| **Serve HTTP** | `alp companion sock serve <port> [seconds]` | CC3501E listening socket (wire protocol v9) |
| RGB status LED | (automatic background thread) | Alif UTIMER PWM (`pwm-leds`) |

### Wi-Fi

`wifi scan` lists nearby APs with channel, RSSI, and **decoded security**
(`open` / `wpa2` / `wpa3`). The security comes from the raw 16-bit TI
`SecurityInfo` — the sec-type bitmap lives in its **high** byte
(`(info >> 8) & 0x3f`), so the host decodes open / WPA2 / WPA3 from there
(`cc3501e_wifi_sec_name`). `wifi connect` takes an SSID and an optional
passphrase (no passphrase ⇒ open; a trailing `wpa3` token forces WPA3-SAE), then
prints the post-join RSSI and the DHCP IP.

```
uart:~$ alp companion wifi scan
5 AP(s):
  MyNetwork                        ch5    -43 dBm  wpa3
  Guest-2.4GHz                     ch3    -59 dBm  wpa2
  IoT-Hub                          ch6    -84 dBm  open
  ...
uart:~$ alp companion wifi connect "MyNetwork" hunter2 wpa3
connecting to "MyNetwork" (wpa3)...
connected  rssi=-44 dBm
ip 192.0.2.74
```

### BLE

`ble enable` brings the CC3501E BLE controller up over the shared Wi-Fi HIF and
starts the Apache NimBLE host. `ble scan` then runs a GAP discovery and lists
advertisers with address, RSSI, and the parsed device name.

```
uart:~$ alp companion ble enable
BLE controller + NimBLE host up
uart:~$ alp companion ble scan
10 device(s):
  66:c6:d2:91:31:26   -91 dBm  ET-2870 Series
  26:2a:8d:21:9d:4d   -50 dBm  (no name)
  ...
```

> **Order matters on older bench firmware:** if a stale CC3501E image returns
> `-4` after back-to-back heavy radio ops, cold-boot the companion and update the
> bridge firmware. Current AEN hardware uses SS0 framing and READY gating, so the
> host link should remain framed across Wi-Fi/BLE radio work.

### Serving over the soft-AP

`sock serve` is the counterpart to `sock tcp-get`: instead of connecting out, it
binds a listening socket and answers inbound HTTP — an embedded web console on a
product with no Ethernet PHY. Start the AP first; `serve` prints the AP-side
address a client should aim at.

```
uart:~$ alp companion wifi ap alp-serve alpserve123
ap "alp-serve" up (wpa2)
uart:~$ alp companion sock serve 80 60
ap ip: 10.0.0.3
listen handle 1
listening on :80 for 60 s (the shell is blocked until then)
[event] opcode 0x2c (len 12)
accepted handle 2
  request: GET /s1 HTTP/1.1
  replied 154 bytes
```

The `opcode 0x2c` line is the inbound connection arriving as an async
`EVT_SOCK_ACCEPTED` event — there is no accept opcode on the wire, because
`accept()` blocks and this bridge is strict request/reply lockstep. The shell is
blocked for the whole window (Zephyr's shell has no cancellation hook, so ctrl-c
does not cut it short).

> **Known limitation, not caused by the listening path:** back-to-back **client**
> socket operations in AP mode are unreliable on current bridge firmware — the
> first `sock tcp-get` succeeds and the next one fails (`-1` / `-4`), and a third
> can leave the link needing a cold boot. Reproduced identically on the
> pre-v9 firmware with no listening socket in play, so serving and fetching in
> the same session is the case to avoid for now.

### RGB status LED

A low-priority background thread breathes the on-board RGB LED through a rainbow
(Alif UTIMER PWM, board overlay's `pwm-leds` children: `led_red` = PWM3/P2_4,
`led_green` = PWM0/P12_7, `led_blue` = PWM1/P12_6). The shell stays fully
responsive while it runs. Note: those DT node *labels* don't match the
physically bench-measured colour (2026-07-28, see
`metadata/boards/e1m-evk.yaml`) — PWM3/P2_4 is physically green and
PWM0/P12_7 is physically red — but the thread drives all three channels in a
continuous phase-shifted rainbow with no per-colour meaning, so it's left
unrenamed; see the comment in `src/main.c`.
The thread is device-tree-guarded on `led_red`, so on a board without those PWM
nodes (native_sim, V2N) it compiles out and nothing is spawned.

## Build & flash (E1M-AEN801)

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
           -d build_console examples/peripheral-io/alp-console
```

The app is slot0-linked (`CONFIG_FLASH_LOAD_OFFSET=0x10000`, reset vector
`0x8001xxxx`), so it flashes via the **Flow D** MRAM path: `app-gen-toc` builds a
signed ATOC, then J-Link `loadbin`s the image to `0x80010000` and the ATOC to its
per-build start address. See `docs/aen-bench-bringup.md` and the
`flashing-and-bench-debugging-aen` workflow for the bench recipe. The companion
CC3501E must be running its bridge firmware (`docs/cc3501e-production.md`).

## Portability

`main()` is identical across targets; only the companion bind differs:

- **Alif (AEN801):** no companion singleton, so `main()` opens the CC3501E
  (`cc3501e_bridge_bringup`, copied into `src/cc3501e_bridge.{c,h}`) and hands the
  handle to the console — `alp companion …` then reaches Wi-Fi/BLE. Guarded by
  `CONFIG_ALP_SDK_CHIP_CC3501E`.
- **V2N:** the GD32 supervisor is a managed singleton inside the SDK — `main()`'s
  bind is a no-op and `alp companion` reaches the GD32 automatically (its
  `gpio` sub-commands replace the CC3501E `wifi`/`ble` ones).
- **native_sim:** no companion at all; the console + all non-companion commands
  still work, so the example builds and runs everywhere.

> `CONFIG_ALP_SDK_CONSOLE_UNSAFE=y` (in `prj.conf`) unlocks the write-capable
> verbs (`mem wr`, `gpio write`, `companion gpio write`). Leave it **off** in
> field builds.
