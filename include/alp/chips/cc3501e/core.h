/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file core.h
 * @brief CC3501E driver context, lifecycle, and request primitive.
 *
 * Shared types every other subheader under `alp/chips/cc3501e/` depends on:
 * the driver context (@ref cc3501e_t), the async-event callback typedef,
 * and the init / reset / sync / version lifecycle.  Included by the
 * `<alp/chips/cc3501e.h>` umbrella; also includable on its own by code
 * that only needs the context type + lifecycle (e.g. a backend that
 * receives an already-initialised handle).
 */

#ifndef ALP_CHIPS_CC3501E_CORE_H
#define ALP_CHIPS_CC3501E_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc3501e cc3501e_t;

/** Async event callback -- runs on the driver's RX thread.
 *  @p cmd is the event opcode (one of `ALP_CC3501E_EVT_*`),
 *  @p payload + @p len carry the event-specific data described
 *  in `<alp/protocol/cc3501e.h>`. */
typedef void (*cc3501e_event_cb_t)(uint8_t cmd, const uint8_t *payload, size_t len, void *user);

struct cc3501e {
	bool               initialised;
	alp_spi_t         *bus;        /**< SPI1 to the CC3501E (Alif master). */
	alp_gpio_t        *enable_pin; /**< WIFI.EN (P15_5).  May be NULL on boards that tie it on. */
	alp_gpio_t        *reset_pin;  /**< E_WIFI.NRST (P15_1_FLEX). */
	alp_gpio_t        *ready_pin;  /**< OPTIONAL host-IRQ/READY in (CC35 GPIO17 -> Alif P2_6):
	                                *   HIGH when the SPI slave is armed+idle.  When populated,
	                                *   cc3501e_request() waits on it before each reply phase
	                                *   instead of a fixed settle gap.  NULL = legacy fixed gap. */
	cc3501e_event_cb_t event_cb;
	void              *event_user;
	/* Framing scratch for cc3501e_request() (#740 scope note): these were
	 * ALREADY per-instance fields before this change (never the
	 * function-local `static` pattern the scan/event buffers below used
	 * to have) and their lifetime was already bounded to a single
	 * cc3501e_request() call -- filled, consumed by the caller-supplied
	 * rx_buf memcpy, and never read back across calls -- so they carry
	 * none of the #740 aliasing risk and needed no change here. */
	uint8_t rx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
	uint8_t tx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
	/* Per-context decode scratch for the scan/event helpers (issue #740).
	 * Each of these used to be a function-local `static` buffer in
	 * cc3501e_wifi.c / cc3501e_ble.c / cc3501e_events.c -- process-global
	 * storage shared by EVERY cc3501e_t instance, so two contexts (or a
	 * caller re-entering the same context, e.g. from inside the event
	 * callback) could alias and corrupt each other's in-flight decode.
	 * Moving the storage in here makes it per-instance, matching
	 * rx_scratch/tx_scratch above; the *_busy flags make same-instance
	 * reentrancy an explicit ALP_ERR_BUSY instead of silent aliasing (see
	 * cc3501e_wifi_scan / cc3501e_ble_scan / cc3501e_poll_events).
	 *
	 * Three SEPARATE ALP_CC3501E_MAX_PAYLOAD (512 B) buffers -- not one
	 * shared "radio scratch" reused across all three helpers -- because
	 * cc3501e_poll_events() is meant to be polled from a low-rate app
	 * thread (or the CONFIG_ALP_SDK_CC3501E_EVENT_IRQ workqueue)
	 * regardless of whatever else the app is doing with the SAME ctx, so
	 * an app that calls e.g. cc3501e_wifi_scan() and polls events from a
	 * different call site must not have one invalidate the other's
	 * decode; and collapsing wifi_scan_buf/ble_scan_buf into one shared
	 * buffer would silently reintroduce the exact same-ctx aliasing this
	 * struct exists to remove the moment an app pipelines a Wi-Fi scan
	 * and a BLE scan close together.  This grows sizeof(cc3501e_t) from
	 * 1088 to 2632 bytes (measured, GCC 13.3 host build) -- accepted
	 * because the driver context is a small, fixed number of
	 * long-lived, typically-static allocations per module (one CC3501E
	 * per E1M-AEN board), not a per-connection/per-packet object; halving
	 * the number of scratch buffers would only save ~1.5 KB while giving
	 * up the correctness property #740 exists for. */
	uint8_t wifi_scan_buf[ALP_CC3501E_MAX_PAYLOAD];
	uint8_t ble_scan_buf[ALP_CC3501E_MAX_PAYLOAD];
	uint8_t evt_buf[ALP_CC3501E_MAX_PAYLOAD];
	bool    wifi_scan_busy;
	bool    ble_scan_busy;
	bool    evt_busy;
	/* Transport-transaction lock (issue #1116): serialises the whole
	 * cc3501e_request() 4-phase exchange (and therefore tx_scratch /
	 * rx_scratch above) across every caller sharing this ctx -- the
	 * Wi-Fi / BLE / GPIO-proxy backends, the console companion, and the
	 * OTA path.  A plain flag guarded by compiler-builtin atomics
	 * (__atomic_* in cc3501e_core.c), not an OS mutex: this driver core
	 * is OS-agnostic (chips/cc3501e/cc3501e_core.c links into the
	 * Zephyr module AND the plain-CMake / Yocto libalp_chips.a build,
	 * per CMakeLists.txt's ALP_SDK_CHIP_LIST comment), so it cannot call
	 * k_mutex_*.  Same rationale as src/common/alp_slot_claim.h's
	 * lock-free slot claim.  Never touch directly -- go through
	 * cc3501e_request(). */
	bool request_lock;
};

