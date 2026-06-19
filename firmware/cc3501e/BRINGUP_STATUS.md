<!--
SPDX-License-Identifier: Apache-2.0
Copyright 2026 Alp Lab AB
-->

# CC3501E bridge bring-up status

Status of the Alif Ensemble E8 (M55-HE) ↔ CC3501E (CC35X1E) 3-wire SPI bridge
on the E1M-AEN801 bench. Branch `bench/cc3501e-v01-bringup`. Updated 2026-06-18.

This is the single consolidated record for the **link / Wi-Fi / BLE** pillars so
their state is not lost between sessions. It is the on-silicon counterpart to the
design recipe in `firmware/cc3501e/ti/WIFI_BLE_INTEGRATION.md`.

## TL;DR — pillar status

| Pillar | State | Evidence / blocker |
|---|---|---|
| **Inter-chip link** (PING / GET_VERSION / GET_MAC / RESET) | ✅ **WORKS cold + warm** | `ping_ok` climbing, `reqhdr_rx=0xA5A5A5A5`, `reply_hdr=0x00010000`, `fail_step=0` |
| **Wi-Fi GET_MAC** | ✅ **WORKS cold** through the bridge | `mac_ok=1`, MAC `44:3E:8A:10:AF:ED` |
| **Wi-Fi scan / RSSI** | ✅ runs through the bridge (worker-routed) | scan completes; **finds 0 APs → RF/antenna follow-up**, not a bridge bug |
| **Wi-Fi connect-STA / soft-AP / sockets** | 🚧 **untested (RAM now sufficient)** | bodies built under `#ifdef CC3501E_WIFI`; the ~264 KB SDK STA+sockets floor now fits the **512 KB** vendor DRAM (linker fix, §3) — bump the heap + bench-test |
| **BLE** (enable / advertise) | ⛔ **HW-gated: external antenna (J1 u.FL)** | RAM solved (512K DRAM); SW sound (suspend fix, sequence matches TI). Netlist-confirmed: U4=BDE-BW35N routes RF to an external u.FL connector **J1** — open/untuned J1 → BLE-enable RF-cal stall (the 120s hang) + the WiFi 0-AP scan. Connect+tune the J1 antenna on-site, then re-test |
| **CAM enables** | ✅ fixed mapping (netlist) | `which` 0→GPIO_1 (LDO0), 1→GPIO_0 (LDO1) — firmware was reversed; corrected from U4 pins 54/55 |
| **GPIO proxy** + camera enables | 🚧 **in progress** | firmware HAL + host API done; portable backend / tests / example pending; pad map parked |
| **Cold-boot** | ✅ host-workable | Puya 64 Mbit flash bug; host hard-reset after each power-cycle |
| **OTA over SPI** | designed, parked (secondary) | opcodes 0x40–0x4F; PSA-FWU cycle built |

---

## 1. The inter-chip link (3-wire CS-less SPI)

Fixed-clock-count lockstep framing, no chip-select, no host-IRQ. 4-byte header,
≤512-byte payload, `0xA5` (`ALP_CC3501E_SYNC_IDLE`) sync marker at header
boundaries, `cmd ≥ 0x80` desync guard.

**Validated:** PING / GET_VERSION / GET_MAC / RESET all complete cold and warm
(bench-confirmed via the SWD witness: aligned reply headers, no framing faults).

**Root cause of the long cold-framing saga = MISO sample timing, NOT framing
logic.** The Alif DW-SSI master had `rx_delay=0` (sample MISO at the SCLK edge);
at 8 MHz over the on-SoM traces + the crossed-data bench bodge, the CC35's MISO
bit had not propagated back by the sample edge → the Alif read `0xFFFFFFFF`
(floating) and the CS-less link never framed. **Fix = drop the bridge SPI clock
8 MHz → 1 MHz** (`CC3501E_SPI_FREQ_HZ` in the bring-up example): bit period 1 µs
≫ the MISO round-trip → clean sampling → cold GET_MAC works end-to-end. To raise
the clock later: set the `alif,dwc-ssi` `rx-delay` DT prop (in ssi_clk/400 MHz =
2.5 ns units) on the cc3501e_spi node to cover the round-trip.

