/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware-abstraction shim consumed by firmware/cc3501e/src/.
 * Each function maps a firmware-level operation onto the CC3501E
 * silicon via TI's SimpleLink CC33xx SDK / driverlib.
 *
 * The default implementations in hal/cc3501e_hw_stub.c keep the
 * protocol path exercisable on the host with no TI SDK (every
 * HW-touching op returns CC3501E_HW_ERR_NOTIMPL).  The real
 * implementations live under hal/ti/ (CC3501E_HAL_BACKEND=ti, linked
 * against TI's SimpleLink SDK on the bench build).
 *
 * v0.1 ("bring-up") surface is intentionally tiny: chip init, the idle
 * housekeeping tick, the factory MAC read, and a deferred self-reset.
 * The Wi-Fi / BLE / GPIO-proxy / camera-enable HAL surfaces land in
 * v0.2+ alongside their protocol handlers (see docs/cc3501e-bridge.md
 * "v0.x roadmap").
 */

#ifndef CC3501E_BRIDGE_HAL_CC3501E_HW_H
#define CC3501E_BRIDGE_HAL_CC3501E_HW_H

#include <stddef.h> /* size_t (cc3501e_hw_wifi_scan) */
#include <stdint.h>

/* Return codes.  0 = success; negatives are operation-independent
 * failure classes (mirrors the gd32-bridge BRIDGE_HW_* convention). */
#define CC3501E_HW_OK          0
#define CC3501E_HW_ERR_NOTIMPL -1 /* op not wired on this backend (stub)   */
#define CC3501E_HW_ERR_IO      -2 /* peripheral / radio access failed       */
#define CC3501E_HW_ERR_INVAL   -3 /* bad argument                           */
#define CC3501E_HW_BUSY        1  /* op accepted, runs off-ISR; caller must re-poll */

/* --------------------------------------------------------------- */
/* Lifecycle                                                         */
/* --------------------------------------------------------------- */

/* One-time chip bring-up: clocks, power, and any board-level pin setup
 * the firmware owns before a transport is started.  No-op on the stub. */
void cc3501e_hw_init(void);

/* Periodic housekeeping, run on each WFI wakeup from main().  No-op on
 * this firmware rev; reserved for watchdog kick / deferred work. */
void cc3501e_hw_tick(void);

/* Bring the radio up ONCE at boot (radio<->SPI coexistence fix).
 *
 * The inter-chip bridge SPI slave cannot be serviced while the CC35 runs a
 * radio op (there is no host-IRQ to defer to), so the bridge is DOWN for the
 * duration of Wlan_Start / Wlan_RoleUp -- seconds.  Doing that radio init
 * LAZILY on the first GET_MAC tears the link down mid-traffic (ping climbs a
 * few, then reqhdr_rx -> 0x00000000 + desync).  Instead the bring-up task
 * calls this ONCE, early -- after the FreeRTOS scheduler + the SPI slave poll
 * task are up but BEFORE any host command is serviced -- so the long radio
 * init happens while there is no host traffic to disrupt (the bridge-down
 * window is then harmless).  After it returns the bridge is up AND the radio
 * is up, so a later GET_MAC only does the SHORT Wlan_Get.
 *
 * Idempotent (one-time guard inside): a no-op on the second+ call and on the
 * stub / silicon-free build (no radio -> nothing to start). */
void cc3501e_hw_wifi_boot_start(void);

/* Bring up the lwIP TCP/IP core (tcpip_init) ONCE at boot.  MUST be called EARLY --
 * from main()'s bring-up task BEFORE transport_spi_init() spawns the busy-poll bridge
 * slave task and before the radio is lazy-started -- because tcpip_init waits for the
 * lwIP thread to start, which the busy-poll task would otherwise starve (and the radio
 * would otherwise have eaten the heap the tcpip stack needs).  Prerequisite for the
 * STA netif (network_stack_add_if_sta) the Wi-Fi connect path needs.  No-op on the
 * stub / silicon-free build and on the ti build without CC3501E_WIFI (no lwIP). */
void cc3501e_hw_net_init(void);

/* --------------------------------------------------------------- */
/* Meta operations                                                   */
/* --------------------------------------------------------------- */

/* Read the CC3501E's 6-byte factory MAC into @p mac (TI wire order).
 * Returns CC3501E_HW_OK on success, CC3501E_HW_ERR_NOTIMPL on the stub
 * backend (the protocol layer maps that to RESP_ERR_NOT_READY). */
int cc3501e_hw_get_mac(uint8_t mac[6]);

/* Request a soft self-reset.  The implementation DEFERS the actual
 * reboot until the in-flight reply has been clocked back to the host
 * (so the host sees the CMD_RESET ack), then resets the chip.  No-op on
 * the stub backend. */
void cc3501e_hw_request_reset(void);

/* Transport -> HAL: the in-flight reply frame has been FULLY clocked back
 * to the host.  The slave-side SPI/SDIO transport calls this from its
 * reply-complete path.  The deferred-reset latch (cc3501e_hw_request_reset)
 * only fires the reboot once this has been observed for the CMD_RESET ack,
 * so the host always sees the ack before the link goes quiet.  No-op on
 * the stub backend. */
void cc3501e_hw_notify_reply_sent(void);

/* --------------------------------------------------------------- */
/* GPIO proxy (v0.4) + camera enables                                */
/* --------------------------------------------------------------- */

/* The CC3501E fronts E1M pads IO11 / IO13 / IO15..IO21 (plus mux/wake
 * lines) and the two camera-enable LDOs, per
 * metadata/e1m_modules/aen/from-cc3501e.tsv.  These shims drive the
 * proxied CC3501E GPIOs on the Alif's behalf.  @p pad is the CC3501E
 * GPIO index; @p dir / @p pull / @p edge use the alp_cc3501e_gpio_*
 * enums in <alp/protocol/cc3501e.h>.  Return CC3501E_HW_* (NOTIMPL maps
 * to RESP_ERR_NOT_READY at the protocol layer). */
int cc3501e_hw_gpio_configure(uint8_t pad, uint8_t dir, uint8_t pull);
int cc3501e_hw_gpio_write(uint8_t pad, uint8_t level);
int cc3501e_hw_gpio_read(uint8_t pad, uint8_t *level_out);
int cc3501e_hw_gpio_set_interrupt(uint8_t pad, uint8_t edge, uint8_t enabled);

/* Camera-enable LDOs (per the E1M-AEN BDE-BW35N U4 netlist): @p which 0 ->
 * CAM_EN_LDO0 = GPIO_1 (pin54), 1 -> CAM_EN_LDO1 = GPIO_0 (pin55); @p on != 0
 * asserts the enable.  Default OFF at boot. */
int cc3501e_hw_cam_enable(uint8_t which, uint8_t on);

/* --------------------------------------------------------------- */
/* Wi-Fi (v0.2)                                                      */
/* --------------------------------------------------------------- */

/* Route to TI's SimpleLink Wi-Fi host (sl_Wlan* / sl_NetApp*) in the ti
 * backend; the stub + the silicon-free host build report NOTIMPL (->
 * RESP_ERR_NOT_READY).  @p security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE
 * (matches alp_cc3501e_wifi_connect_t.security).  ssid/psk are not NUL-
 * terminated; the lengths are authoritative. */
int cc3501e_hw_wifi_scan_start(void);
int cc3501e_hw_wifi_scan_stop(void);

/* Run a Wi-Fi scan and PACK the resulting AP list into @p buf in the host's
 * wire format -- per record: bssid[6] | rssi(1) | channel(1) | security(LE16) |
 * ssid_len(1) then ssid_len SSID bytes (the cc3501e_wifi_scan parser's
 * CC3501E_SCAN_REC_HDR=11 layout; security is the raw 16-bit TI SecurityInfo so
 * the host can decode open / WPA2 / WPA3).  Records are packed until @p cap
 * would be exceeded; *out_len receives the total bytes written.  Worker-routed
 * (the scan blocks for seconds), so this runs off the SPI ISR.  Returns
 * CC3501E_HW_OK on success; the stub / silicon-free build reports
 * CC3501E_HW_ERR_NOTIMPL with *out_len = 0. */
int cc3501e_hw_wifi_scan(uint8_t *buf, size_t cap, size_t *out_len);
int cc3501e_hw_wifi_connect_sta(const uint8_t *ssid,
                                uint8_t        ssid_len,
                                const uint8_t *psk,
                                uint8_t        psk_len,
                                uint8_t        security);
int cc3501e_hw_wifi_disconnect(void);
int cc3501e_hw_wifi_ap_start(const uint8_t *ssid,
                             uint8_t        ssid_len,
                             const uint8_t *psk,
                             uint8_t        psk_len,
                             uint8_t        security);
int cc3501e_hw_wifi_ap_stop(void);
int cc3501e_hw_wifi_get_rssi(int8_t *rssi_dbm_out);
int cc3501e_hw_wifi_get_ip(uint8_t ip_out[4]);

/* ---- async-connect status latch (CMD_WIFI_STATUS) -------------------------- *
 * The connect body (cc3501e_hw_wifi_connect_sta) BLOCKS for seconds on the
 * association event, so it is worker-routed off the SPI ISR.  The host no longer
 * blocks polling it; instead the firmware mirrors the outcome into a small latch
 * that the NON-blocking CMD_WIFI_STATUS reads (no radio op, ISR-safe).
 *
 * mark_connecting() is called SYNCHRONOUSLY when a connect is submitted (from the
 * protocol handler, before the drain runs the body) so the latch reads CONNECTING
 * from submit onward -- a host status poll never sees a stale CONNECTED from a
 * previous attempt during the brief queued window.  conn_status() copies the latch
 * out: @p state = alp_cc3501e_wifi_conn_state_t, @p fail_reason =
 * alp_cc3501e_wifi_fail_t (valid on FAILED), @p rssi_dbm = STA RSSI (valid on
 * CONNECTED).  Any out pointer may be NULL.  The stub / silicon-free build keeps
 * the latch DISCONNECTED. */
void cc3501e_hw_wifi_mark_connecting(void);
int  cc3501e_hw_wifi_conn_status(uint8_t *state, uint8_t *fail_reason, int8_t *rssi_dbm);

/* --------------------------------------------------------------- */
/* TCP/UDP sockets (v0.5)                                            */
/* --------------------------------------------------------------- */

/* Route to the firmware IP stack's BSD socket API (lwIP sockets: lwip_socket /
 * lwip_connect / lwip_send / lwip_recvfrom / lwip_close in the ti backend); the
 * stub + the silicon-free host build report NOTIMPL (-> RESP_ERR_NOT_READY).
 *
 * These bodies BLOCK (a socket op is a tcpip_apimsg round-trip to the lwIP core
 * thread; connect/recv can wait for the network), so the protocol handlers
 * WORKER-ROUTE all five off the SPI ISR -- exactly like the blocking Wlan_* ops.
 *
 * v1 IP-stack surface is IPv4-only: @p family is an alp_cc3501e_sock_family_t
 * (only IPV4 accepted), @p type an alp_cc3501e_sock_type_t (STREAM=TCP,
 * DGRAM=UDP), @p protocol the IP protocol number (0 = default for the type).
 * Addresses are 4 raw IPv4 octets in network (big-endian) order; @p port is
 * host byte order (the backend converts to network order on the wire). */

/* Allocate a socket; returns the firmware-side handle (non-zero; 0 is the
 * invalid handle) in @p handle_out. */
int cc3501e_hw_sock_open(uint8_t family, uint8_t type, uint8_t protocol, uint16_t *handle_out);

/* STREAM: start + complete the TCP handshake to @p addr:@p port.  DGRAM: set the
 * default peer for later sends.  Blocks until the handshake resolves. */
int cc3501e_hw_sock_connect(uint16_t handle, uint8_t family, uint16_t port, const uint8_t addr[4]);

/* Queue @p data_len bytes on the socket; @p sent_out receives the byte count the
 * stack accepted.  @p flags mirrors alp_cc3501e_sock_send_t::flags (bit 0 = MORE). */
int cc3501e_hw_sock_send(uint16_t       handle,
                         uint8_t        flags,
                         const uint8_t *data,
                         uint16_t       data_len,
                         uint16_t      *sent_out);

/* Receive up to min(@p max_len, @p cap) bytes into @p buf; @p recv_len_out gets
 * the byte count (0 = nothing available within the socket's receive timeout, or
 * peer closed -- still CC3501E_HW_OK, non-blocking semantics at the wire).  For
 * DGRAM sockets @p from_addr / @p from_port_out receive the datagram source
 * (zeroed for STREAM).  Any out pointer other than @p buf may be NULL. */
int cc3501e_hw_sock_recv(uint16_t  handle,
                         uint16_t  max_len,
                         uint8_t  *buf,
                         uint16_t  cap,
                         uint16_t *recv_len_out,
                         uint8_t   from_addr[4],
                         uint16_t *from_port_out);

/* Release the socket (STREAM: issue the TCP teardown).  The handle is invalid
 * afterwards and the firmware may reuse its value. */
int cc3501e_hw_sock_close(uint16_t handle);

/* --------------------------------------------------------------- */
/* BLE 5.4 (v0.3)                                                    */
/* --------------------------------------------------------------- */

/* Route to TI's BLE host (NimBLE, source/ti/net/ble_interface) in the ti
 * backend; the stub + the silicon-free host build report NOTIMPL.  adv/scan
 * intervals are in ms; addr is a 6-byte BLE device address; GATT ops carry
 * a 16-bit attribute handle. */
int cc3501e_hw_ble_enable(void);
int cc3501e_hw_ble_disable(void);
int cc3501e_hw_ble_adv_start(uint8_t        connectable,
                             uint16_t       interval_min_ms,
                             uint16_t       interval_max_ms,
                             const uint8_t *adv_data,
                             uint8_t        adv_data_len);
int cc3501e_hw_ble_adv_stop(void);
int cc3501e_hw_ble_scan_start(void);
int cc3501e_hw_ble_scan_stop(void);
/* Worker-routed, record-returning BLE scan (the BLE mirror of
 * cc3501e_hw_wifi_scan): PACK discovered advertisers into @p buf -- per record
 * addr[6] | addr_type(1) | rssi(1) | name_len(1) then name_len name bytes.
 * Requires the NimBLE host up (else NOTIMPL -> NOT_READY). */
int cc3501e_hw_ble_scan(uint8_t *buf, size_t cap, size_t *out_len);
int cc3501e_hw_ble_connect(uint8_t addr_type, const uint8_t addr[6]);
int cc3501e_hw_ble_disconnect(void);
/* Register a dynamic GATT service from a wire-format descriptor (see
 * ALP_CC3501E_CMD_BLE_GATT_REGISTER in <alp/protocol/cc3501e.h> for the exact
 * byte layout).  @p handles_out receives one attribute VALUE handle per
 * characteristic, in descriptor order, capped at @p handles_cap; @p
 * num_handles_out is always set (0 on any error). */
int cc3501e_hw_ble_gatt_register(const uint8_t *desc,
                                 uint16_t       desc_len,
                                 uint16_t      *handles_out,
                                 uint16_t       handles_cap,
                                 uint16_t      *num_handles_out);
int cc3501e_hw_ble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t data_len);
int cc3501e_hw_ble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len);
int cc3501e_hw_ble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t data_len);

