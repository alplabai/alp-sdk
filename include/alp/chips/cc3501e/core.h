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

#include <assert.h> /* static_assert (C11) / _Static_assert fallback -- see the
                      * cc3501e_link_log_entry_t size check below */
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

/** Recovery callback (issue #2126) -- see @ref cc3501e_set_recover_callback.
 *  @p ctx is the context that just recovered, @p recover_count is its
 *  post-increment @ref cc3501e::recover_count, @p user is whatever was
 *  registered. Runs after cc3501e_recover() has already committed. */
typedef void (*cc3501e_recover_cb_t)(cc3501e_t *ctx, uint32_t recover_count, void *user);

/** Maximum simultaneous async-event subscribers per context (issue #1723).
 *  Sized for the in-tree consumers -- the Zephyr console companion plus an
 *  application -- with headroom; @ref cc3501e_add_event_callback reports
 *  @ref ALP_ERR_NOMEM rather than silently dropping one past this. */
#define CC3501E_EVENT_SUBSCRIBERS 4

/** Number of @ref cc3501e_link_log_entry_t slots in @ref cc3501e::link_log
 *  (issue #2136). A fixed ring, oldest entry overwritten first -- see
 *  @ref cc3501e_link_log_count / @ref cc3501e_link_log_get. */
#define CC3501E_LINK_LOG_LEN 8u

/** @ref cc3501e_link_log_entry_t::phase values -- which of
 *  cc3501e_request_locked()'s four wire phases an attempt reached before
 *  bailing. There is deliberately no fifth "verdict" value: this ring is
 *  filled ONLY on !decoded (see @ref cc3501e_link_log_entry_t), and reaching
 *  the verdict means a status WAS decoded, so that phase can never actually
 *  appear in a recorded entry -- review (#2136): a `PHASE_VERDICT` constant
 *  used to exist for exactly that unreachable case; this repo does not ship
 *  speculative public surface, so it was deleted rather than documented as
 *  dead.  @{ */
#define CC3501E_LINK_LOG_PHASE_REQUEST_HEADER  1u
#define CC3501E_LINK_LOG_PHASE_REQUEST_PAYLOAD 2u
#define CC3501E_LINK_LOG_PHASE_REPLY_HEADER    3u
#define CC3501E_LINK_LOG_PHASE_REPLY_PAYLOAD   4u
/** @} */

/** @ref cc3501e_link_log_entry_t::flags bits (issue #2136).  @{ */
#define CC3501E_LINK_LOG_READY_BEFORE \
	0x1u                                  /**< READY level sampled before the request header.
                                             *   READY (CC35 GPIO17 -> Alif P2_6) is an OPEN
                                             *   CONNECTION on some boards (see cc3501e_core.c's
                                             *   in-band-armed-check comment), so a caller reading
                                             *   this or READY_AFTER MUST also check READY_PROVEN:
                                             *   a frozen level on a board that never proved the
                                             *   line wired is not evidence of anything, just an
                                             *   unpopulated read. */
#define CC3501E_LINK_LOG_READY_AFTER 0x2u /**< READY level sampled at this attempt's exit. */
#define CC3501E_LINK_LOG_READY_PROVEN \
	0x4u /**< The line has been observed HIGH at least once
                                             *   this boot (cc3501e_core.c's g_ready_line_proven) --
                                             *   without this, BEFORE/AFTER are not trustworthy. */
/** #2136 review (MAJOR): a phase's byte array is memcpy'd from
 *  ctx->rx_scratch regardless of whether THAT phase's own transceive
 *  actually succeeded -- on a bail, that scratch can be stale residue from a
 *  DIFFERENT, earlier exchange (the poisoned-marker byte 0xDA plus leftover
 *  neighbours), presented as if it were wire data from THIS attempt. These
 *  two bits are the ONLY way to tell "genuinely read off the wire this
 *  attempt" apart from "zeroed because that phase's transceive never
 *  completed" -- see @ref cc3501e_link_log_entry_t::hdr_bytes /
 *  ::reply_hdr. A caller classifying deaf-armed vs desynced vs crashed from
 *  those byte patterns MUST check the matching VALID bit first; an unset bit
 *  means the byte array is all-zero and means nothing. */
#define CC3501E_LINK_LOG_HDR_BYTES_VALID \
	0x8u /**< hdr_bytes came off the wire this attempt
                                                 *   (the request-header transceive returned
                                                 *   ALP_OK) -- unset means hdr_bytes is all-zero. */
#define CC3501E_LINK_LOG_REPLY_HDR_VALID \
	0x10u /**< reply_hdr came off the wire this attempt
                                                 *   (the reply-header transceive returned
                                                 *   ALP_OK) -- unset means reply_hdr is all-zero,
                                                 *   whether because this attempt never reached
                                                 *   phase 3 or because phase 3's own transceive
                                                 *   itself failed. */
