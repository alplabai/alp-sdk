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

/** Maximum simultaneous async-event subscribers per context (issue #1723).
 *  Sized for the in-tree consumers -- the Zephyr console companion plus an
 *  application -- with headroom; @ref cc3501e_add_event_callback reports
 *  @ref ALP_ERR_NOMEM rather than silently dropping one past this. */
#define CC3501E_EVENT_SUBSCRIBERS 4

struct cc3501e {
	bool initialised;
	/* Wire version the FIRMWARE reported at the last @ref cc3501e_reset
	 * (ADR 0033).  Both are 0 before the first successful GET_VERSION.
	 *
	 * fw_proto_major always equals ALP_CC3501E_PROTOCOL_MAJOR on a usable
	 * context -- a mismatch refuses the link -- so the field that carries
	 * information is fw_proto_minor: LOWER than this host's minor means the
	 * firmware lacks newer additive features.  These are also set on the
	 * REFUSAL path, so a caller that got ALP_ERR_VERSION can report what the
	 * firmware actually claimed; a fw_proto_major of 0 there means the
	 * firmware predates the scheme and answered with a raw v1..v9 integer.
	 *
	 * Prefer @ref cc3501e_get_capabilities over reasoning from the minor: it
	 * reports what the build IMPLEMENTS, not what its number implies. */
	uint8_t     fw_proto_major;
	uint8_t     fw_proto_minor;
	alp_spi_t  *bus;        /**< SPI1 to the CC3501E (Alif master). */
	alp_gpio_t *enable_pin; /**< WIFI.EN (P15_5).  May be NULL on boards that tie it on. */
	alp_gpio_t *reset_pin;  /**< E_WIFI.NRST (P15_1_FLEX). */
	alp_gpio_t *ready_pin;  /**< OPTIONAL host-IRQ/READY in (CC35 GPIO17 -> Alif P2_6):
	                                *   HIGH when the SPI slave is armed+idle.  When populated,
	                                *   cc3501e_request() waits on it before each reply phase
	                                *   instead of a fixed settle gap.  NULL = legacy fixed gap. */
	/* Async-event SUBSCRIBERS (issue #1723), not one callback slot.
	 *
	 * This used to be a single { event_cb, event_user } pair, and the last
	 * registration won -- silently.  The Zephyr console companion registers
	 * its own callback on the shared ctx from its init path and polls every
	 * ~500 ms, so it OVERWROTE the application's callback after main() had
	 * already set it: the firmware ring drained normally,
	 * cc3501e_poll_events() returned ALP_OK, and every event went to the
	 * console's sink while the application received nothing -- with no error
	 * and no way to detect it.
	 *
	 * The console and the application are both legitimate consumers of the
	 * same events, so events now fan out to EVERY registered subscriber.  A
	 * small fixed array rather than a list or an allocation: subscribers are
	 * a handful of long-lived registrations, and this keeps registration
	 * usable from an init path that has nowhere useful to report a failure. */
	struct {
		cc3501e_event_cb_t cb;
		void              *user;
	} event_subs[CC3501E_EVENT_SUBSCRIBERS];
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
	/* Socket send/recv staging.  cc3501e_sockets.c used to declare
	 * `uint8_t p[ALP_CC3501E_MAX_PAYLOAD]` (send) and
	 * `uint8_t reply[ALP_CC3501E_MAX_PAYLOAD]` (recv) as LOCALS -- 4 KB stack
	 * frames each.  The Zephyr shell thread is CONFIG_SHELL_STACK_SIZE=2048, so
	 * `alp companion sock tcp-get` overflowed it deterministically and the app
	 * took a USAGE FAULT ("Stack overflow (context area not valid)").
	 *
	 * Issue #740 already moved the scan/event decode buffers off the stack for
	 * exactly this reason; the socket path was missed by that sweep.  Same
	 * pattern, same caveat: sock_busy catches same-call-stack reentrancy, not
	 * two truly concurrent callers on one ctx. */
	uint8_t sock_buf[ALP_CC3501E_MAX_PAYLOAD];
	bool    wifi_scan_busy;
	bool    ble_scan_busy;
	bool    evt_busy;
	bool    sock_busy;
	/* CMD_SOCK_SEND retry seq (proto v7, alp-sdk#1746 / cc3501e-bridge-firmware#88).
	 * Same free-running-counter shape as spi1_seq below, owned by the driver so a
	 * transport-level retry (poll_by_repeat re-issuing the identical frame on BUSY
	 * or IO) comes back from the firmware's cached reply instead of re-submitting
	 * -- and for CMD_SOCK_SEND, re-submitting means re-TRANSMITTING the payload,
	 * not just re-clocking a read.  cc3501e_sock_send() assigns it ONCE, before
	 * the poll_by_repeat() call, so it stays constant across that call's retries;
	 * see the assignment site for why that constancy is what makes the fix work.
	 * uint8_t: wraps 255 -> 0 (defined unsigned overflow) after 256 sends, which
	 * cannot collide with the firmware's single-entry cache -- it only ever holds
	 * the immediately-preceding completed send's seq, never one from 256 sends
	 * back. */
	uint8_t sock_send_seq;