/* --------------------------------------------------------------- */
/* OTA firmware update (over-the-bridge PSA-FWU streaming)            */
/* --------------------------------------------------------------- */

/* Stream a new signed GPE vendor image into the CC3501E's NON-primary
 * vendor slot via PSA-FWU, then install + reboot so the cold BL2/MCUboot
 * swaps it to primary.  The Alif host drives the 0x40..0x44 OTA opcodes;
 * these shims carry the session.  ONE session at a time; bytes arrive
 * SEQUENTIALLY (offset == the running cursor).  Return CC3501E_HW_*
 * (NOTIMPL -> the stub / silicon-free build maps it to RESP_ERR_NOT_READY).
 * The real bodies live in the ti backend (psa_fwu_*). */

/* Open a session: pick the non-primary vendor slot, bring it READY, latch
 * @p total_len (full image size, must exceed the manifest). */
int cc3501e_hw_ota_begin(uint32_t total_len);

/* Accept a sequential image chunk at absolute @p offset (must equal the
 * running write cursor).  The first TI_FWU_MANIFEST_SIZE bytes are buffered
 * for psa_fwu_start; the remainder is psa_fwu_write()n into the slot. */
int cc3501e_hw_ota_write(uint32_t offset, const uint8_t *data, uint32_t len);

