# CC3501E bridge — Wi-Fi + BLE host integration recipe (SDK 10.10.01.08, ticlang)

Grounded from the TI SDK 2026-06-18 (deep agent maps). SDK at
`<ti-sdk>/simplelink_wifi_sdk_10_10_01_08`.
Both stacks run ON the CC35 app core; the bridge SPI-slave (to the Alif) feeds a
FreeRTOS **worker** (P0-4) that calls these — never the SPI ISR.

## Load-bearing facts
- Wi-Fi API = `Wlan_*` / `InitHostDriver` (NOT the classic `sl_*`). Header
  `source/ti/drivers/net/wifi/wifi_host_driver/inc_adapt/wlan_if.h`; impl in `wifi_stack.a`.
- BLE = Apache **NimBLE**; controller in CC35 FW, **shared HIF with Wi-Fi** ⇒
  **Wi-Fi must start first**: `Wlan_Start` → `Wlan_RoleUp(STA)` → BLE `nimble_host_start`.
- **No IP-acquired event** — read IP from lwIP `network_stack_get_if_ip` (network_lwip.c).
- BLE controller is gated by the device conf_bin `EnableBle` — regen `cc35xx-conf.bin`
  with BLE ON before the BLE silicon test, else `BleIf_EnableBLE` times out.

## Wi-Fi link set (ticlang `.a`, under `<SDK>/source/`)
- `ti/net/wifi_stack/lib/ticlang/wifi_stack.a`  (Wlan_*/InitHostDriver/CME/scan/connect — the impl)
- `ti/drivers/net/wifi/wifi_host_driver/lib/ticlang/wifi_host_driver.a`
- `ti/drivers/net/wifi/wifi_platform/cc35xx/lib/ticlang/wifi_platform_cc35xx.a`  (HIF/bus/IRQ)
- `third_party/lwip/lib/ticlang/lwip.a`  (IP/DHCP/sockets)
- `third_party/hostap/lib/ticlang/hostap.a`  (WPA2/WPA3/EAP supplicant)
- `ti/utils/FWU/lib/ticlang/FWU.a`  (already linked)
- circular deps (wifi↔lwip↔hostap): ticlang link uses `-Wl,--reread_libs` (gcc: `--start-group/--end-group`).
- driverlib/secure/mbedtls/psa_crypto/hsmddk pulled via the SysConfig `ti_utils_build_linker.cmd.genlibs`.

## BLE link set
- `third_party/nimble/lib/ticlang/nimble.a` ; `ti/net/ble_interface/lib/ticlang/ble_interface.a`
- BLE controller-enable symbols (`ctrlCmdFw_EnableBLECmd` etc.) are in `wifi_stack.a`.

## Sources to COMPILE from SDK (NOT in any lib — `wifi_stack.a` has `osi_*` as undefined U)
From `examples/rtos/LP_EM_CC35X1/demos/network_terminal/`:
- `adaptation/osi_dpl.c`  ← **MANDATORY full OSI→DPL/FreeRTOS port (~30 funcs:
  SyncObj/Thread/MsgQ/Lock/Timer/malloc/Sleep/GetTimeMS). The v0.1 `osi_uSleep` stub is far short.**
- `adaptation/osi_filesystem.c` (OSI FS/NV — connectivity-FW container access)
- `adaptation/uart_term.c` (provides `Report()` — libs call it; stub OK but must resolve)
- `adaptation/syslog.c`
- `network_lwip.c`  ← app source: `network_stack_init`, `network_stack_add_if_sta/ap`,
  `network_set_up`, `network_get_sta_if`, `network_stack_get_if_ip` (WIFI_GET_IP)
BLE port (from `source/third_party/nimble/ti_config/nimble-port/`, per its CMakeLists):
- `transport/cc3xxxhif/src/cc3xxxhif_ble_hci.c` ← **LL transport glue (`ble_transport_ll_init`,
  `ble_transport_to_ll_*`); gc-strip risk — keep referenced.**
- `porting/npl/osi/src/npl_os_osi.c` ; `porting/npl/osi/src/nimble_osi_filesystem.c` ; `porting/nimble/src/base64.c`
- `ble_store_ram.c` is in `nimble.a` (use `ble_store_ram_init()` unless persistent bonding).
App-side (copy/adapt): `nimble_host.c` (the `nimble_host_*` wrappers + `gap_event_cb` + GATT svc table).