/** @} */

/**
 * @brief One link-failure ring entry (issue #2136).
 *
 * Filled ONLY at cc3501e_request_locked()'s `out:` label, ONLY when no reply
 * status was decoded (see @ref ALP_CC3501E_RX_SCRATCH_NO_STATUS) -- a genuine
 * pre-decode transport/framing failure, never a decoded device error and
 * never a success. This is the evidence a bridge wedge leaves on the HOST
 * side specifically because the firmware side does not survive the warm
 * nRESET used to recover it (cc3501e-bridge-firmware#148 measured 0
 * retained-RAM state across that reset -- see cc3501e_core.c's corrected
 * comment above cc3501e_recover()).
 *
 * 20 bytes, no padding -- enforced below by a `static_assert` right after
 * the type, so a future field addition that drifts this fails the BUILD,
 * not a silent ABI change.
 */
typedef struct {
	uint32_t ts_ms;                 /**< Truncated alp_uptime_ms() (wraps ~49 days --
	                                      *   acceptable for an 8-entry recent-history ring). */
	uint32_t recover_attempt_count; /**< @ref cc3501e::recover_attempt_count at record time. */
	uint8_t  cmd;                   /**< @c alp_cc3501e_cmd_t opcode this attempt used. */
	uint8_t  phase;                 /**< One of the @c CC3501E_LINK_LOG_PHASE_* values. */
	int8_t   status;                /**< @c alp_status_t narrowed (the whole enum fits -15..0). */
	uint8_t  flags;                 /**< @c CC3501E_LINK_LOG_READY_* / @c
	                                      *   CC3501E_LINK_LOG_*_VALID bits, OR'd. */
	uint8_t  hdr_bytes[4];          /**< Request-header phase's 4 MISO bytes (the in-band
	                                      *   armed-check marker: ALP_CC3501E_SYNC_IDLE x4 = armed) --
	                                      *   MEANINGLESS, all-zero, unless @c flags has
	                                      *   @ref CC3501E_LINK_LOG_HDR_BYTES_VALID set (#2136
	                                      *   review: previously memcpy'd from ctx->rx_scratch
	                                      *   regardless of whether the transceive that was
	                                      *   supposed to fill it actually succeeded, so a bail
	                                      *   before this phase ran could present a PRIOR
	                                      *   attempt's stale scratch as if it were this
	                                      *   attempt's wire data). */
	uint8_t  reply_hdr[4];          /**< Reply-header phase's 4 bytes (echoed opcode + length) --
	                                      *   MEANINGLESS, all-zero, unless @c flags has
	                                      *   @ref CC3501E_LINK_LOG_REPLY_HDR_VALID set: unset
	                                      *   covers BOTH "never reached phase 3" and "phase 3's
	                                      *   own transceive itself failed" -- see that macro's
	                                      *   doc comment. */
} cc3501e_link_log_entry_t;

#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
static_assert(sizeof(cc3501e_link_log_entry_t) == 20u,
              "cc3501e_link_log_entry_t drifted from 20 bytes");
#endif

/** Value cc3501e_request_locked() (cc3501e_core.c) writes into
 *  @ref cc3501e::rx_scratch's byte 0 on every PRE-DECODE exit -- a failed
 *  transceive, the in-band armed-check reject, a bad reply header -- so
 *  that byte means "a status byte was actually decoded" whenever it equals
 *  a real @c ALP_CC3501E_RESP_ERR_* code, never a coincidence of leftover
 *  wire residue (#2035: 0x06 is ALSO @ref ALP_CC3501E_CMD_GET_CAPABILITIES,
 *  so an un-poisoned residue byte could misread as a decoded RESP_ERR_RADIO).
 *  Public (not just an internal cc3501e_core.c constant) because a caller
 *  legitimately reading @ref cc3501e::rx_scratch directly for diagnostics
 *  (see examples/aen/aen-evk-demo/src/main.c's BLE_ENABLE failure probe)
 *  needs to recognise it too, rather than misreport it as real wire data.
 *  One of the eight documented RESERVED bit-flip neighbours of
 *  @ref ALP_CC3501E_RESP_OK in <alp/protocol/cc3501e.h> -- guaranteed never
 *  to become a real status code, unlike an arbitrary unused byte. */
#define ALP_CC3501E_RX_SCRATCH_NO_STATUS 0xDAu

