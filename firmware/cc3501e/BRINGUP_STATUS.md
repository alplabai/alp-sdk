<!--
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Alp Lab AB
-->

# CC3501E bridge bring-up status

Status of the Alif Ensemble E8 (M55-HE) <-> CC3501E (CC35X1E) SPI bridge on
the E1M-AEN801 bench. Updated 2026-08-28.

This is the consolidated on-silicon record for the **link / Wi-Fi / BLE**
pillars. The authoritative topology is the hardware-framed SPI bridge described
in [`docs/cc3501e-bridge.md`](../../docs/cc3501e-bridge.md): Alif `SPI1_SS0_C`
frames every protocol phase and READY gates reply phases. Async events ride an
attention edge on that same READY wire (#130, alp-sdk#1721) -- shipped and
silicon-validated, build-time opt-in, and with Wi-Fi connect/disconnect as its
only producers so far (see § 4).

## TL;DR - pillar status

| Pillar | State | Evidence / remaining work |
|---|---|---|
| **Inter-chip link** (PING / GET_VERSION / GET_MAC / RESET) | PASS, cold + warm | Hardware SS0 + READY framing is bench-validated on E1M-AEN801; `ver` remains responsive after radio ops. |
| **Wi-Fi GET_MAC / scan / RSSI** | PASS | Real scan records with security decode validated through the bridge. |
| **Wi-Fi connect-STA** | PASS | Async connect survives the bridge; re-confirmed 2026-08-28 on fw 0.4.0 (`state: connected`, `rssi=-35 dBm`, DHCP lease). Association is INTERMITTENT in practice -- several attempts returned `wifi connect ... timed out` or `-5` and needed a cold cycle. |
| **Sockets** | **FAIL - does not connect** | `sock tcp-get` cannot open a TCP connection to ANY destination on fw 0.4.0 / protocol 5: LAN targets return `-1` (`ALP_ERR_INVAL`), public ones `-4` (`ALP_ERR_TIMEOUT`), while the same targets fetch fine from the host on the same LAN. `sock_open` succeeds (`handle=1`); the firmware emits no new error (`lasterr` stays `2` = `RESP_ERR_BUSY`), so the failure is host-side or a worker that never completes the connect job. **alp-sdk#1746 -- do not treat sockets as shipped.** |
| **Soft-AP** | Partial / unresolved | Advertises and keeps advertising well past the ~100 s of alp-sdk#1562 (cache-proof measurement, 2026-08-28), but a client could not ASSOCIATE to it across 181 s while it was confirmed still beaconing. One client only; needs a second radio to confirm. `ap start` also returns `-4 unconfirmed` -- there is no AP status latch (alp-sdk#1385), so AP state must be checked out of band. |
| **BLE** (enable / advertise / scan / connect + GATT scaffolding) | PASS for enable + real scan (re-confirmed 2026-08-28: 9-15 real advertisers) | NimBLE enable and `ble_gap_disc` scan validated with real advertisers; full runtime GATT/event parity remains v1.0 work. |
| **CAM enables** | PASS | `which` 0 -> GPIO_1 (LDO0), 1 -> GPIO_0 (LDO1); mapping fixed from U4 pins 54/55. |
| **GPIO proxy** + camera enables | PASS | Firmware HAL, host API, portable proxy, ztests, and warm-boot GPIO example are validated. |
| **Cold-boot** | Host-workable | Puya 64 Mbit flash workaround is host hard-reset after every power-cycle. |
| **OTA over SPI** | PASS - full cycle | BEGIN -> WRITE(RAM-stage) -> FINISH(one flash burst -> `psa_fwu_install` -> STAGED) -> swap is silicon-validated. **#493 criterion 1 CLOSED 2026-07-10**: the full cold-swap cycle is proven on E8 (see §5). Forward-version candidates only - the CC35 refuses a version rollback. |

## 0.5 Current state (2026-08-28)

**Shipping version.** App SemVer **0.4.0** (`fw_version=0x0400` over `GET_DIAG_INFO`),
wire protocol **5**, prebuilt `prebuilt/cc3501e-v0.4.0.bin` GPE-stamped `0.4.0.0`.
Verified on E1M-AEN801: `GET_VERSION -> protocol v5 (host expects v5) -- match`,
20/20 soak PINGs, `GET_MAC ok 44:3e:8a:10:b6:9e`, `WIFI_SCAN ok -> 6 AP(s)`.

**Three version numbers, not interchangeable** -- app SemVer (`0.4.0`), wire protocol
(`5`), and the GPE anti-rollback stamp (`0.4.0.0`) burned irreversibly into the part.
Conflating them has repeatedly cost bench time; see `prebuilt/CHANGELOG.md`.

### Fixed since the last revision

- **Phantom async events (alp-sdk#1740).** An empty event ring produced ~5.8
  `opcode 0x00, len 0` events/second forever. Reply padding is folded into the
  declared payload length, so an empty ring arrived as 7 zero bytes and was walked
  as three entries. The host walk now stops at a zero opcode. **Any new
  variable-length reply payload must be self-delimiting**, or it hits the same trap.
- **Shell stack overflow (alp-sdk#1743).** `sock tcp-get` overflowed the 2 KB Zephyr
  shell stack and halted the board; `CONFIG_SHELL_STACK_SIZE` is now 4096 when the
  `alp` console is enabled.

### Open, with measurements

- **Sockets do not connect (alp-sdk#1746).** See the TL;DR row. This is the single
  biggest gap: the socket surface is documented and shipped but non-functional.
- **One BLE scan costs exactly 100 frame errors (alp-sdk#1754).** `frames ok=15 err=0`
  -> one `ble scan` -> `err=100`, then frozen at 100 while `ok` keeps climbing. The
  counter is a plain `uint32_t` with no saturation, so 100 is a real count -- a bounded
  loop somewhere, source not yet found.
- **Throughput (alp-sdk#1677) is unmeasurable** until sockets work; there is no way to
  move bulk data over the bridge today. Round-trip latency is healthy:
  `200 GET_VERSION ops in 120 ms = 600 us/op, 1666 ops/s, fails=0`.

### Bench facts worth knowing before debugging

- **A wedged bridge needs a LONG power-off.** After heavy use the link can refuse
  everything with `-5`, including `alp companion reset` itself (an in-band reset over
  a wedged link cannot work). A **7-8 s** cold cycle does NOT clear it; **20 s does**,
  immediately. Do not conclude the part is dead before trying that.
- **The wedge does not reproduce under control.** 6/6 connect->status->disconnect
  cycles and 6/6 with a `sock tcp-get` interleaved both ran clean from a cold boot.
  Treat "wedges after N connects" as unproven.
- **Interactive `alp companion` verbs need the right app.** `aen-cc3501e-bringup` never
  calls `alp_console_companion_set()`, so every companion command answers `companion not
  registered`. Use `examples/peripheral-io/alp-console` (alp-sdk).

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

Remaining BLE work is API completeness, not the bridge link. The attention
transport for async events shipped (#130, alp-sdk#1721), but nothing pushes BLE
events into the ring yet, so BLE async delivery plus full runtime GATT/event
parity belong to the v1.0 workstream.

## 4. Bridge / radio coexistence

Radio operations can still temporarily disrupt the CC35 host-DMA client used by
the SPI slave. The production model is:

1. Submit the radio operation from the Alif host.
2. Run the slow SimpleLink body on the CC3501E worker, off the SPI callback.
3. Re-open and re-arm the bridge SPI after the radio operation.
4. Let the host poll/retry across `ALP_ERR_IO` / BUSY until the result is ready.

READY gates per-phase traffic once the SPI slave is armed. Since #130 /
alp-sdk#1721 the same wire also carries the async-event attention edge -- the
firmware pulses it when it queues an event and the host drains on the edge.
Sharing one wire for both is why the drain needs a 50 ms empty-drain backoff: a
transaction end is indistinguishable from an attention pulse, and without the
backoff 86 events cost 11888 ISRs instead of 220.

Two limits apply before relying on it. The pulse is a **build-time opt-in**
(`build_ti.ps1 -AttnPulse`, `-DCC3501E_ATTN_PULSE=1`), default OFF because the
wire is a rev-1 bodge absent on the stock EVK and `package_cc3501e_prod.ps1`
does not pass it -- a build without the flag links a no-op stub and the host
falls back to its timer poll. And only `EVT_WIFI_CONNECTED` /
`EVT_WIFI_DISCONNECTED` call `event_ring_push()`; `EVT_GPIO_INTERRUPT` and the
BLE events have no producer, so they still never arrive.

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

### Verifying a CC35 flash ACTUALLY committed (hard-won, 2026-07-12)

**`primary_vendor_image_validate_pass` / `*_done` in `programming_report.txt` are
NOT a commit signal — they read `0` even on the known-good REF_SET.** Do not trust
them (they cost multiple bench sessions of false "it worked" / "it failed"). The
**only ground truth is the XDS110 `query` image table**: `programmer -i XDS110
-param1 <SN> query --query_action_req_path <signed query_action_request>`. A
non-empty table with valid flash magic = an image is committed + booted. **But an
empty table does NOT necessarily mean "nothing written / dead SE"** — it can also be
a **rollback-blocked image that streamed fine but the SBL refused to BOOT** (see
below), so it never registered. The truly definitive commit check is: after
programming, cold-POR and **boot the CC35, then read PING / GET_VERSION** — if the
bridge answers, it committed. `validate_pass`/`*_done` remain red herrings either way.
A cheap WARM-path guard (`deploy_validate.sh`) is the streamed byte count — the
stale/image-coupled no-op streams only ~1.3 KB vs the full vendor image.

### The #1 cause of "streams clean but dead link" = a VERSION ROLLBACK

The CC35 SBL enforces GPE-version **monotonicity against the last-seen version** —
even when all the rollback-protection **fuses read 0** (`get_fuse_data`). Program a
vendor image at a version **lower** than anything ever flashed and it streams clean
(exit 0, full ~1.09 MB) but the SBL **refuses to boot it** → dead link, and the
XDS110 `query` image table stays **empty** (a never-booted image never registers).
This is easy to misread as "the SE won't commit / beyond software recovery" — it is
NOT. (Exactly that happened on `e1m-aen-evk-01` 2026-07-12: recovery sets at v0.83
were rollbacks vs an earlier v0.99 → dead; **reprogramming a full set at v0.250.0.0
(major=0, > anything ever flashed) with the validation key + a cold POR revived it**
— PING ok, protocol v4, Wi-Fi 5 APs, persisted across the cold POR.)

**Rules:** (1) VERSION must be monotonically ≥ anything ever flashed on the unit, and
**major=0** (a GPE major ≥ 1 fails BL2 secure-boot AUTH). (2) The correct reflash flow
is **WARM programming-only** — NO `activation` (activation on a DEPLOYED part is
rejected `Life cycle DEPLOYED is not valid`; it was never the missing step). (3) Keys:
the part's `rot` fuse = the **DER-SPKI sha256 of the validation public key**; confirm
with `get_fuse_data --activation_type vendor_key`. (4) Still don't over-reflash need­lessly
(each signed program+cold-cycle stresses the SE) — but a dead bridge is almost always a
**version rollback or a never-booted image**, not a dead SE or a physical fault. (A true
whole-carrier cold POR is a DPS-150 `power off/on` — verified: E8 J-Link VTref 1.786 →
0.000 → 1.786 V; the old "DPS doesn't cold-boot" note was a stale-telemetry artifact.)

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

**#493 CRITERION 1 — CLOSED (2026-07-10).** The full OTA cold-swap cycle is
silicon-proven on E8: a FORWARD candidate (the plain radio-free bridge signed at
GPE **v0.90.0.0**, above the primary) streamed → `state=2 written=37016/37016 B`
**STAGED** → the CC35's own `psa_fwu_request_reboot()` swapped it (bridge dropped
~2 s, then returned) → post-swap the CC35 runs the radio-free candidate
(`WIFI_SCAN`/`GET_MAC`/`BLE` go NOT_READY where pre-swap found 5 APs — the proof)
→ **self-accepted and PERSISTED across a true cold POR** (no rollback). The key
was a candidate version ABOVE the primary: a downgrade (the old v0.0.4.0
candidate) is refused at `psa_fwu` install (`state=3` ERROR), a forward one is
accepted. Regenerate the forward candidate with `firmware/cc3501e/ti/build_ti.sh`
(plain) → `build+sign vendor_image --version <above primary>` → bin2c (recipe in
`cc3501e_ota_candidate.c`). **Known follow-up (non-blocking):** the FIRST OTA
attempt after a failed/aborted OTA can return `-1` and wedge the bridge until a
CC35 reset — the `OTA_BEGIN` clear does not cover every stuck slot state; the
forward image stages reliably from a clean slot.

--- history (how we got here) ---

Corrected-procedure run 2026-07-09: the
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

**The unblock is now IMPLEMENTED (proto v4): `OTA_PROMOTE` (opcode 0x46).** It
arms the same deferred swap-reboot `FINISH` uses, for an image already committed
to STAGED — so the pending slot can be promoted without a fresh session (which is
unreachable while the slot is occupied). Surface: host `cc3501e_ota_promote()`,
firmware `cc3501e_hw_ota_promote()` (ti) / NOTIMPL (stub), example
`-DCC3501E_OTA_PROMOTE=ON`. Native transport test covers the opcode
(NOT_READY on stub, INVALID on bad payload).

**Bench step to close crit-1** (needs a CC35 firmware rebuild + reflash, since the
promote opcode must be present in the CC35's *running* firmware — the current
primary v1 predates it): (1) `build_ti.sh` the CC35 firmware with this change +
reflash it; (2) run the E8 app with `-DCC3501E_OTA_PROMOTE=ON` — it calls
`cc3501e_ota_promote()`, the firmware arms `psa_fwu_request_reboot()`, the bridge
drops as the CC35 self-reboots, and BL2/MCUboot swaps the pending image to
primary; (3) confirm `GET_VERSION` reports the new image, then one true cold POR
retains it. (The exact PSA "already pending" error mapped to host `-1` lives in
the license/vendor FWU path, not this repo's `src/` — TBD.)

## 6. GPIO proxy

GPIO proxy and camera-enable opcodes are shipped. The firmware guards reserved
bridge/UART/unbonded pads, the host exposes `cc3501e_gpio_*`, the portable
backend delegates non-CC3501E pins to the platform backend, and the warm-boot
GPIO example has bench coverage.

## 7. OTA

OTA over the bridge is RAM-staged by design: each WRITE copies into RAM, and
FINISH performs one flash burst plus `psa_fwu_install`. This avoids repeatedly
tearing down the bridge DMA during a long image stream. Stage/install *and*
the final swap are silicon-validated: **#493 criterion 1 closed 2026-07-10**
with the full cold-swap cycle proven on E8 (§5 records the run). Candidates
must be forward-versioned — the CC35's FWU service refuses a version
rollback, which earlier bench runs misread as a dead secure element.

## 8. Open items / next

1. **Async-event producers** - the transport shipped (#130, alp-sdk#1721); the
   gap is producers. `EVT_GPIO_INTERRUPT` (`gpio_irq_cb()` only clears the edge)
   and the BLE events never call `event_ring_push()`. This is a push at the
   source, not new hardware. Separately, confirm whether the shipped
   `cc3501e-v0.4.0.bin` was built with `-AttnPulse` -- the flag is default OFF.
2. **Full runtime GATT/event parity** - finish the v1.0 portable BLE event
   surface; the attention transport it needs already exists.
3. **Credentialed socket soak** - run against a lab network during production
   validation.
4. **OTA cold swap-boot** - repeat final swap validation on a correctly
   activated cold-bootable CC3501E unit.
5. **`flash.py` real flashing** - now Alp-internal tooling (moved to
   `alp-sdk-internal`); replace manual SWD/J-Link when TI's
   `cc3501e-flasher` CLI becomes public.