## Init order
Pre-scheduler: `Board_init()`. After scheduler, on the worker/init task:
`initAppVariables()` (create OSI sync objs) → `network_stack_init()` (lwIP, blocks) →
`Wlan_Start(WlanEventHandlerCB_t cb)` → `Wlan_RoleUp(WLAN_ROLE_STA, &staParams, WLAN_WAIT_FOREVER)`.
BLE (after Wi-Fi STA up): `nimble_host_start()` (does `BleIf_EnableBLE` + NimBLE + blocks ~2s to sync).

## Worker ↔ event rendezvous (both stacks)
Command thread: `osi_SyncObjClear(&s)` → issue `Wlan_*`/`ble_gap_*` → `osi_SyncObjWait(&s, timeout)`.
Event cb (runs on the driver/host-task thread, NOT ISR, NOT worker): copy result → `osi_SyncObjSignal(&s)`.
ISR-safe only: `osi_SyncObjSignalFromISR`, `osi_MsgQWrite(...,OSI_NO_WAIT)`. Create sync objs once at startup.

## Per-HAL bodies (Wi-Fi) — wlan_if.h signatures
- GET_MAC (blocking, no event): `WlanMacAddress_t p={.roleType=WLAN_ROLE_STA}; Wlan_Get(WLAN_GET_MACADDRESS,&p);` → `p.pMacAddress[6]`.
- WIFI_SCAN: `scanCommon_t sc={.Band=BAND_SEL_BOTH}; Wlan_Scan(WLAN_ROLE_STA,&sc,N);` → wait `WLAN_EVENT_SCAN_RESULT` (ONE event, whole list in `Data.ScanResult.NetworkListResult[]`, `WlanNetworkEntry_t{Ssid[32],Bssid[6],SsidLen,Rssi,SecurityInfo,Channel}`). Pack compact (MAX_PAYLOAD=512 ⇒ ~17 entries/frame; page if needed).
- WIFI_CONNECT (STA): `Wlan_Connect((signed char*)ssid,len,NULL,secType,pass,passlen,0);` wait `WLAN_EVENT_CONNECT` (`Data.Connect.Status<0`=fail). Sec enum: OPEN=0,WPA_WPA2=2,WPA2_PLUS=11,WPA3=12,WPA2_WPA3=16. IP via DHCP after — call `network_set_up()` then poll `network_stack_get_if_ip`.
  - **MUST be worker-routed (P0-4), NOT run in the SPI ISR** — the association blocks ~15s on `WLAN_EVENT_CONNECT`; a blocking wait in `protocol_dispatch`'s ISR context either hung the bridge (`WAIT_FOREVER` → host -4) or could not pend (bounded → fast -2). Fixed 2026-06-23: `protocol.c wifi_join` routes via `worker_submit_payload` (the connect payload rides a worker-owned buffer); the drain runs the blocking `Wlan_Connect` off-ISR. The L2 association then completes on silicon.
  - **Bench-validated 2026-06-24 (E1M-AEN801 EVK, fw v0.0.207.0): the bridge SURVIVES the ~15s association** — `ver`/GET_VERSION after a connect still returns, no desync, no power-cycle needed. The earlier "the ~15s association DESYNCS the CS-less r1 SPI bridge permanently (no recovery without power-cycle)" limitation is RESOLVED by running the bridge over the proper hardware peripheral-driven SS0 chip-select (P14_7 = SPI1_SS0_C; dwc-ssi drives SS0 per transfer) + per-phase READY gating. The bridge is NOT CS-less and NOT a GPIO bodge.
  - **DHCP/IP needs the lwIP stack up** (`network_stack_init` once at boot, before any `Wlan_*`, + `network_stack_add_if_sta` before `Wlan_RoleUp(STA)`, per `network_terminal.c:1349` / `wlan_cmd.c:700`). NOT yet integrated — adding it naïvely at boot broke the tuned radio bring-up (scan died). A v0.3 task: bring lwIP up without disturbing the boot role pre-cache + bridge suspend/reinit window.