**Known limitation (CS-less, no host-IRQ):** the link can briefly desync on a
first-contact gap; a desync self-heal (re-arm on a `0xA5` burst) is in place. The
**next board rev's CS line + host-IRQ** is the clean fix (CS delimits frames; the
CC35 signals busy/ready so the host defers). Until then the link works between
radio ops; see §4.

**Net-positive link hardening to KEEP:** `0xA5` idle marker (ship value), GET_MAC
gated behind `ping_ok ≥ 20` (radio read waits for a stable link), desync
self-heal, confirm-first ordering (psa_fwu_accept before radio).

## 2. Wi-Fi

- **GET_MAC** = `Wlan_Get(WLAN_GET_MACADDRESS)`, needs only `Wlan_Start` (the HIF
  bring-up; internally calls `InitHostDriver`). **Worker-routed** off the SPI ISR
  (poll-by-repeat from the host: retry on BUSY + IO). **Validated cold** on
  silicon: `mac_ok=1`, MAC `44:3E:8A:10:AF:ED`.
- **Scan / RSSI** = worker-routed too; the host packs the AP list in the wire
  format `bssid[6]|rssi|channel|security|ssid_len|ssid`. Runs through the bridge
  without breaking the link. **scan_count=0** on the bench → RF/antenna/scan-config
  follow-up (NOT the bridge). Gotcha proven: `Wlan_RoleUp(STA)` before
  `Wlan_Scan` breaks the link (worker blocks ~10 s + bridge disrupted) — scan
  needs only `Wlan_Start`, never RoleUp. RoleUp is for CONNECT only (bounded).
- **connect-STA / soft-AP / sockets** = bodies implemented under `#ifdef
  CC3501E_WIFI`; **untested**. The ~264 KB SDK STA-with-sockets floor was thought
  to exceed DRAM, but that was the 193 KB linker-cap bug (see §3) — it fits the
  real **512 KB** vendor DRAM. Bump the heap and bench-test; no PSRAM needed.

## 3. BLE — RAM ceiling was a LINKER BUG (fixed); enable-hang root-caused + fixed

**The "needs PSRAM" verdict was wrong — it was a wrong linker cap, not silicon.** The
build used the STOCK board cmd `cc35xx_freertos.cmd`, which caps app `DRAM` at 0x30000
(192 KB, "static only"). TI's **connectivity vendor apps** (`network_terminal` AND
`ble_wifi_provisioning`, same 0x14000000 vendor FLASH base, same DRAM bank) use
`DRAM_NON_SECURE = 0x28000DB0..0x2807FFFF = 512 KB`. So the chip has 512 KB app DRAM;
the 192 KB cap drove the entire dead-end.

**Fix (`build_ti.ps1`):** switched the linker base to the `network_terminal` demo's
`linker.cmd` (512 KB DRAM; stack already in TCM = the cold-boot fix; all else in DRAM).
Dropped the earlier TCM `.ble_bss` split + the `.stack` patch (both obsolete). Result:
`-Ble` links with the 136 KB heap + **all `.bss` (WiFi + NimBLE) in DRAM**, ~275 KB
free. **On silicon, WiFi GET_MAC works in the combined WiFi+BLE image** (`mac_ok=1`,
MAC 44:3E:8A:10:AF:ED) and the earlier `Wlan_Start` hang is **gone** (it was caused by
the TCM `.ble_bss` placement, now removed). **No PSRAM needed** for coexistence — and
full WiFi STA+sockets (264 KB SDK floor) now also fits 512 KB (untested; just bump the
heap).