	/* Generic request retry seq (proto v8, cc3501e-bridge-firmware#102).
	 *
	 * sock_send_seq above covers ONE opcode, because v7 could only spend a
	 * spare byte that happened to exist inside alp_cc3501e_sock_send_t.  v8
	 * puts a 5-bit seq in the frame header's flags byte instead, so every
	 * worker-routed opcode gets the same protection at zero wire cost.
	 *
	 * poll_by_repeat() allocates ONE value here per LOGICAL command and
	 * re-sends it unchanged on every BUSY/IO retry of that command; the
	 * constancy across retries is the whole mechanism, exactly as for
	 * sock_send_seq.  cc3501e_request() -- the single-shot path, no retry
	 * loop -- sends ALP_CC3501E_REQ_SEQ_NONE instead, so a frame with no
	 * retry semantics can never be answered from the firmware's latch.
	 *
	 * WRAP: the space is 1..ALP_CC3501E_REQ_SEQ_LAST (31), skipping 0, so
	 * this wraps every 31 commands rather than every 256.  That is far
	 * tighter than sock_send_seq's, and it is a real residual: the firmware
	 * latch is a single entry, so a stale hit needs the same opcode, the
	 * same seq, AND no intervening worker-routed completion to overwrite the
	 * entry -- roughly 31 intervening non-worker-routed commands, which is
	 * seconds of ordinary idle rather than something exotic.  Stated, not
	 * hidden; see the s_retry_latch comment in the firmware's protocol.c. */
	uint8_t req_seq;