- WIFI_SOFTAP: `network_stack_add_if_ap(); RoleUpApCmd_t ap={.ssid,.channel,.secParams{Type,Key,KeyLen}}; Wlan_RoleUp(WLAN_ROLE_AP,&ap,...);` then `network_set_up(ap_if)`. Peers: `WLAN_EVENT_ADD_PEER/REMOVE_PEER`.
- WIFI_RSSI (blocking): `WlanBeaconRssi_t r={.role_id=0}; Wlan_Get(WLAN_GET_RSSI,&r);` → `r.rssi_beacon`.
- WIFI_GET_IP: `network_stack_get_if_ip(WLAN_ROLE_STA,&ip,&mask,&gw,&dhcp);` (net-order u32; poll until ip!=0).

## Per-HAL bodies (BLE) — NimBLE
- BLE_ENABLE: `nimble_host_start()` (budget >2s); ready = `ble_hs_is_enabled()`.
- BLE_ADVERTISE (ext-adv API, BLE_EXT_ADV=1): `ble_gap_ext_adv_configure(inst,&params,NULL,gap_event_cb,NULL)` → `ble_hs_adv_set_fields_mbuf` + `ble_gap_ext_adv_set_data` → `ble_gap_ext_adv_start(inst,dur,maxev)`. Name via `ble_svc_gap_device_name_set`.
- BLE_SCAN: `ble_gap_ext_disc(own_addr,dur,period,filt_dup,filt_pol,0,&uncoded,&coded,gap_event_cb,NULL)`; results = `BLE_GAP_EVENT_EXT_DISC` (`ext_disc.{addr,rssi,data,length_data}`, parse via `ble_hs_adv_parse_fields`); done = `BLE_GAP_EVENT_DISC_COMPLETE`. Pack ~28B/rec ⇒ ~17/frame.
- BLE_CONNECT: `ble_addr_t peer; BLE_COPY_BD_ADDRESS(peer.val,bd)` (**byte-reverses!**); `ble_gap_ext_connect(own_addr,&peer,0,PHY_1M|2M,...,gap_event_cb,NULL)`; result `BLE_GAP_EVENT_CONNECT{status,conn_handle}`. Disconnect `ble_gap_terminate`.
- BLE_GATT server: static `ble_gatt_svc_def[]` + `ble_gatts_count_cfg`+`ble_gatts_add_svcs` (BEFORE host task); access cb read=`os_mbuf_append`, write=`ble_hs_mbuf_to_flat`; notify=`ble_gatts_notify_custom`. Client (no TI example): `ble_gattc_disc_all_svcs/chrs`, `ble_gattc_read`, `ble_gattc_write_flat`, subscribe by writing CCCD; add `BLE_GAP_EVENT_NOTIFY_RX` to `gap_event_cb`.

## Coexistence: Wi-Fi + BLE run CONCURRENTLY — bench-validated 2026-06-24.
- Bench-validated 2026-06-24 (E1M-AEN801 EVK, fw v0.0.207.0): `wifi scan`, `ble enable`
  (NimBLE host up), and `wifi connect` (WPA3, async — shell stays live) all succeed TOGETHER.
  The bridge survives the ~15s association (`ver` after a connect still returns, no desync).
  The earlier "Wi-Fi SCAN + BLE are NOT concurrent / one radio at a time / gated on the r2
  CS + host-IRQ transport" framing is RESOLVED — the bridge now runs over the proper hardware
  peripheral-driven SS0 chip-select (P14_7 = SPI1_SS0_C) + per-phase READY gating.
- Validated on the AEN801 EVK bench (2 boards). Not a production-certification claim.
- Background on the earlier SoftGemini investigation (FW 1.8.0.42, TI **OSPREY_MX-1518**
  "degraded scan in coex", `CMD_STATUS_REJECT_MEAS_SG_ACTIVE = 11`): the 2026-06-22/23 scan
  rejections under BLE were observed on the CS-less r1 transport; the hardware SS0 + per-phase
  READY bridge no longer hits the wall on the bench. If a full-band survey is ever rejected while
  BLE is active on a given FW build, that arbiter behaviour (not the transport) would be the
  remaining lever; keep an eye on the TI NWP FW. See `docs/cc3501e-production.md`
  "Wi-Fi + BLE concurrency".

## Build order (P0-4..P0-6 then pillars)
P0-4 worker (offload from ISR) → P0-5 link the full set + compile osi_dpl.c/network_lwip.c/adaptation
→ P0-6 GET_MAC (first Wlan_* ref ⇒ proves the link). Then WIFI scan/connect/AP, then BLE.
build_ti.ps1 `-WifiHostDriver` currently links ONLY wifi_host_driver.a+wifi_platform.a (INCOMPLETE — extend).
