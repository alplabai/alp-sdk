<!--
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Alp Lab AB
-->

# CC3501E bridge bring-up status

Status of the Alif Ensemble E8 (M55-HE) <-> CC3501E (CC35X1E) SPI bridge on
the E1M-AEN801 bench. Updated 2026-07-08.

This is the consolidated on-silicon record for the **link / Wi-Fi / BLE**
pillars. The authoritative topology is the hardware-framed SPI bridge described
in [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md): Alif `SPI1_SS0_C`
frames every protocol phase and READY gates reply phases. HOST_IRQ / async
event delivery remains future work.

## TL;DR - pillar status

| Pillar | State | Evidence / remaining work |
|---|---|---|
| **Inter-chip link** (PING / GET_VERSION / GET_MAC / RESET) | PASS, cold + warm | Hardware SS0 + READY framing is bench-validated on E1M-AEN801; `ver` remains responsive after radio ops. |
| **Wi-Fi GET_MAC / scan / RSSI** | PASS | Real scan records with security decode validated through the bridge. |
| **Wi-Fi connect-STA / soft-AP / sockets** | PASS for async STA connect; socket APIs shipped | Async connect survives the bridge; keep credentialed socket soak in production validation. |
| **BLE** (enable / advertise / scan / connect + GATT scaffolding) | PASS for enable + real scan | NimBLE enable and `ble_gap_disc` scan validated with real advertisers; full runtime GATT/event parity remains v1.0 work. |
| **CAM enables** | PASS | `which` 0 -> GPIO_1 (LDO0), 1 -> GPIO_0 (LDO1); mapping fixed from U4 pins 54/55. |
| **GPIO proxy** + camera enables | PASS | Firmware HAL, host API, portable proxy, ztests, and warm-boot GPIO example are validated. |
| **Cold-boot** | Host-workable | Puya 64 Mbit flash workaround is host hard-reset after every power-cycle. Correctly activated production units still need cold swap-boot validation. |
| **OTA over SPI** | Stage/install PASS; final cold swap-boot gated | BEGIN -> WRITE(RAM-stage) -> FINISH(one flash burst -> `psa_fwu_install` -> STAGED) is silicon-validated; final swap needs a correctly activated, cold-bootable unit. |

## 1. Inter-chip link

The current E1M-AEN bridge is not the early bring-up three-pin assumption. It
uses:

| Net | Alif side | CC3501E side | Role |
|---|---|---|---|
| SCLK | `P14_6` / `SPI1_SCLK_C` | `GPIO_27` | SPI clock from the Alif master |
| MOSI | `P14_5` / `SPI1_MOSI_C` | `GPIO_29` | CC3501E SPI0 data in |
| MISO | `P14_4` / `SPI1_MISO_C` | `GPIO_28` | CC3501E SPI0 data out |
| SS0 | `P14_7` / `SPI1_SS0_C` | `GPIO_16` CSN resource | Hardware chip-select per protocol phase |
| READY | `P2_6` | `GPIO_17` | Slave armed / reply phase ready |

The wire frame remains a 4-byte header plus payload. A command/reply exchange is
split into four hardware-SS0-framed phases:

| # | Master clocks | Direction | Length |
|---|---|---|---|
| 1 | request header | MOSI | 4 |
| 2 | request payload | MOSI | `payload_len` from phase 1 |
| 3 | reply header | MISO | 4 |
| 4 | reply payload | MISO | reply `payload_len` from phase 3 |

The host waits for READY before reply phases, and the CC3501E backend advances
on `SPI_TRANSFER_COMPLETED`. `SPIWFF3DMA_CMD_RETURN_PARTIAL_ENABLE` stays
disabled because hardware SS0 already frames each transfer and the extra CSN
deassert callback double-advances the READY state machine.

## 2. Wi-Fi

- **GET_MAC** uses the SimpleLink host path and is validated cold through the
  bridge.
- **Scan / RSSI** is worker-routed. The bridge path returns real AP records and
  security decode; an empty result should now be treated as an RF/environment
  question, not as bridge evidence by itself.
- **STA connect** is asynchronous and validated across the bridge: association
  no longer wedges the link, and a `GET_VERSION`/`ver` check after connect still
  responds.
- **Socket APIs** are implemented; keep credentialed socket soak in production
  validation because it depends on local network availability.

## 3. BLE

The 512 KB DRAM linker fix removed the old false "needs PSRAM" conclusion.
Wi-Fi + BLE coexist in the CC3501E image, NimBLE enable is validated, and real
BLE scan records are observed through the bridge.

Remaining BLE work is API completeness, not the bridge link: HOST_IRQ-backed
async event delivery and full runtime GATT/event parity belong to the v1.0
workstream.

## 4. Bridge / radio coexistence

Radio operations can still temporarily disrupt the CC35 host-DMA client used by
the SPI slave. The production model is:

1. Submit the radio operation from the Alif host.
2. Run the slow SimpleLink body on the CC3501E worker, off the SPI callback.
3. Re-open and re-arm the bridge SPI after the radio operation.
4. Let the host poll/retry across `ALP_ERR_IO` / BUSY until the result is ready.

READY gates per-phase traffic once the SPI slave is armed. It is not a
replacement for HOST_IRQ async-event push delivery.

## 5. Cold-boot

Root cause remains a TI-SDK path around the Puya 64 Mbit flash on the bench
unit. The validated host-side workaround is to hard-reset the CC3501E after each
power-cycle: drive WIFI_EN, let the first boot settle, then pulse nRESET. This
is implemented in `cc3501e_hard_reset` / `cc3501e_reset`.

## 6. GPIO proxy

GPIO proxy and camera-enable opcodes are shipped. The firmware guards reserved
bridge/UART/unbonded pads, the host exposes `cc3501e_gpio_*`, the portable
backend delegates non-CC3501E pins to the platform backend, and the warm-boot
GPIO example has bench coverage.

## 7. OTA

OTA over the bridge is RAM-staged by design: each WRITE copies into RAM, and
FINISH performs one flash burst plus `psa_fwu_install`. This avoids repeatedly
tearing down the bridge DMA during a long image stream. Stage/install is
bench-validated; the final cold swap-boot remains gated on a correctly
activated, cold-bootable unit.

## 8. Open items / next

1. **HOST_IRQ / async events** - add the board line and host event-drain path for
   BLE/Wi-Fi/GPIO unsolicited events.
2. **Full runtime GATT/event parity** - finish the v1.0 portable BLE event
   surface once HOST_IRQ exists.
3. **Credentialed socket soak** - run against a lab network during production
   validation.
4. **OTA cold swap-boot** - repeat final swap validation on a correctly
   activated cold-bootable CC3501E unit.
5. **`flash.py` real flashing** - replace manual SWD/J-Link when TI's
   `cc3501e-flasher` CLI becomes public.