/* Finalize: psa_fwu_finish + psa_fwu_install (CANDIDATE -> STAGED), then arm
 * a DEFERRED reboot (cc3501e_hw_tick performs it once the FINISH ack has
 * clocked back, like CMD_RESET) so BL2 swaps the slot on the next boot.
 * Errors if the stream is incomplete. */
int cc3501e_hw_ota_finish(void);

/* Cancel an in-flight session (psa_fwu_cancel) and return to IDLE. */
int cc3501e_hw_ota_abort(void);

/* Promote an ALREADY-committed pending image: arm the same deferred swap-reboot
 * that FINISH uses, without needing a fresh session.  A STAGED image survives a
 * bare nRESET (which carries no swap request) while the RAM session state resets
 * to IDLE -- so once a slot is occupied, FINISH short-circuits and the swap can
 * never be requested.  This is the unjam/promote path: cc3501e_hw_tick performs
 * the reboot once the reply has clocked back.  Not gated on ota.state (that is
 * IDLE after the reset that jammed the slot). */
int cc3501e_hw_ota_promote(void);

/* Result of the last psa_fwu_request_reboot() (0 if none requested).  Since
 * request_reboot only RETURNS on refusal (success reboots), a non-zero value
 * means the swap was REFUSED (e.g. BL2 anti-rollback on a downgrade) -- lets the
 * host distinguish "refused" from "never fired".  Surfaced in OTA_STATUS. */