/**
 * @brief Initialise the driver and bind it to an open SPI1 bus.
 *
 * Does not enable the radio -- call @ref cc3501e_reset to bring
 * the firmware up.  @p bus must remain valid for the lifetime
 * of @p ctx.
 */
alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus);

/**
 * @brief Pulse the firmware's reset line, de-assert WIFI.EN, then enforce
 *        wire-protocol compatibility.
 *
 * Blocks for the TI SWRU626 cold-boot budget, then reads @c GET_VERSION.
 * If the round trip completes and the reply differs from this host's
 * @c ALP_CC3501E_PROTOCOL_VERSION, the context is refused: @p ctx is left
 * uninitialised (every later call returns @ref ALP_ERR_NOT_READY) and this
 * returns @ref ALP_ERR_VERSION -- retrying cannot reconcile two binaries
 * that disagree about the wire (#1371).  A round trip that does not
 * complete at all (the common case immediately after a cold boot -- see
 * @ref cc3501e_hard_reset's Puya-flash note) is NOT a version verdict:
 * @p ctx is left usable so a caller's own retry (another
 * @ref cc3501e_hard_reset) can still align the link.
 *
 * @ref cc3501e_get_version stays a bare round-trip with no comparison of
 * its own -- callers that use it as a liveness probe (not a compat gate)
 * are unaffected by the refusal above.
 */
alp_status_t cc3501e_reset(cc3501e_t *ctx);

/**
 * @brief Cut the CC3501E's supply and leave it off (WIFI_EN low).
 *
 * The deepest power state available, and far below anything
 * @ref cc3501e_power_policy can reach: WIFI_EN gates VPA (3.3 V) through the
 * board's load switch, so this removes the companion's power rather than idling
 * it.  Intended for LONG idle periods -- a node that uplinks once an hour or once
 * a day spends almost all its life here, and at that duty cycle the sleep-state
 * current is irrelevant next to simply having the chip off.
 *
 * Use @ref cc3501e_power_policy instead for short gaps: it keeps the association
 * and wakes on the next SPI frame, where this costs a full cold boot.
 *
 * EXPLICIT ONLY.  The application on the host decides when the companion is not
 * needed and calls this; nothing in the driver, and no power preset, ever powers
 * the device down on its own.  A duty cycle is a product decision -- only the
 * application knows when the next uplink is due and whether anything is in
 * flight -- so it is never inferred from an idle timer down here.
 *
 * ALL DEVICE STATE IS LOST.  The Wi-Fi association, the BLE host, every open
 * socket and any OTA session are gone; the secure boot chain re-runs from
 * scratch on the way back up.  Bringing it back is @ref cc3501e_reset, which
 * runs the cold-boot sequence (rail discharge, supply ramp, reset release, boot
 * budget) and re-arms this context -- budget on the order of a second and a half,
 * plus re-association.
 *
 * Until then every other call on @p ctx returns ALP_ERR_NOT_READY immediately,
 * rather than clocking frames at an unpowered slave and burning a timeout each.
 *
 * @warning NEVER call this with an OTA in flight -- it destroys the partially
 *          staged image, and the device cannot report that it happened.  The
 *          caller owns that sequencing; the driver does not track it.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK once the supply is gated; ALP_ERR_INVAL if @p ctx is NULL;
 *         ALP_ERR_NOT_PRESENT_ON_THIS_SOC on a board that ties WIFI_EN on, where
 *         software cannot gate the rail.
 */
alp_status_t cc3501e_power_off(cc3501e_t *ctx);

/**
 * @brief Warm hard reset: pulse nRESET with WIFI_EN kept asserted (rails stay up).
 *
 * Re-boots the module WITHOUT a cold power cycle.  This is the "second boot" of the
 * CC3501E Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround: a cold power-on
 * mis-reads the Puya flash on the FIRST boot (TI SDK bug, 32/64Mbit Puya parts), so
 * the secure boot never launches the vendor image; a hard reset re-boots with the
 * flash settled and the image launches.  @ref cc3501e_reset already issues one such
 * re-boot after the cold power-up; call this again (e.g. from a soak/retry loop) if
 * a single re-boot has not brought the link up.  Remove once TI ships the flash fix.
 *
 * @param ctx Initialised driver context (must have @c reset_pin populated).
 * @return ALP_OK after the re-boot budget elapses; ALP_ERR_NOSUPPORT if no reset pin.
 */
alp_status_t cc3501e_hard_reset(cc3501e_t *ctx);