struct cc3501e {
	bool initialised;
	/* Wire version the FIRMWARE reported at the last @ref cc3501e_reset
	 * (ADR 0033).  Both are 0 before the first successful GET_VERSION.
	 *
	 * v4.0 (#2035): the host is BILINGUAL, so fw_proto_major on a usable
	 * context is @ref ALP_CC3501E_PROTOCOL_MAJOR (4) OR
	 * @ref ALP_CC3501E_PROTOCOL_MAJOR_LEGACY (3) -- 3 refuses NOTHING; it is
	 * every bit as usable as 4, just talking the no-CRC 3.1 dialect (see the
	 * migration-order paragraph above ALP_CC3501E_PROTOCOL_MAJOR in
	 * <alp/protocol/cc3501e.h>).  A caller MUST branch on this field before
	 * relying on anything major-4-only (e.g. assuming a CRC trailer on every
	 * reply) -- do not assume it is pinned to PROTOCOL_MAJOR.  Only a value
	 * outside {3, 4} refuses the link (@ref ALP_ERR_VERSION).  These are also
	 * set on that REFUSAL path, so a caller that got ALP_ERR_VERSION can
	 * report what the firmware actually claimed; a fw_proto_major of 0 there
	 * means the firmware predates the scheme and answered with a raw v1..v9
	 * integer.
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
	 * rx_buf memcpy, and never read back across calls BY THE DRIVER ITSELF
	 * -- so they carry none of the #740 aliasing risk and needed no change
	 * here.
	 *
	 * #2035 follow-up: a caller MAY also read ctx->rx_scratch[] directly, as
	 * diagnostic evidence, immediately after a single-shot request-family
	 * call (cc3501e_ping(), cc3501e_request()) returns -- see the BLE_ENABLE
	 * failure probe in examples/aen/aen-evk-demo/src/main.c, which snapshots
	 * it right after cc3501e_ping() to classify the wire-level failure
	 * shape.  That is a legitimate, SEPARATE use of this field from the
	 * driver's own internal one above, with its own, narrower lifetime rule:
	 * valid only until the NEXT request-family call on this SAME ctx (the
	 * very next transceive overwrites it, and cc3501e_request_locked()'s
	 * out: label -- cc3501e_core.c -- always writes SOMETHING into byte 0
	 * specifically, a real decoded status or @ref ALP_CC3501E_RX_SCRATCH_NO_STATUS,
	 * before that call returns), and only meaningful for the call that just
	 * returned -- a caller that wants to compare two probes' raw bytes must
	 * snapshot this into its own buffer between them, not hold a pointer
	 * across a second call. */
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
	 * not just re-clocking a read.  cc3501e_sock_send() assigns it ONCE per FRAME
	 * (cc3501e-bridge-firmware#107: one chunk of a logical send -- one iteration
	 * of its remainder-retry loop, including that iteration's own bounded
	 * post-timeout collection grace), before each poll_by_repeat() call, so it
	 * stays constant across that frame's retries but changes for the next chunk's
	 * different remaining bytes; see the assignment site for why that constancy
	 * is what makes the fix work.
	 * uint8_t: wraps 255 -> 0 (defined unsigned overflow) after 256 increments,
	 * which cannot collide with the firmware's single-entry cache -- it only ever
	 * holds the immediately-preceding completed frame's seq, never one from 256
	 * increments back. */
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