**BLE-enable hang — ROOT-CAUSED + FIXED at the link level (2026-06-18).** OpenOCD
0.12.0 + gdb-multiarch are on the box, but the **M33 core is secure-debug-gated**:
OpenOCD attaches to the XDS110 + CC35 DAP, yet the only APs are 3 TI-custom ones (no
standard MEM-AP; CPUID unreadable), so vanilla OpenOCD can't halt/inspect the core
without TI's proprietary CFGAP unlock. A research workflow's **source** analysis gave
the answer instead: `BleIf_EnableBLE → cmd_Send → osi_SyncObjWait` is bounded at
`CMD_SEND_TIMEOUT_MS=120 s` (not infinite) and the NWP never command-completes
`BLE_ENABLE` because the **bridge SPI slave's live DMA (ch12/13) contends with the HIF
handshake during the op** — and `ctrlCmdFw_LockHostDriver/Unlock` are **NO-OPs**, so
nothing serializes them. (WiFi `Wlan_Start` survived because the worker re-arms the
bridge *after*; BLE enable hangs *during*.) **Fix:** `bridge_transport_spi_hw_suspend()`
(`SPI_transferCancel`+`SPI_close`, releasing DMA ch12/13) called in
`cc3501e_hw_ble_enable` *before* `cc3501e_nimble_host_start`, paired with the existing
reinit *after* — so the HIF is the sole DMA client during enable. **On silicon (v0.0.27)
the hang is GONE: the link recovers cleanly** (`reqhdr=0xA5A5A5A5`, `ping_fail=0`,
`mac_ok=1`, `scan_status=0` — was -4) where it previously stuck down for 120 s.

**Pending (final confirmation):** `ble_status` is still `-4` (`ble_enabled=0`) because
the host `cc3501e_ble_enable` poll gave up at its 10 s budget before the now-working but
slower (~10–15 s NWP BLE cold-init) enable published its result. Host-side fix applied
(not yet flashed): bumped the floor 10 s→30 s (`CC3501E_BLE_ENABLE_WINDOW_MS`). Rebuild
the Alif app (WSL `ninja -C ~/aenbuild_slot0` → `app-gen-toc` → J-Link flash) + cold-cycle
→ expect `ble_enabled=1`. (CC35 core secure-debug-gate: future CC35-side debugging needs
the TI CFGAP unlock — OpenOCD alone can't.)

**`Wlan_Start`'s heap floor was pinned on silicon by bisection (2026-06-18):**

| FreeRTOS heap | GET_MAC (`Wlan_Start`) result |
|---|---|
| 88 KB (`0x16000`) | ✗ starves — `ping_ok` stuck at the gate, `reqhdr_rx→0`, mac timeout |
| 106 KB (`0x1A000`) | ✗ starves (same signature) |
| 120 KB (`0x1E000`) | ✗ starves (same signature) |
| **136 KB (`0x22000`)** | ✅ **works** — `ping_ok` climbs, `mac_ok=1` |

⇒ `Wlan_Start` needs **~128–136 KB heap**. This is the HIF bring-up that **both**
Wi-Fi GET_MAC **and** BLE must run. The heap floor is independent of total DRAM, so
it still holds under the 512 KB linker; the ship config sets `0x22000` (136 KB).

**Coexistence budget (against the corrected 512 KB DRAM):** ~136 KB heap + ~48 KB
fixed Wi-Fi-stack `.bss` + ~8 KB stack `.bss` + NimBLE host/controller `.bss` all
land in the 512 KB `DRAM_NON_SECURE` bank, with **~275 KB free** in the linked
`-Ble` image (map-confirmed). **So Wi-Fi + BLE DO coexist in RAM** — the earlier
"cannot coexist / needs PSRAM" verdict was a side-effect of the 192 KB linker cap
and is **void**. BLE's remaining blockers are runtime, not RAM (see the enable
section above + the antenna): confirm `ble_enabled=1` after the 30 s enable-budget
reflash and the on-site J1/antenna tune. The BLE firmware
(`cc3501e_nimble_host.{c,h}`, `cc3501e_hw_ble_enable`, the `-Ble` link in
`build_ti.ps1`) is **code-complete and links**.

## 4. Bridge ↔ radio coexistence