/**
 * @brief (Re)establish byte alignment on the CS-less 3-wire link.
 *
 * With no chip-select to delimit transactions, the master and slave keep
 * framing by fixed clock count alone; a missed/extra clock (or a slave
 * that booted mid-transaction) leaves them byte-misaligned with no edge to
 * recover on.  This walks the SPI byte phase until it observes the slave's
 * header-idle marker (@ref ALP_CC3501E_SYNC_IDLE, driven only when the
 * slave is parked at a clean request-header boundary), confirming with two
 * consecutive aligned reads to reject a stray marker byte inside reply
 * data.  Call it before the first request after reset, and on any
 * desync the request path detects (reply header that doesn't echo the
 * command).
 *
 * Thread-safe (issue #1116): the byte-walk clocks the same CS-less bus as
 * @ref cc3501e_request, so it runs under the same transport lock and holds
 * it for the whole walk — re-aligning to the slave's header boundary is
 * only meaningful if nothing else moves the bus underneath it.  A request
 * issued concurrently therefore gets @ref ALP_ERR_BUSY from its own bounded
 * acquire, which is the honest answer: the link is by definition unusable
 * until the re-sync completes.
 *
 * @param ctx         Initialised driver context.
 * @param timeout_ms  Coarse upper bound on re-sync effort (each ~ms covers
 *                    one full-frame byte-walk attempt).
 * @return ALP_OK once aligned; ALP_ERR_TIMEOUT if the slave never parked
 *         (e.g. unpowered / not running its firmware); ALP_ERR_BUSY if the
 *         transport lock was not acquired within its bounded timeout.
 */
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Retrieve the firmware's reported protocol version.
 *
 * A bare @c GET_VERSION round-trip -- it does NOT compare the reply against
 * `ALP_CC3501E_PROTOCOL_VERSION` itself; @ref cc3501e_reset performs that
 * comparison (and refuses a mismatch) once, right after the cold-boot
 * completes (#1371).  This function stays a pure liveness/diagnostic probe
 * deliberately, so that callers which use it that way (the cold-boot soaks
 * in examples/aen/aen-cc3501e-bringup and examples/peripheral-io/alp-console)
 * keep working: `ALP_OK` here means "the round trip completed", nothing
 * about wire compatibility.
 */
alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out);

/**
 * @brief Send one FRAMED bulk-data frame to the CC3501E stream sink (proto v2).
 *
 * Wraps @ref ALP_CC3501E_CMD_STREAM_WRITE -- the request payload (@p len bytes)
 * is clocked in a single SPI transfer, so it rides the host peripheral-DMA path
 * when @p len reaches the SPI DMA threshold (@c CONFIG_SPI_DW_ALIF_DMA_MIN_LEN).
 * The firmware sinks + acks the frame, so unlike raw throwaway clocking the link
 * stays framed and never desyncs.  Send frames back-to-back for a bulk stream.
 *
 * @param ctx   Initialised, reset driver context.
 * @param data  Bulk bytes to send (may be NULL only if @p len is 0).
 * @param len   Byte count, at most @c ALP_CC3501E_MAX_PAYLOAD minus the header.
 * @return ALP_OK on ack; ALP_ERR_INVAL on a bad arg / oversized frame; the
 *         mapped firmware status otherwise.
 */
alp_status_t cc3501e_stream_write(cc3501e_t *ctx, const uint8_t *data, size_t len);

/**
 * @brief Issue a synchronous command + wait for the response.
 *
 * Thread-safe (issue #1116): the whole 4-phase request/reply exchange --
 * and the @c tx_scratch / @c rx_scratch it reads and writes -- runs under
 * @p ctx's internal transport lock, so concurrent callers on the same
 * @p ctx (Wi-Fi, BLE, GPIO proxy, console, OTA, ...) serialise instead of
 * interleaving frames on the CS-less SPI link.  The lock acquire itself is
 * BOUNDED (@c CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS, default
 * 100 ms on Zephyr): a caller stuck behind another transaction gets @ref
 * ALP_ERR_BUSY back rather than blocking forever.  Not re-entrant -- do
 * not call this (directly or via a wrapper) from inside a callback this
 * same call chain invokes; no current caller does (the async event
 * callback runs only after cc3501e_poll_events()'s own call has already
 * returned and released the lock).
 *
 *  @param ctx         CC3501E driver context (must be initialised first).
 *  @param cmd         Command opcode (one of @c ALP_CC3501E_CMD_* ).
 *  @param tx_payload  Outbound payload bytes (may be NULL with len 0).
 *  @param tx_len      Outbound payload length in bytes.
 *  @param rx_buf      Reply buffer (response payload, less the
 *                     frame header).  Truncated to @p rx_cap.
 *  @param rx_cap      Capacity of @p rx_buf in bytes.
 *  @param rx_len      Receives bytes copied (may be NULL).
 *  @param timeout_ms  Max wait.
 *  @return ALP_OK on success; ALP_ERR_BUSY if the transport lock was not
 *          acquired within the bounded timeout; otherwise the mapped
 *          firmware status or a transport error. */
alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_CORE_H */