	/* SPI1 host-passthrough staging (proto v6, opcodes 0x55..0x57).  Same rule
	 * as sock_buf above, for the same reason: one TRANSFER chunk is
	 * ALP_CC3501E_SPI1_MAX_XFER (4088) data bytes plus its header, so a local
	 * would be a 4 KB frame on a CONFIG_SHELL_STACK_SIZE=2048 thread.
	 *
	 * TWO buffers, not one: poll_by_repeat() re-issues the request payload from
	 * this exact memory on every retry WHILE writing the reply into rx_buf, so
	 * the two must not alias -- sharing one would corrupt the frame being
	 * re-sent, and this is the retry path that exists to avoid re-clocking a
	 * flash write.  spi1_busy makes same-ctx reentrancy an explicit
	 * ALP_ERR_BUSY rather than silent aliasing (same caveat as sock_busy: it
	 * catches a reentrant call stack, not two truly concurrent callers).
	 *
	 * spi1_seq is the wire duplicate-suppression counter, owned by the driver
	 * rather than the caller so a transport-level retry of a page program comes
	 * back from the firmware's cache instead of clocking the write twice.
	 *
	 * spi1_configured is SESSION binding, not bus state: the firmware's
	 * g_configured latch and cached (seq, result) are file statics that
	 * survive an Alif reboot, while cc3501e_init() only memsets THIS side --
	 * so a fresh ctx's first TRANSFER is always seq 1, which can collide with
	 * a previous session's cached seq-1 DONE result and hand back stale RX
	 * bytes as ALP_OK with the bus never re-clocked.  Requiring a CONFIGURE
	 * in the CURRENT session before any TRANSFER closes that: CONFIGURE polls
	 * the firmware's SPI1_CONFIGURE opcode, and worker_poll()'s orphan-discard
	 * arm drops any stale cached SPI1_TRANSFER result the moment a DIFFERENT
	 * opcode polls, before this ctx can ever reach the collision.
	 *
	 * Cost: 8192 bytes of context (measured, 24696 -> 32888, GCC host build),
	 * carried by every board whether or not it drives the connector's SPI1.
	 * Accepted for now -- the context is one long-lived allocation per module,
	 * and the alternative (a Kconfig that compiles the buffers out) makes
	 * sizeof(cc3501e_t) config-dependent, which is the worse trade until a
	 * board actually runs short. */
	uint8_t spi1_tx_buf[sizeof(alp_cc3501e_spi1_transfer_t) + ALP_CC3501E_SPI1_MAX_XFER];
	uint8_t spi1_rx_buf[sizeof(alp_cc3501e_spi1_transfer_resp_t) + ALP_CC3501E_SPI1_MAX_XFER];
	bool    spi1_busy;
	bool    spi1_configured;
	uint8_t spi1_seq;
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
 * @brief Recover a bridge that has stopped answering (warm reset, keeps rails up).
 *
 * The inter-chip link can enter a state where the CC3501E is healthy but no
 * longer receives what the host clocks: requests time out (ALP_ERR_TIMEOUT) and
 * then fail (ALP_ERR_IO) indefinitely. It does not self-heal.
 *
 * Firmware-side diagnostics taken across the fault (see #1691) show the slave
 * armed and idle in its request-header phase with READY HIGH, its housekeeping
 * task still running, and its resync / arm-failure counters at zero -- i.e. the
 * firmware has no way to know anything is wrong. Only the host, which is getting
 * no answers, can tell. Hence this call.
 *
 * Issues a warm reset (nRESET only, supply left up) and confirms the link with a
 * PING; falls back to a full supply cycle if the warm reset does not take. On
 * success the link is usable again -- but the device rebooted, so the Wi-Fi
 * association, the BLE host and every open socket are gone and must be
 * re-established.
 *
 * @warning NEVER call this with an OTA in flight -- resetting mid-update destroys
 *          the partially staged image. Tearing down or re-opening the bridge
 *          during a flash operation is what #1610 traced its hangs to.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK when the link answers again; ALP_ERR_INVAL if @p ctx is NULL;
 *         otherwise the mapped error from the reset or the confirming PING.
 */
alp_status_t cc3501e_recover(cc3501e_t *ctx);

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
 * @brief Read which opcode families the firmware implements
 *        (GET_CAPABILITIES, opcode 0x06).
 *
 * **Ask this instead of inferring a feature from a version number.** The wire
 * version cannot express what this bitmap can: the firmware has real build
 * variants, and a build without Wi-Fi or without BLE reports the same wire
 * version as a full one while its socket or BLE opcodes are `NOTIMPL` stubs.
 * The bitmap is composed from those same compile-time switches.
 *
 * Because features are discovered rather than implied, adding one is a MINOR
 * bump that never refuses an existing host — see ADR 0033.
 *
 * @param ctx       Initialised driver context.
 * @param caps_out  Receives an OR of @ref alp_cc3501e_capability_t bits.
 * @return ALP_OK with @p caps_out set; ALP_ERR_INVAL if @p caps_out is NULL;
 *         ALP_ERR_IO on a short reply; mapped error otherwise. A firmware
 *         predating this opcode answers `RESP_ERR_INVALID`, which maps to
 *         @c ALP_ERR_INVAL — treat that as "no capability information", not as
 *         "no capabilities".
 */
alp_status_t cc3501e_get_capabilities(cc3501e_t *ctx, uint32_t *caps_out);

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

/* ------------------------------------------------------------------ */
/* SPI1 host passthrough (proto v6, opcodes 0x55..0x57).               */
/*                                                                    */
/* The E1M connector's SPI1 lands on the CC3501E, NOT on the Alif      */
/* (E1M-AEN-2626-R2 netlist: AG10 SCK -> CC35 GPIO_32, AG9 MOSI ->     */
/* GPIO_33, AG8 MISO -> GPIO_34, AH9 CS0 -> GPIO_31, AH8 CS1 ->        */
/* GPIO_15).  A device on that bus is therefore reached by RELAY: the  */
/* CC3501E is the SPI controller and these three calls hand it the     */
/* bytes.  Nothing here touches the inter-chip bridge itself -- that   */
/* is a different CC35 instance (SPI0, GPIO_27/28/29 + GPIO16).        */
/*                                                                    */
/* Shape of a transaction:                                            */
/*                                                                    */
/* @code                                                              */
/*   uint16_t max_xfer = 0;                                           */
/*   cc3501e_spi1_configure(&fw, 10000000, 0, ALP_CC3501E_SPI1_CS0,    */
/*                          NULL, &max_xfer, 1000);                   */
/*   cc3501e_spi1_transfer(&fw, cmd, NULL, 4, 0, true, 1000);  // hold */
/*   cc3501e_spi1_transfer(&fw, NULL, page, 256, 0xFF, false, 1000);   */
/*   cc3501e_spi1_release(&fw, 1000);                                 */
/* @endcode                                                           */
/*                                                                    */
/* CHUNKING: chunk at @p max_xfer from the CONFIGURE reply, never at   */
/* the far-end device's page size.  A board without the READY pad's    */
/* input-enable pinctrl group has no working READY line (chips/        */
/* cc3501e/cc3501e_sockets.c, silicon-measured 2026-08-24) and falls   */
/* back to fixed settle gaps, where per-transaction latency dominates  */
/* -- hold CS and let one big chunk straddle page boundaries, because  */
/* 64 page-sized chunks cost 64 round trips where one costs one.       */
/* Short chunks belong only at the tail, on any board.                 */
/*                                                                    */
/* CS TIMING: both selects are software-driven GPIOs on the CC3501E    */
/* side, so CS edges are scheduler-timed, not clock-edge-exact.  A     */
/* peripheral that demands sub-microsecond CS-to-first-clock setup     */
/* will not work over this path, and no protocol change fixes that.    */
/* ------------------------------------------------------------------ */

/**
 * @brief Acquire the CC3501E's SPI1 controller and pin the bus parameters
 *        (SPI1_CONFIGURE, 0x55).
 *
 * Idempotent: re-issuing it re-opens the controller with new parameters.  The
 * settings hold until the next configure or @ref cc3501e_spi1_release.
 *
 * Word size is fixed at 8 bits -- the only value v6 firmware accepts -- so it
 * is not a parameter here; the field exists on the wire for a later firmware.
 *
 * @param ctx                 Initialised bridge handle.
 * @param freq_hz             REQUESTED SCK in Hz.  The divider rounds.
 * @param mode                SPI mode 0..3, i.e. (CPOL << 1) | CPHA.
 * @param cs                  Which software chip-select to drive
 *                            (@ref ALP_CC3501E_SPI1_CS0 = GPIO_31 / E1 AH9,
 *                            @ref ALP_CC3501E_SPI1_CS1 = GPIO_15 / E1 AH8).
 * @param actual_freq_hz_out  Optional; receives the SCK the divider ACTUALLY
 *                            produced.  Read it rather than assume the request
 *                            was met -- a real clock divides.
 * @param max_xfer_out        Optional; receives the peer firmware's per-chunk
 *                            byte limit.  Chunk to this, not to a constant.
 * @param timeout_ms          Caller budget (worker-routed, so poll-by-repeat).
 * @return ALP_OK on success; ALP_ERR_INVAL on a bad @p mode / @p cs or a
 *         payload the firmware refused; ALP_ERR_BUSY when CS is still held by
 *         an unfinished CS_HOLD chain (finish it, or release first) -- this is
 *         a terminal reject, not a retryable busy; ALP_ERR_IO if the
 *         controller could not be opened; ALP_ERR_NOT_READY on a firmware
 *         build without the passthrough.
 */
alp_status_t cc3501e_spi1_configure(cc3501e_t            *ctx,
                                    uint32_t              freq_hz,
                                    uint8_t               mode,
                                    alp_cc3501e_spi1_cs_t cs,
                                    uint32_t             *actual_freq_hz_out,
                                    uint16_t             *max_xfer_out,
                                    uint32_t              timeout_ms);

/**
 * @brief Clock one full-duplex chunk on the CC3501E's SPI1 (SPI1_TRANSFER, 0x56).
 *
 * A NULL buffer drops that direction from the wire: @p tx NULL clocks @p len
 * copies of @p tx_fill (flash read, FIFO drain), @p rx NULL discards MISO
 * (page program, display refresh).  Each drop removes up to 4 KB from a link
 * where per-transaction latency, not bandwidth, is what this bus costs --
 * worth more here than it looks on a board without the READY pad's
 * input-enable pinctrl group, which has no working READY line at all
 * (chips/cc3501e/cc3501e_sockets.c, silicon-measured 2026-08-24).
 *
 * CS is under explicit caller control: @p cs_hold false is the cheap
 * single-shot (assert, clock, deassert); @p cs_hold true leaves CS asserted so
 * the next call continues the SAME device transaction.  Clear it on the last
 * chunk.  @p len 0 with @p cs_hold false is a pure CS deassert, which is why
 * this family needs no separate chip-select opcode.
 *
 * @warning RETRY SEMANTICS, because this bus will drive flash.  The driver
 *          stamps each transfer with a sequence byte, so the TRANSPORT-level
 *          retries inside this call (firmware busy, bridge momentarily down)
 *          come back from the firmware's cached result and never re-clock the
 *          device.  A CALLER-level retry after ALP_ERR_TIMEOUT is a NEW
 *          transfer and WILL clock the device again -- for a page program that
 *          is a second write, not a repeated read.  Read status back and decide
 *          rather than blindly re-issuing.
 *
 * @param ctx         Initialised bridge handle, already configured.
 * @param tx          Bytes to clock out, or NULL to clock @p tx_fill instead.
 * @param rx          Receives exactly @p len bytes on ALP_OK, or NULL to
 *                    discard MISO.
 * @param len         Bytes to clock, 0..@c ALP_CC3501E_SPI1_MAX_XFER (chunk at
 *                    the max_xfer the peer reported, see above).
 * @param tx_fill     Byte clocked out when @p tx is NULL.
 * @param cs_hold     Leave CS asserted after this chunk.
 * @param timeout_ms  Caller budget (worker-routed, so poll-by-repeat).
 * @return ALP_OK with @p rx filled; ALP_ERR_INVAL if @p len exceeds the chunk
 *         limit or the firmware refused the frame; ALP_ERR_NOT_READY if no
 *         configure has succeeded (or the firmware lacks the passthrough);
 *         ALP_ERR_BUSY either because the controller refused the transfer (a
 *         terminal reject -- deterministic, retrying will not fix it) or
 *         because another call on this @p ctx is mid-transfer; ALP_ERR_IO on a
 *         short or mismatched reply (link desync); ALP_ERR_TIMEOUT if the
 *         firmware worker never produced a result inside the budget -- note a
 *         long Wi-Fi scan shares that single worker slot.
 */
alp_status_t cc3501e_spi1_transfer(cc3501e_t     *ctx,
                                   const uint8_t *tx,
                                   uint8_t       *rx,
                                   uint16_t       len,
                                   uint8_t        tx_fill,
                                   bool           cs_hold,
                                   uint32_t       timeout_ms);

/**
 * @brief Deassert CS, close the SPI1 controller, free the bus
 *        (SPI1_RELEASE, 0x57).
 *
 * The escape hatch, so it has no preconditions and cannot fail on state:
 * calling it with nothing open returns ALP_OK.  A host that lost track of a
 * CS_HOLD chain (a timeout mid-chain, a restarted application) always has this
 * one call back to a known-free bus.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget (worker-routed, so poll-by-repeat).
 * @return ALP_OK once the bus is free; ALP_ERR_INVAL on a NULL @p ctx;
 *         otherwise the mapped transport error.
 */
alp_status_t cc3501e_spi1_release(cc3501e_t *ctx, uint32_t timeout_ms);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_CORE_H */