On this no-host-IRQ rev the CC35 cannot service the inter-chip SPI slave WHILE it
runs a radio op (`Wlan_Start` at boot, or a worker `Wlan_*` body): the link is
DOWN during the op. The async submit/poll model handles it: host submits → (bridge
down during the radio op) → the worker re-arms the SPI at a clean boundary after
the op → host poll (bridge up) reads the cached result. The host retries on
`ALP_ERR_IO` (not just BUSY) across a budget that covers the op (`Wlan_Start` ~s).
This is why GET_MAC/BLE_ENABLE are worker-routed and the host floors their budget
to a ~10 s "radio-down window".

## 5. Cold-boot

Root cause = a TI-SDK bug supporting Puya 32/64 Mbit flash (ours is PY25Q64LB =
Puya 64 Mbit); the first boot after power-on mis-reads the flash. **Workaround
(host-side, validated):** after every power-cycle the host gives the CC35 a hard
reset (WIFI_EN high, let the first boot settle, then drive nRESET low/high) — the
second boot runs the vendor image. Implemented in `cc3501e_hard_reset` /
`cc3501e_reset`. Provision FRESH units via the atomic CLI factory flow (not the
GUI activation wizard) so the cold-launch binding is established in one session.

## 6. GPIO proxy (in progress)

Opcodes already in the protocol: `GPIO_CONFIGURE 0x50 / WRITE 0x51 / READ 0x52 /
SET_INTERRUPT 0x53 / EVT 0x54`, `CAM_ENABLE 0x60 / CAM_DISABLE 0x61`. Synchronous
in firmware (GPIO is µs, no worker).

- **Done + compiles** (default build, exit 0): firmware HAL (`cc3501e_hw_gpio_*` /
  `cc3501e_hw_cam_enable` in `cc3501e_hw_ti.c`) — the CC35xx TI Drivers GPIO is
  **pin-indexed** (`gpioPinConfigs[]` indexed by pad number), so the wire's **raw
  `cc3501e_gpio` index drives the pad 1:1 — no firmware pad map needed**. A
  reserved-pin guard refuses the bridge SPI (16/27/28/29), UART2 (5/6), and
  unbonded pads (7/8/9) so a stray host command can't tear down the link. CAM
  `which` 0/1 → GPIO0/GPIO1. Host API (`cc3501e_gpio_configure/write/read/
  set_interrupt`, `cc3501e_cam_enable`) added in `chips/cc3501e/cc3501e.c` +
  `include/alp/chips/cc3501e.h`.
  - **GPIOWFF3 controller limits (discovered at compile):** no true open-drain
    output (`GPIO_CFG_OUTPUT_OPEN_DRAIN_INTERNAL` = NOT_SUPPORTED) → `DIR_OPEN_DRAIN`
    is emulated as a push-pull output idling HIGH (electrically equivalent on a
    single-driver W_DISABLE line, unsafe on a shared line); no both-edges interrupt
    (`GPIO_CFG_IN_INT_BOTH_EDGES` = NOT_SUPPORTED) → `EDGE_BOTH` returns INVAL (arm a
    single edge). RISING/FALLING + push-pull/input + pulls are fine.
- **Done (scaffold):** portable `<alp/gpio.h>` bridge backend
  `src/backends/gpio/cc3501e_proxy.c` (`CONFIG_ALP_SDK_GPIO_CC3501E_PROXY`, default
  n, AEN-only). One backend per gpio class, so it's a **delegating proxy**:
  registers above the `*` platform backend, routes pin_ids in the board's
  `cc3501e_gpio_routes[]` table through the bridge (`cc3501e_gpio_*`), delegates
  everything else to `zephyr_drv` (via the new `alp_z_gpio_ops()` accessor) — so
  the Alif's own pins are untouched. Ships a **weak EMPTY route table** + an
  `alp_gpio_cc3501e_attach(ctx)` API; with no routes / no bridge it's a pure no-op
  (every pin delegates). Bridge-pin IRQ returns NOSUPPORT (no slave→master line).
- **Done:** firmware protocol ztests (configure→write→read round-trip, bad-length
  → INVALID, CAM enable/disable) in `tests/zephyr/cc3501e_bridge_transport` against
  the in-memory stub HAL — validate the wire structs + dispatch both layers share.
