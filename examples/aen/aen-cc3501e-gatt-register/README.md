# aen-cc3501e-gatt-register

Bench check for the **NEW CC3501E dynamic GATT registration** path
(#480/#892) on the Alif **M55-HE** (**E1M-AEN801**, Alif Ensemble E8) SoM.
`alp_ble_gatt_register_service()` (`<alp/ble.h>`) used to be a stub; on this
branch it really registers a service on the CC3501E's NimBLE host over the
inter-chip bridge and fills in a real attribute handle per characteristic.

This is a **BENCH-VALIDATION** app, not a customer teaching example -- see
[`aen-cc3501e-companion-tour`](../aen-cc3501e-companion-tour) for the
full-surface, guarded/non-fatal demo of the whole companion API. This app is
a strict PASS/FAIL **gate**: the first step that fails stops the run.

## Peer-free by design

There is no BLE central on this bench, so the app **cannot** do full
central-side service discovery (a real central connecting and reading the
two characteristics by their discovered handles is **HIL-deferred**). What
it proves without a peer:

1. registration returns real (non-zero) attribute handles;
2. the bridge is still alive **after** registration -- the on-silicon check
   for a NimBLE double-`ble_gatts_start()` use-after-free the register path
   used to be able to trigger;
3. the **register-before-advertise ordering guard**: NimBLE's
   `ble_gatts_mutable()` refuses to rebuild the GATT table once the stack is
   advertising, so registering again while advertising must come back
   `ALP_ERR_BUSY`.

## The six-step PASS contract

```
1. cc3501e_bridge_bringup() + PING           -> ALP_OK                (silicon-dependent)
2. alp_ble_open()                            -> non-NULL              (silicon-dependent)
3. alp_ble_gatt_register_service() (2 chars) -> ALP_OK + both         (silicon-dependent)
                                                 handles non-zero
4. a second bridge PING, post-register       -> still ALP_OK          (silicon-dependent;
                                                                        the UAF-fix check)
5. alp_ble_advertise_start() (<=8-char name) -> ALP_OK                (silicon-dependent)
6. register AGAIN while advertising          -> ALP_ERR_BUSY          (silicon-dependent)
```

Every step needs the real CC3501E firmware answering over the bridge, so
every step is silicon-dependent -- there is no host-deterministic
sub-check here (unlike a pure-math unit test). Steps 1/4 are liveness
probes (`PING`); steps 2/3/5/6 exercise the actual BLE surface under test.
Step 6 is expected to take **several seconds** to return: the driver
retries across its full poll budget before the NimBLE
`ble_gatts_mutable()` guard's persistent failure gets remapped to
`ALP_ERR_BUSY` (a fast-path EBUSY signal is a known follow-up, not
implemented here).

A single `RESULT PASS: ...` line means every step held; the first failing
step prints `RESULT FAIL: <which step>` and the app stops there.

## SoM bring-up is one call

Same template as `aen-cc3501e-companion-tour`:
[`src/cc3501e_bridge.c`](src/cc3501e_bridge.c) /
[`cc3501e_bridge.h`](src/cc3501e_bridge.h), copied verbatim -- the
inter-chip wiring (SPI1 on P14_6/5/4, WIFI_EN P15_5, `E_WIFI.NRST`
P15_1_FLEX) is unchanged from that example; see its README for the wiring
table and bench-unverified-overlay notes.

## Console

`diagnostics.console: ram` in `board.yaml` -- this bench rig has no UART
wired to USB, so the app's output goes to the Zephyr RAM console
(`ram_console_buf`), read back over SWD.

## What the bench-runner must build + flash

This app AUTHOR pass does not flash or run anything. To exercise it on
silicon, the bench-runner needs:

- **Host app**: this example, built for
  `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` and flashed to the
  E1M-AEN801's Alif M55-HE, per the two-J-Link recipe in
  `aen-cc3501e-bringup`'s README.
- **CC3501E firmware**: the branch's `feat/480-cc3501e-gatt-register-fw`
  firmware image (built from `firmware/cc3501e` with `build_ti.sh`, per
  `reference_cc3501e_ti_build_local`), flashed to the on-module CC3501E --
  this app exercises the NEW `BLE_GATT_REGISTER` (0x38) firmware handler,
  so an older firmware image will answer with the old stub behaviour, not
  the fix under test.
- Read `ram_console_buf` over SWD for the `RESULT PASS:` / `RESULT FAIL:`
  line (no serial console on this rig).

## Builds verified (off-silicon)

- **native_sim/native/64** (`twister`, `build_only`) -- against the
  emulated SPI controller + `alp,pin-array` in
  `boards/native_sim_native_64.overlay`. This proves the build only; the
  emulated CC3501E backend has no real firmware behind it, so the PASS
  contract above only runs meaningfully on real AEN silicon.