int8_t cc3501e_hw_ota_reboot_rc(void);

/* Report session progress: @p state = alp_cc3501e_ota_state_t, @p
 * bytes_written = bytes accepted so far, @p total_len = the BEGIN value.
 * Any out pointer may be NULL. */
int cc3501e_hw_ota_status(uint8_t *state, uint32_t *bytes_written, uint32_t *total_len);

/* --------------------------------------------------------------- */
/* Power policy + diagnostics (configurability)                      */
/* --------------------------------------------------------------- */

/* Apply the host's power-policy hint (alp_cc3501e_power_policy_t fields:
 * coarse policy preset, wake-event bitmap, idle-before-sleep ms).  These
 * are firmware-side config knobs (no radio needed) -- the backend applies
 * what it can and returns OK. */
int cc3501e_hw_set_power_policy(uint8_t policy, uint8_t wake_events, uint32_t idle_ms_before_sleep);

/* Set firmware log verbosity (0 = off).  Firmware-side config -> OK. */
int cc3501e_hw_set_log_level(uint8_t level);

/* Diagnostics sources for GET_DIAG_INFO (best-effort; 0 / UNKNOWN when the
 * backend has no source yet).  reset_cause is an alp_cc3501e_reset_cause_t. */
uint8_t  cc3501e_hw_reset_cause(void);
uint32_t cc3501e_hw_uptime_ms(void);
uint32_t cc3501e_hw_free_heap_bytes(void);

#endif /* CC3501E_BRIDGE_HAL_CC3501E_HW_H */