- **Pending verification:** native_sim twister (compile + run) of the host API +
  proxy + new ztests — deferred to the Land step (the host/proxy paths are
  native_sim-bound; firmware HAL is already TI-build-verified). Example
  self-loopback exercise.
- **Parked (needs the user's pad map):** the logical IO11/IO13/IO15..IO21 → raw
  CC35-GPIO-index route table (board metadata, fills `cc3501e_gpio_routes[]`) and
  one **safe-to-toggle GPIO index** for the on-silicon write→read loopback. The
  firmware/host/proxy code is validatable as soon as a safe pin + the map exist.

## 7. Build matrix (`firmware/cc3501e/ti/build_ti.ps1`)

| Build | Switch | Size | Notes |
|---|---|---|---|
| default (radio-free bridge) | *(none)* | ~25 KB | PING/IO/GPIO/OTA; native CI baseline |
| Wi-Fi host | `-WifiHostDriver` | ~987 KB text | GET_MAC/scan/RSSI; heap `0x22000` (136 KB) |
| Wi-Fi + BLE | `-Ble` (implies `-WifiHostDriver`) | ~1.05 MB | links + fits 512 KB DRAM (~275 KB free); runtime-gated only by the enable-budget reflash + antenna |

## 8. Bench validate recipe (roles, not paths)

1. Build: `build_ti.ps1 -WifiHostDriver` → `cc3501e-bridge.out`.
2. FIB: toolbox `flash-images-builder build vendor_image --version <v> --public_key
   <validation pub> --vendor_out_file <out> --conf_bin_file <conf>` → produces
   `vendor_image.unsign.bin`; then `flash-images-builder sign vendor_image
   --unsign_image vendor_image.unsign.bin --activation_type vendor_key
   --signing_module sign.py` → produces `vendor_image.sign.bin`. **Gotcha:** `sign`
   names its output after the input base name, so **copy it to
   `primary_vendor_image.sign.bin`** (the name `tool_settings.json` references).
3. Program: toolbox `programmer -i XDS110 -param1 <SN> programming --tool_settings
   <ts>` (retry on the intermittent `-1141` SECAP reject).
4. Cold-cycle: PSU CH2 power-cycle (true cold POR) → the Alif app's
   `cc3501e_reset` runs the Puya hard-reset workaround → soak loop.
5. Read the SWD witness (`g_cc3501e_witness` @ a fixed RAM address) over the Alif
   J-Link: `magic / reset_status / ping_ok / ping_fail / last_status / version /
   mac_ok / mac_lo / mac_hi / scan_status / scan_count / ble_status / ble_enabled`.
   (A reusable `deploy_validate.ps1` chains FIB → program → cold-cycle →
   witness-read on the bench.) Sign of life: `ping_ok` climbing, `ping_fail` flat.
   WiFi: `mac_ok=1`. (The raw-framing debug fields — `fail_step / reqhdr_rx /
   reply_hdr` — were bench-only and are reverted; re-add locally if a future
   framing regression needs them.)

## 9. Open items / next

1. **GPIO proxy** — finish the portable backend + tests + example; bench-validate
   once a safe GPIO index + pad map are provided.
2. **BLE confirm** — reflash with the 30 s enable budget, tune the on-site
   antenna (J1 u.FL / the A1 NN02-201 pi-match), expect `ble_enabled=1`. RAM is
   no longer a blocker (512 KB linker fix).
3. **Wi-Fi 0-AP** — RF/antenna + scan band/dwell/channel-set check (bridge path is
   proven correct). Same antenna root cause as BLE.
4. **Wi-Fi STA+sockets** — bench-test the `#ifdef CC3501E_WIFI` connect/soft-AP/
   socket bodies now that they fit 512 KB (bump the heap). No PSRAM needed.
5. **Next board rev** — CS line + host-IRQ removes the CS-less framing fragility
   and enables async EVT (BLE/Wi-Fi/GPIO-IRQ push to the host).
6. **Land** — revert the bench-only diagnostics (witness debug fields, host
   `cc3501e_dbg_*`), keep the `0xA5` marker + OTA + radio + GPIO code, run
   native_sim CI, then PR to dev (on the maintainer's go). ← DONE in this PR.