	/* CMD_SOCK_RECV's OWN request-seq counter (alp-sdk#2108) -- deliberately
	 * NOT drawn from req_seq above, unlike every other worker-routed opcode.
	 *
	 * Bench evidence that motivated this: one run downloaded 262144 B over
	 * 4071 B cc3501e_sock_recv() calls, every call returned ALP_OK, and
	 * exactly one 4069 B block never arrived -- a GAP, not a duplicate --
	 * sitting exactly on a reply boundary. Root cause: the bridge firmware
	 * commits its receive-ring advance EAGERLY, before the host has actually
	 * received anything, so a reply that then fails CRC on the wire has
	 * ALREADY consumed that chunk firmware-side. Today's firmware cannot
	 * tell a retry of that exact request apart from a brand-new one, so
	 * poll_by_repeat()'s identical re-issue gets answered with the NEXT
	 * chunk instead of the lost one -- gone for good.
	 *
	 * The real fix is firmware-side and NOT YET MERGED
	 * (cc3501e-bridge-firmware, branch fix/sock-recv-retry-safe): the ring
	 * advance becomes LAZY, keyed on (header seq, socket handle) recorded on
	 * EVERY SOCK_RECV dispatch -- a request whose seq AND handle match the
	 * immediately preceding dispatch is a REPLAY (the same bytes, or a
	 * superset of them if more has since become readable), the ring does
	 * not advance again; any other request commits and serves the next
	 * chunk. That mechanism only works if the (seq, handle) key it reads is
	 * unambiguous -- this host-side counter is the groundwork that makes it
	 * so. It does not by itself close #2108's gap (that needs the firmware
	 * change to land); the one case it fixes directly today is an immediate
	 * retry of the SAME handle after cc3501e_sock_recv()'s OWN call fails --
	 * see that function's seq-commit comment in cc3501e_sockets.c.
	 *
	 * ONE counter per ctx, shared across every socket HANDLE, is sufficient
	 * -- not one per handle. The firmware records its (seq, handle) key on
	 * every SOCK_RECV dispatch regardless of handle, and compares only
	 * against that single most-recent entry, never against history. Because
	 * this counter advances by exactly one on every SOCK_RECV dispatch (any
	 * handle), two dispatches that are ADJACENT in the SOCK_RECV stream can
	 * never carry the same seq; the value can only repeat once a full
	 * ALP_CC3501E_REQ_SEQ_LAST-count cycle has passed, and by then the
	 * firmware's single entry has long since been overwritten by whatever
	 * else was dispatched in between -- an old numeric coincidence is
	 * harmless. E.g. recv(A), 30 recvs on handle B, recv(A) again: the
	 * second recv(A) numerically repeats the first's seq, but it is compared
	 * against B's last dispatch, not A's, so it is never mistaken for a
	 * replay of A's own first chunk. If SOCK_RECV instead drew from req_seq
	 * above (shared with EVERY OTHER worker-routed opcode, not just other
	 * recvs), that adjacency guarantee would not hold: unrelated traffic
	 * can land between two recvs without ever touching this dedicated
	 * counter, so two recvs that ARE adjacent in the firmware's
	 * SOCK_RECV-only view can still end up exactly 30 unrelated commands
	 * apart in req_seq's cycle and numerically identical -- exactly the
	 * #2108 bench failure.
	 *
	 * ADVANCE-ON-SUCCESS ONLY (alp-sdk#2108 review): cc3501e_sock_recv()
	 * computes this counter's NEXT value as a candidate for the wire but
	 * only commits it back into this field once its own call returns
	 * ALP_OK with the reply fully decoded -- never after ALP_ERR_TIMEOUT,
	 * ALP_ERR_IO, or a short reply header. See that function's own comment
	 * for the exact rule and its one documented limit: recovery holds only
	 * if the very next recv on the SAME handle reuses the kept seq; an
	 * intervening recv on a DIFFERENT handle correctly starts fresh instead,
	 * and the original lost chunk is not recovered.
	 *
	 * WRAP: 1..ALP_CC3501E_REQ_SEQ_LAST (31), skipping 0
	 * (ALP_CC3501E_REQ_SEQ_NONE) -- the same narrow 5-bit wire-header space
	 * as req_seq above, NOT the wider 8-bit (0..255, wraps through 0) space
	 * sock_send_seq and spi1_seq get: those two smuggle their seq inside an
	 * opcode-specific payload field with a whole spare byte to spend, where
	 * this field and req_seq both ride the frame header's shared 5-bit flags
	 * allocation (proto v8) instead. Pre-incremented, so a fresh ctx's
	 * first successful recv commits seq 1.
	 *
	 * Firmware without the replay logic (before cc3501e-bridge-firmware#138) ignores the
	 * seq bits for SOCK_RECV, exactly as it already does for every opcode
	 * that never checks them -- this counter is harmless against it. */
	uint8_t sock_recv_seq;

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

