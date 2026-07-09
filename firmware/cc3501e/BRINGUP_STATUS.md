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

**Activation state — CORRECTED 2026-07-09 (`e1m-aen-evk-01`, XDS110 `L50015YR`):**
the bench unit is **already activated**. The `boot_sector_programmed = 0`
figure that an earlier read reported is from a **stale, pre-activation
baseline** (`activation_report.txt`, dated 2026-07-03 17:32 — the
factory/pre-provision snapshot), NOT a current device read. The authoritative
current state is the device fuse read-backs: **30 `programming_report.txt`
dumps across 2026-07-05 (05:26 → 21:48) all read `boot_sector_programmed = 1`,
`non_recoverable_failure = 0`** — i.e. the boot sector was programmed sometime
between Jul 3 and Jul 5 and has read as programmed ever since. The auth fuses
are set and `permanently_lock_debug_enable = 0` (debug open). So the vendor SBL
is armed; **cold swap-boot should be exercisable directly — no re-activation is
needed on this unit.**

Caveat on re-confirming live: a fresh `get_fuse_data` today via the vendor-key
path is blocked at the toolbox's "Action Required: Update Signing Module" RoT
gate (a tooling limitation, not a fuse-0 signal). A clean live re-read would use
a signed `query` action request (`flash-images-builder build action_request
--type query` → sign → `programmer … query`); the 30 historical device reads are
already consistent at `1`.

### Re-activating a fresh / mis-activated unit (only if needed)

Not needed for the current bench unit (already activated). For a genuinely
fresh or mis-activated unit, programming the cold-launch boot sector re-arms the
vendor SBL — a **one-time, hard-to-reverse** operation. Confirm
`permanently_lock_debug_enable = 0` first, then use the full signed flash set
(`programming_image` + `action_requests` + `vendor_image` + `boot_sector`, all
signed with the VALIDATION key), programmed via `simplelink-wifi-toolbox
programmer -i XDS110 -param1 L50015YR programming`. `deploy_validate.sh` alone is
**insufficient** — it refreshes only `vendor_image`. NOTE: a ready-to-run
full-set-regen script is **not** currently staged in the bench signing dir; the
`gen-out-*/` trees are prior outputs, not a reproducible command. Confirm by
re-reading the fuse: `boot_sector_programmed` `0 → 1`.

### Cold swap-boot cycle — bench result 2026-07-09 (#493 criterion 1: STAGED proven, SWAP fails)

Ran the real-image OTA cycle on `e1m-aen-evk-01` (E8 slot0 = the
`-DCC3501E_OTA_REAL=ON` app, SE-UART `app-write-mram`). Result:

- **Real-image STAGED: PROVEN.** The genuine signed candidate (31428 B,
  v0.0.4.0 GPE) streamed over the bridge and `psa_fwu` accepted it:
  `OTA status: state=2 written=31428/31428 B`, `OTA -> STAGED (genuine image
  accepted by psa_fwu)`. This is the real-image confirmation the inert blob
  never gave. The STAGED image **persisted across a verified true cold POR** (a
  second `cc3501e_ota_update` after the POR returned `-1`/INVAL because a staged
  image was already pending).
- **Cold swap-boot: FAILED.** After a verified true cold POR (PSU power-cycle,
  power drop + `Cortex-M55 identified` re-up both confirmed on the J-Link), the
  CC3501E booted its PRIMARY slot **unchanged** — `GET_VERSION -> protocol v1`
  (host expects v3), `fw_version=0x0001`, identical to before. The STAGED image
  was **not promoted** to primary. No accept/rollback/trial observed (no swap
  occurred).

