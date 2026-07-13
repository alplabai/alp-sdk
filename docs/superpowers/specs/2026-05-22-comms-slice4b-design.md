# Comms Registry Migration (Slice 4b) Design

**Date:** 2026-05-22
**Status:** Draft — pending implementation
**Owner:** alpCaner
**Predecessor:** Slice 4a (`feat/backend-registry-simple-peripherals`) — proved the multi-peripheral one-PR pattern
**Foundation:** Slice 1 ADC pilot (PR #18)
**Branch:** `feat/backend-registry-comms` (off `feat/backend-registry-simple-peripherals`)

## Motivation

Three communication peripherals — **USB, BLE, IoT (= WiFi + MQTT)** — currently live in legacy `src/zephyr/{usb,ble,iot}_zephyr.c` files (1269 lines total). All have real Zephyr-side bodies wrapping the respective Zephyr subsystems (USBD/BT/Net+MQTT). This slice migrates them onto the backend registry pattern with **no behavioural change** — same Zephyr subsystem calls, same error semantics, structurally moved into `src/backends/<class>/zephyr_drv.c`.

`<alp/iot.h>` carries two independent handle types (`alp_wifi_t` and `alp_mqtt_t`) — they get **two separate class registries** (`wifi` and `mqtt`), mirroring the Slice 4a precedent where `alp_counter_t` and `alp_qenc_t` split despite sharing `<alp/counter.h>`.

Net: **four class dispatchers** in this slice.

## Non-goals

- Vendor extensions. None of USB / BLE / WiFi / MQTT pass the §3 audit — every per-SoC feature is reachable through Zephyr's portable USB device class / BT host / Net / MQTT subsystems.
- CC3501E proxy as a vendor extension. Superseded by issue #478: the CC3501E
  is the AEN Wi-Fi/BLE coprocessor, not the V2N module. AEN now routes portable
  Wi-Fi/BLE through exact `ti-cc3501e` registry backends, still **not** through
  a vendor-extension header.
- Yocto backends. Yocto-side BlueZ / IPv6 / WiFi userland is out of scope. Stays on stub or the existing `src/yocto/*` (BLE/USB don't have Yocto stubs today).
- New `<alp/wifi.h>` / `<alp/mqtt.h>` headers. Both surfaces stay in `<alp/iot.h>`. Two dispatchers, one header.

---

## Section 1 — File layout (per class)

For each of `usb`, `ble`, `wifi`, `mqtt`:

```
src/
├── <class>_dispatch.c                    (new)
└── backends/
    └── <class>/
        ├── <class>_ops.h                 (new: internal ops vtable + struct alp_<class>)
        ├── zephyr_drv.c                  (new: lift body from src/zephyr/<x>_zephyr.c)
        └── sw_fallback.c                 (new: native_sim no-op / loopback)
```

Plus per-class ztest harness under `tests/unit/<class>_registry/`.

Files **deleted**:
- `src/zephyr/usb_zephyr.c` (USB subagent)
- `src/zephyr/ble_zephyr.c` (BLE subagent)
- `src/zephyr/iot_zephyr.c` (MQTT subagent — last to migrate, deletes the file outright; WiFi subagent leaves it intact and just adds its files)

`src/zephyr/handles.h` and `handles.c`: USB has no pool plumbing there (uses internal Zephyr USB device state). BLE / WiFi / MQTT similarly — none of them have `DEFINE_POOL(<class>, ...)` lines. **handles.c is not touched in this slice.** `handles.h` carries `struct alp_usb_dev` / `struct alp_usb_host` / `struct alp_ble` / `struct alp_wifi` / `struct alp_mqtt` definitions if any exist — check on a per-class basis and move the relevant struct into the new ops.h if so. (Most likely they live inside the legacy `.c` files, not in handles.h.)

---

## Section 2 — Backend matrix

| Class | `zephyr_drv` (priority 100, `*`) | `sw_fallback` (priority 0, `*`) | Tracking |
|-------|---------------------------------:|---------------------------------:|---------|
| usb   | yes (USBD subsystem)             | yes (no-op stub)                | n/a — real backend ships in this slice |
| ble   | yes (Zephyr BT host)             | yes (no-op stub)                | n/a |
| wifi  | yes (Zephyr Net WiFi mgmt)       | yes (no-op stub)                | n/a |
| mqtt  | yes (Zephyr MQTT client API)     | yes (no-op stub)                | n/a |

All four follow the Slice 4a pattern: a portable Zephyr backend at `silicon_ref = "*"` priority 100 that wraps the respective Zephyr subsystem, and a wildcard sw_fallback at priority 0 for native_sim builds.

**Historical note:** this draft originally planned no vendor-specific Wi-Fi/BLE
backend. Issue #478 supersedes that for AEN: the CC3501E is an exact-silicon
registry backend for `alif:ensemble:e3` through `alif:ensemble:e8`. Other
targets still use the wildcard Zephyr BT / Wi-Fi backends.

---

## Section 3 — Vendor-extension audit

| Feature considered                  | Class | Portable equivalent                                          | Decision |
|-------------------------------------|-------|--------------------------------------------------------------|----------|
| USB CDC-ACM vs HID class selection  | usb   | Zephyr `usbd_*` device-class registration; `alp_usb_device_config_t` carries class info | PORTABLE |
| BLE GATT MTU negotiation            | ble   | Zephyr `bt_gatt_exchange_mtu` driver call                    | PORTABLE |
| WiFi WPA3-PSK / WPA3-Enterprise     | wifi  | Zephyr `WIFI_SECURITY_TYPE_*` enum                           | PORTABLE |
| MQTT TLS via the Zephyr-side stack  | mqtt  | Zephyr `mqtt_client_tls_config_t` (already supported)        | PORTABLE |
| CC3501E firmware update path        | wifi  | Chip-driver internal; `chips/cc3501e.h` already covers it    | INTERNAL (not user-facing) |

**Conclusion: zero vendor extensions.** `include/alp/ext/` is untouched.

---

## Section 4 — Per-class surface delta

Each public header gains one new declaration: `alp_<class>_capabilities()`. Surface inventory:

- **USB**: two surfaces — device and host. **One dispatcher** (since they're both under `<alp/usb.h>`)? Actually each is a different handle type with its own state. Per the Slice 4a precedent, they could split into two class registries. **DECISION: one dispatcher with separate ops fields for device vs host**, because they share the underlying Zephyr USB controller. The dispatcher exposes both `alp_usb_device_open` and `alp_usb_host_open` and routes through the SAME `usb` registry; the ops vtable carries function pointers for both surfaces. Simpler than two registries.
- **BLE**: single handle type `alp_ble_t`, plus a `alp_ble_conn_t` for active connections (not registry-managed — owned by the BLE backend internally). One dispatcher.
- **WiFi**: single handle type `alp_wifi_t`. One dispatcher.
- **MQTT**: single handle type `alp_mqtt_t`. One dispatcher.

ABI deltas: 4 new functions (`alp_usb_capabilities`, `alp_ble_capabilities`, `alp_wifi_capabilities`, `alp_mqtt_capabilities`). USB's `capabilities` takes a `const alp_usb_dev_t *` (device-side) — host-side capability getter can be added in a follow-up if needed (low priority since hosts are less common on E1M form factors).

---

## Section 5 — sw_fallback behaviour per class

- **USB**: `open` succeeds, all I/O ops return `ALP_ERR_NOT_IMPLEMENTED`. The native_sim build can't do real USB; the stub lets examples that include `<alp/usb.h>` compile.
- **BLE**: `open` succeeds; `advertise_start`/`scan_start` succeed but never observe peers; GATT ops return `ALP_ERR_NOT_IMPLEMENTED`.
- **WiFi**: `open` succeeds; `connect` returns `ALP_ERR_NOT_IMPLEMENTED` (no real radio).
- **MQTT**: `open` succeeds; `connect` returns `ALP_ERR_NOT_IMPLEMENTED`.

All four sw_fallbacks carry the mandatory `@par Cost:` and `@par Performance:` tags (`scripts/check_sw_fallback_tags.py` enforces).

---

## Section 6 — Subagent dispatch shape

Four sequential subagents, **strictly ordered**:

1. **USB** (Task 1) — independent migration, no inter-class dependency.
2. **BLE** (Task 2) — independent.
3. **WiFi** (Task 3) — first half of `iot_zephyr.c`. Leaves the file intact (containing only MQTT body).
4. **MQTT** (Task 4) — second half of `iot_zephyr.c`. DELETES the file at the end.

Plus Task 5: ABI regen + PR.

Each subagent ships 7–8 commits matching the Slice 4a RTC cadence (capability decl, ops.h, dispatcher, zephyr_drv, sw_fallback, drop legacy, build wiring, test harness). WiFi has no "drop legacy" commit (file kept for MQTT); MQTT drops the legacy file in its cleanup commit.

---

## Cross-cutting concerns

### ABI impact

- 4 new public functions (capability getters).
- No signature changes.
- Headers stay `[ABI-STABLE]` (USB / BLE / IoT are stable) — capability getter additions are non-breaking.

### Migration risk

- USB: low. Single Zephyr USBD subsystem path, straightforward lift.
- BLE: medium. The legacy `ble_zephyr.c` has callback-heavy GATT registration paths; the registry pattern preserves the callback dispatch verbatim.
- WiFi: medium. Net mgmt event callbacks need careful preservation.
- MQTT: medium. Has the loop-callback pattern; preserve verbatim.

### Test coverage

Four new ztest harnesses, 6–7 cases each — selector tests + null-arg tests + handle-count + (where applicable) inval-on-bad-config tests.

---

## References

- Slice 4a plan (canonical template): `docs/superpowers/plans/2026-05-22-simple-peripherals-slice4a.md`
- Slice 1 ADC (foundational): `docs/superpowers/specs/2026-05-22-adc-registry-pilot-design.md`
- Foundation PR: #17 (Slice 0 registry)
- Stacks on PR #19 (Slice 4a)
- Memory: `feedback_portable_peripheral_api`, `project_v2n_no_dedicated_math_accelerator` (re: CC3501E routing inside Zephyr driver, not vendor-ext)