	/* Recovery bookkeeping (issue #2126), all maintained by cc3501e_recover()
	 * itself (<alp/chips/cc3501e/core.h>) so a manual `alp companion recover`
	 * and the automatic path (cc3501e_link_check_and_recover(), internal --
	 * see cc3501e_internal.h) share IDENTICAL state -- neither is a second,
	 * independently-maintained copy.
	 *   - recover_count: bumped on every SUCCESSFUL recovery. A legitimate
	 *     read for a caller's own telemetry (how many times THIS boot has
	 *     needed a warm-reset recovery) -- e.g. `alp companion recover`
	 *     (src/zephyr/console) prints it.
	 *   - recover_attempt_count / last_recover_ms / recover_fail_streak:
	 *     internal cooldown/back-off bookkeeping only, same category as
	 *     sock_send_seq above -- public because this whole struct is, not
	 *     because a caller should read them. recover_attempt_count is
	 *     bumped on EVERY attempt regardless of outcome (unlike
	 *     recover_count) -- the fix for a real bug: gating the cooldown on
	 *     recover_count alone let a caller stuck in a fail/retry loop
	 *     re-trigger a fresh warm reset on every single op, because a
	 *     FAILED attempt never advanced the old gate. recover_fail_streak
	 *     resets to 0 on success and drives the cooldown's exponential
	 *     back-off (CC3501E_RECOVER_COOLDOWN_MS doubling up to
	 *     CC3501E_RECOVER_COOLDOWN_MAX_MS, cc3501e_core.c) on repeated
	 *     failure. */
	uint32_t recover_count;
	uint32_t recover_attempt_count;
	uint64_t last_recover_ms;
	uint8_t  recover_fail_streak;
	/* Exclusive-ownership flag for cc3501e_recover()'s own reset sequence
	 * (issue #2126) -- claimed via compiler-builtin CAS (same portability
	 * shape as request_lock above), so two concurrent callers (an
	 * auto-trigger racing a manual `alp companion recover`, or two
	 * auto-triggers off two different failing ops) cannot double-pulse
	 * nRESET. The loser gets ALP_ERR_BUSY immediately, no reset attempted.
	 * Never touch directly -- go through cc3501e_recover(). */
	bool recovering;
	/* Bumped by cc3501e_recover() on every SUCCESSFUL recovery (issue
	 * #2126). The device reboots on recovery, so a raw firmware handle
	 * minted before this counter last changed (a socket from
	 * cc3501e_sock_open(), an accepted connection's handle from the async
	 * event queue) may now refer to nothing, or -- worse -- to a
	 * DIFFERENT, unrelated socket the post-reboot firmware happens to
	 * allocate the same numeric id to. cc3501e_sock_send() etc.
	 * (cc3501e_sockets.c) encode this into the upper byte of every handle
	 * they hand back and refuse (ALP_ERR_NOT_READY) a handle whose epoch
	 * byte no longer matches. Wraps 255 -> 0 (defined unsigned overflow);
	 * a caller holding a handle across 256 recoveries is not something
	 * this scheme defends against, same accepted-residual shape as
	 * sock_send_seq's own wrap above. */
	uint8_t link_epoch;
	/* Registered via cc3501e_set_recover_callback() (issue #2126); invoked
	 * by cc3501e_recover() after a successful recovery has already
	 * committed (recover_count bumped, stale state cleared), for BOTH the
	 * automatic and manual paths. NULL = no callback registered (the
	 * default). Never touch directly -- go through
	 * cc3501e_set_recover_callback(). */
	cc3501e_recover_cb_t recover_cb;
	void                *recover_cb_user;
	/* True for the whole span an OTA/update-mode session is open (issue
	 * #2126) -- set BEFORE the first update-mode send in
	 * cc3501e_ota_update_mode(ctx, true, ...) (chips/cc3501e/cc3501e_ota.c),
	 * cleared once that same function confirms the device is genuinely
	 * back in normal mode (readback OR its own hard-reset fallback, both
	 * of which guarantee it), on a bail (cc3501e_ota_update()'s
	 * ota_update_bail(), which itself goes through update_mode(false)), on
	 * a confirmed FINISH, or on a confirmed PROMOTE. Deliberately NOT
	 * cc3501e_peer_is_polled() (cc3501e_internal.h): that flag is a
	 * transport-framing detail (edge- vs level-gate the READY line) that
	 * happens to correlate with a session but is not one -- a caller that
	 * flips it directly, or a driver bug that leaves it stale, must not be
	 * misread as "OTA in progress" or "OTA long over" respectively.
	 * cc3501e_recover() refuses outright (ALP_ERR_BUSY) while this is
	 * true, for both the automatic path and a manual `alp companion
	 * recover`. */
	bool ota_session_active;

	/* #2136 review (MAJOR): pre-decode failures LOGGED while a
	 * cc3501e_link_check_and_recover() probe is running (see
	 * link_log_suppress below), counted here instead -- up to
	 * CC3501E_LINK_PROBE_TRIES (24), so a uint8_t is plenty. Reset to 0 at
	 * the start of every probe. This is the "the probe ran and here is how
	 * it failed" evidence an operator still needs even though the probe's
	 * OWN PING failures must not evict the wedging op's ring entry -- see
	 * @ref cc3501e_link_log_probe_fail_count. Placed here (not after
	 * link_log_next below) purely to reuse this padding byte rather than
	 * grow the struct -- no relation to ota_session_active. */
	uint8_t link_log_probe_fail_count;
	/* #2136 review (minor): ctx->link_log_fail_streak's snapshot at the
	 * moment the MOST RECENT cc3501e_recover() call started (top of that
	 * function, before nRESET or the confirming PING) -- cc3501e_recover()'s
	 * own confirming PING decodes successfully and resets the live
	 * link_log_fail_streak to 0 via the normal decoded-reply path in
	 * cc3501e_request_locked(), so by the time companion_recover_notify()
	 * ran its dump the live field always read 0 regardless of how the link
	 * actually got here. Saturates at 0xFF (plenty -- a real streak this
	 * long would already have tripped the probe/recover cycle many times
	 * over). See @ref cc3501e_link_log_recover_streak. Placed here for the
	 * same padding-reuse reason as link_log_probe_fail_count above. */
	uint8_t link_log_recover_streak;