**ROOT CAUSE (root-cause pass): the bench procedure was wrong, not (yet) a
silicon block.** The STAGED→primary swap is completed by the CC35 firmware's
**own `psa_fwu_request_reboot()`** after FINISH (the deferred `ota_reboot_pending`
latch: armed at the end of `ota_do_finish()`, fired in `cc3501e_hw_tick()`), NOT
by a host PSU cold POR. A bare PSU cycle carries no swap request, so the SBL
cold-boots straight into the unchanged primary and leaves STAGED inert. The
SELFTEST path proves the pattern — `cc3501e_ota_install()` does `psa_fwu_install()`
→ **immediately** `psa_fwu_request_reboot()`, never "install then wait for an
external POR". "SBL not armed" is refuted by the 2026-06-17 cold-revert evidence
in `cc3501e_hw_ti.c` (cold power-on actively reverted unconfirmed TRIAL images →
the SBL demonstrably evaluates FWU state at cold POR). Caveat: the Puya
host-hard-reset-after-power-cycle workaround + the warm/debug-launch trailer-check
bypass make a bare PSU POR **doubly** invalid as the promotion trigger on this
unit.

**Corrected procedure — the discriminating test (one OTA cycle, no new tooling):**
re-run `cc3501e_ota_update`; the moment the host prints STAGED, **do NOT touch the
PSU** — loop PING/GET_VERSION for ~30–60 s and watch the READY line/console.
Three conclusive outcomes:

1. **Bridge drops and comes back at protocol v3** → the mechanism works; the swap
   was procedural (PSU POR instead of the firmware self-reboot). Then confirm
   permanence: the new image's first tick must `psa_fwu_accept()`, after which one
   true cold POR should retain v3. **→ closes #493 criterion 1.**
2. **Bridge drops, comes back still v1, STAGED still pending** → the requested
   reboot fired but the SBL didn't honor it → escalate to activation: unblock the
   toolbox RoT "Update Signing Module" gate for one live `get_fuse_data`, or repeat
   on a second correctly-activated unit.
3. **No self-reboot at all** → firmware defect: `psa_fwu_request_reboot()` returns
   without effect and its rc is discarded (`cc3501e_hw_tick()`, + the sibling calls
   in `cc3501e_ota_install()`/accept). Fix = check + surface those rcs via
   console/`GET_DIAG_INFO` so armed/requested/refused are distinguishable, and
   investigate the FWU-service state that refused the reboot.

**#493 criterion 1 is NOT yet closed.** Corrected-procedure run 2026-07-09: the
test was **blocked upstream** — a genuine STAGED image from a prior run is stuck
pending in the CC35 secondary slot, and there is **no non-destructive way to
clear or promote it over the bridge**:

- A fresh `cc3501e_ota_update` short-circuits to `-1` (INVAL, "slot already
  pending") **before** `OTA_FINISH`, so `ota_reboot_pending` is never armed and
  `psa_fwu_request_reboot()` is never issued — the test cannot reach any of the
  three outcomes above.
- `OTA_ABORT` (protocol.c) cancels only an **in-flight** session; it does not
  clear a committed STAGED image. There is no OTA reject/erase opcode, and the
  staged image survived a verified true cold POR — so nothing on the bench frees
  the slot.
- **Chicken-and-egg:** a fresh FINISH needs a free slot; the slot frees only via
  a swap; the swap needs the request armed by a fresh FINISH.

Corroboration (strengthens the root cause): across **two** bare-nRESET CC35
reboots (the E8 `cc3501e_bridge_bringup` Puya WIFI_EN+nRESET workaround, one per
E8 boot) the image stayed pending, un-promoted — confirming a bare reset carrying
no `psa_fwu_request_reboot()` does NOT promote.

**The unblock is CC35-firmware-side** (needs a TI-toolchain rebuild + reflash):
either (a) add a bridge OTA reject/erase path (or make `psa_fwu_start` replace a
pending image) so a fresh FINISH is reachable, or (b) add a
"request-reboot-for-existing-pending-image" bridge command that calls
`psa_fwu_request_reboot()` on the already-committed image — the cleaner test,
since it directly promotes the stuck v3 image and exercises the swap path. Until
one lands, crit-1 stays blocked on this jammed slot. (The exact PSA "already
pending" error mapped to host `-1` lives in the license/vendor FWU path, not this
repo's `src/` — TBD.)

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