	/* Link-failure ring (issue #2136) -- see @ref cc3501e_link_log_entry_t
	 * for the fill rule and @ref cc3501e_link_log_get for the read side.
	 * 8 * 20 = 160 B, plus this bookkeeping. Never touch link_log /
	 * link_log_count / link_log_next / link_log_fail_streak directly --
	 * go through the accessors, which also take @ref request_lock (the
	 * SAME lock the sole writer, cc3501e_request_locked()'s `out:` label,
	 * runs under) so a reader never sees a torn entry mid-write. */
	cc3501e_link_log_entry_t link_log[CC3501E_LINK_LOG_LEN];
	uint8_t link_log_count; /**< Entries held, saturates at CC3501E_LINK_LOG_LEN. */
	uint8_t link_log_next;  /**< Ring write cursor -- also the oldest entry's
	                                          *   slot once the ring is full. */
	/* #2136 review (MAJOR): true for the span cc3501e_link_check_and_recover()'s
	 * own probe loop is running -- cc3501e_request_locked()'s `out:` label
	 * checks this and, when set, counts the failure into
	 * link_log_probe_fail_count above INSTEAD of writing a ring entry. The
	 * probe fires up to CC3501E_LINK_PROBE_TRIES (24) PINGs, more than
	 * CC3501E_LINK_LOG_LEN (8), so without this the probe's own PINGs
	 * guarantee-evict the ring entry an operator actually needs -- the
	 * wedging op that triggered recovery in the first place. Placed here
	 * (not beside ota_session_active) to reuse THIS padding gap instead of
	 * growing the struct. */
	bool link_log_suppress;
	/* Consecutive pre-decode failures (issue #2136): bumped on every ring
	 * write, reset to 0 on the next DECODED reply (success or device-side
	 * error alike) -- so it answers "how many attempts in a row have left
	 * no evidence at all", distinct from @ref recover_fail_streak above
	 * (which only counts failed RECOVERY attempts, not ordinary requests).
	 * NOT bumped while link_log_suppress is set (see link_log_probe_fail_count
	 * instead) and NOT bumped by a request that lost cc3501e_lock_acquire()
	 * (ALP_ERR_BUSY after the CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS
	 * window, without ever entering cc3501e_request_locked()) -- during a
	 * wedge that is the common shape for every thread except the lock
	 * holder, so a thin ring / low streak during heavy contention does NOT
	 * mean "few failures", just "few that got the lock". */
	uint32_t link_log_fail_streak;
	/* Last successful GET_DIAG_INFO probe (issue #2136), for correlating the
	 * ring against firmware-side telemetry: a wedge-probe firmware build
	 * rides its own free-running word in the GET_DIAG_INFO reply's
	 * free_heap_bytes field (see cc3501e_diag_info(), chips/cc3501e/
	 * cc3501e_diag.c) instead of a real heap count. */
	uint32_t link_log_last_probe_word;
	uint32_t link_log_last_probe_ms; /**< Truncated alp_uptime_ms() at that probe. */
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
 * then fail (ALP_ERR_IO) indefinitely. It does not self-heal on its own --
 * this call, or the automatic path built on it (@ref
 * cc3501e_wifi_connect and every worker-routed op wired to it internally,
 * issue #2126), is what clears it.
 *
 * Firmware-side diagnostics taken across the fault (see #1691) show the slave
 * armed and idle in its request-header phase with READY HIGH, its housekeeping
 * task still running, and its resync / arm-failure counters at zero -- i.e. the
 * firmware has no way to know anything is wrong. Only the host, which is getting
 * no answers, can tell. Hence this call.
 *
 * Issues a warm reset (nRESET only, supply left up), confirms the link with a
 * PING, and re-fetches the protocol version -- all three under ONE hold of the
 * internal transport lock (issue #2126), so a concurrent caller on this same
 * @p ctx (another thread's request, the console's async-event poll) gets
 * @ref ALP_ERR_BUSY instead of clocking a byte into the module mid-reboot,
 * which would desync the CS-less link permanently. Falls back to a full
 * supply cycle if the warm reset does not take. On success the link is
 * usable again -- but the device rebooted, so the Wi-Fi association, the BLE
 * host and every open socket are gone and must be re-established; a stale
 * socket handle from before this call now reads back @ref ALP_ERR_NOT_READY
 * (@ref cc3501e::link_epoch, cc3501e_sockets.c) rather than silently
 * addressing whatever new socket now happens to share its number.
 *
 * At most one recovery runs at a time per @p ctx: a concurrent call (auto
 * racing manual, or two auto-triggers) loses a compare-and-swap and returns
 * @ref ALP_ERR_BUSY immediately, with no reset attempted. Register @ref
 * cc3501e_set_recover_callback to be notified after a successful recovery.
 *
 * @warning NEVER call this with an OTA in flight -- resetting mid-update destroys
 *          the partially staged image, and #1610 traced its hangs to exactly
 *          this. Refused outright: returns @ref ALP_ERR_BUSY while @ref
 *          cc3501e::ota_session_active (cc3501e_ota.c) is true.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK when the link answers again; ALP_ERR_INVAL if @p ctx is NULL;
 *         ALP_ERR_BUSY if an OTA session is active or another recovery is
 *         already running on this @p ctx; otherwise the mapped error from
 *         the reset or the confirming PING.
 */
alp_status_t cc3501e_recover(cc3501e_t *ctx);

/**
 * @brief Register a callback invoked after every SUCCESSFUL recovery.
 *
 * Runs after @ref cc3501e_recover has already committed -- recover_count
 * bumped, stale same-ctx state cleared -- for BOTH the automatic path and a
 * manual `alp companion recover`. One slot per @p ctx; a second call
 * overwrites the first. Pass a NULL @p cb to unregister.
 *
 * @note On Zephyr, `alp_console_companion_set()` registers its own callback
 *       (it prints `cc3501e: link recovered by warm reset (#n)`). Register
 *       yours AFTER binding the console, or the console's registration
 *       replaces it.
 *
 * @param ctx   Initialised bridge handle.
 * @param cb    Callback to invoke, or NULL to unregister.
 * @param user  Opaque pointer handed back unchanged as @p cb's last argument.
 */
void cc3501e_set_recover_callback(cc3501e_t *ctx, cc3501e_recover_cb_t cb, void *user);

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
 * @note **Issue #2035 -- the first-radio-op wedge.**  Roughly 2 in 16 cold boots,
 * the FIRST worker-routed radio opcode of a boot (a Wi-Fi scan, a BLE enable, a
 * connect -- not @ref cc3501e_ping / @ref cc3501e_get_version / diag, which are
 * not worker-routed and succeed regardless) times out (@ref ALP_ERR_TIMEOUT),
 * and the link then reads @ref ALP_ERR_IO until a cold cycle.  The bridge
 * firmware deliberately does NOT fix this in-band -- every candidate firmware
 * move has a demonstrated wedge/brick precedent, recorded in
 * cc3501e-bridge-firmware's `prebuilt/CHANGELOG.md` -- so the sanctioned
 * response is host-side: if the FIRST radio op of a boot fails with
 * @ref ALP_ERR_TIMEOUT, call this function ONCE and retry that same op ONCE.
 * That takes 2 in 16 to roughly 1 in 128. A second failure is a real failure
 * and must be surfaced, not retried again; a LATER op failing the same way is
 * not this condition and must not be papered over the same way.
 *
 * DELIBERATELY @ref ALP_ERR_TIMEOUT ONLY -- never @ref ALP_ERR_IO, even though
 * a caller who remembers the spoken "-4 or -5" form of this condition will be
 * tempted to widen the trigger.  poll_by_repeat() (cc3501e_core.c) treats a
 * bare ALP_ERR_IO as retryable and loops it to the deadline, so a wedged link
 * cannot surface as -5 out of a worker-routed call -- it surfaces as -4 once
 * the budget elapses.  The only way -5 emerges from that loop is its
 * `terminal_decoded_io` check: a well-framed, CRC-valid reply the device
 * itself decoded as RESP_ERR_RADIO / RESP_ERR_PROTOCOL / RESP_ERR_INTERNAL.
 * The link is ALIVE in that case -- it answered, just with a real fault --
 * so hard-resetting it on a -5 would destroy a genuine RF/firmware
 * diagnostic instead of recovering a wedge.
 *
 * This function itself is still app-level policy, not driver behaviour -- do
 * NOT fold a bare call to it into any op function in this driver, because a
 * caller mid-association or mid-BLE-link must not silently lose that state to
 * a reset it never asked for.  It only pulses the line and blind-settles; it
 * does not confirm the link itself (no PING, no version check), so a caller
 * using it for the #2035 one-shot-retry recipe above must treat the RETRIED
 * OP as what proves recovery, not this call's own return value.
 *
 * @note **Issue #2126 update.** A DIFFERENT, broader mechanism now IS folded
 * into every worker-routed op and into @ref cc3501e_wifi_connect -- when a
 * top-level op comes back @ref ALP_ERR_TIMEOUT (or @ref ALP_ERR_IO) with NO
 * reply EVER decoded off the wire for that op -- the same shape a #2035
 * first-radio-op wedge produces, among others -- the driver's internal
 * cc3501e_link_check_and_recover() (wired into those ops' own
 * failure exits) probes the link and, only if every probe fails, calls @ref
 * cc3501e_recover() below, which does a superset of this function's job (this
 * pulse, THEN a confirming PING, THEN a protocol-version re-fetch, all
 * automatic). That does not change this function's own contract -- it
 * remains a raw, uninvoked-by-the-driver primitive an application may still
 * call by hand for the #2035 recipe above -- but an application relying on
 * that recipe purely to recover from a wedge (not to distinguish a
 * first-boot-specific fault) now gets the same recovery automatically and
 * should consider retiring its own hand-rolled version (see
 * examples/aen/aen-cc3501e-companion-tour's own retired copy).
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
 * only meaningful if nothing else moves the bus underneath it.  A direct
 * cc3501e_request() call issued concurrently gets @ref ALP_ERR_BUSY from its
 * own bounded acquire, which is the honest answer: the link is by
 * definition unusable until the re-sync completes.  A poll_by_repeat()
 * caller (alp-sdk#2035) only gets that same @ref ALP_ERR_BUSY on its very
 * FIRST lock attempt; past that, it instead waits out the sync within its
 * own deadline (retrying the lock acquire like any other retryable BUSY),
 * so it can block up to its own timeout_ms plus one more lock wait before
 * giving up.
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
 * @param len         Bytes to clock, 0..(@c ALP_CC3501E_SPI1_MAX_XFER minus
 *                    @c ALP_CC3501E_CRC_BYTES once this @p ctx has negotiated
 *                    the MAJOR-4 wire -- cc3501e_request()'s own tx_len
 *                    ceiling already enforces the tighter bound once the CRC
 *                    trailer it appends is accounted for; chunk at the
 *                    max_xfer the peer reported, see above, not at the bare
 *                    macro).
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

/**
 * @brief Number of link-failure ring entries currently held (issue #2136).
 *
 * @par Concurrency (#2136 review)
 * Takes @ref cc3501e::request_lock for the duration of the read -- the SAME
 * lock the sole writer (cc3501e_request_locked()'s `out:` label) holds --
 * so this never races a write mid-entry. Bounded by the same
 * @c CONFIG_ALP_SDK_CC3501E_REQUEST_LOCK_TIMEOUT_MS as an ordinary request;
 * on contention this reports 0 rather than blocking past that budget or
 * returning a torn count.
 *
 * @param ctx  Initialised bridge handle.
 * @return 0..CC3501E_LINK_LOG_LEN; 0 on a NULL @p ctx or a lock timeout.
 */
uint8_t cc3501e_link_log_count(const cc3501e_t *ctx);

/**
 * @brief Read one link-failure ring entry, oldest-first (issue #2136).
 *
 * @par Concurrency (#2136 review)
 * Same locking discipline as @ref cc3501e_link_log_count -- see its doc
 * comment. A lock timeout here reports @ref ALP_ERR_BUSY, same as an
 * ordinary request under contention.
 *
 * @param ctx  Initialised bridge handle.
 * @param idx  0 = oldest entry currently held .. cc3501e_link_log_count(ctx) - 1 = newest.
 * @param out  Receives a copy of the entry on success.
 * @return ALP_OK; ALP_ERR_INVAL if @p ctx or @p out is NULL, or @p idx is
 *         out of range for the entry count currently held; ALP_ERR_BUSY on
 *         a lock timeout.
 */
alp_status_t cc3501e_link_log_get(const cc3501e_t *ctx, uint8_t idx, cc3501e_link_log_entry_t *out);

/**
 * @brief Live consecutive-pre-decode-failure streak (issue #2136).
 *
 * The current value of @ref cc3501e::link_log_fail_streak, read under the
 * same lock as @ref cc3501e_link_log_count (#2136 review: direct field
 * reads of this counter are exactly the discipline @ref cc3501e::link_log
 * documents against for its neighbours -- this accessor is the fix).
 *
 * @param ctx  Initialised bridge handle.
 * @return The live streak; 0 on a NULL @p ctx or a lock timeout.
 */
uint32_t cc3501e_link_log_fail_streak(const cc3501e_t *ctx);

/**
 * @brief The fail streak as it stood when the MOST RECENT cc3501e_recover()
 *        call started (issue #2136 review).
 *
 * Frozen BEFORE that recovery's own confirming PING decoded and reset the
 * live streak (@ref cc3501e_link_log_fail_streak) to 0 -- use this, not the
 * live accessor, when reporting what a just-finished recovery actually
 * found, e.g. in an `alp companion recover` / auto-recovery dump.
 *
 * @param ctx  Initialised bridge handle.
 * @return The snapshot (saturates at 0xFF); 0 on a NULL @p ctx or if no
 *         recovery has run yet on this @p ctx.
 */
uint8_t cc3501e_link_log_recover_streak(const cc3501e_t *ctx);

/**
 * @brief Pre-decode failures the most recent auto-recovery probe absorbed
 *        WITHOUT writing a ring entry (issue #2136 review).
 *
 * cc3501e_link_check_and_recover()'s own up-to-@c CC3501E_LINK_PROBE_TRIES
 * (24) PINGs suppress ring writes so they cannot evict the wedging op's own
 * evidence (see @ref cc3501e::link_log_suppress) -- this is the "the probe
 * ran and here is how many of its own PINGs failed" count an operator still
 * needs, reset at the start of every probe.
 *
 * @param ctx  Initialised bridge handle.
 * @return The count (0..24); 0 on a NULL @p ctx or if no probe has run yet.
 */
uint8_t cc3501e_link_log_probe_fail_count(const cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_CORE_H */
