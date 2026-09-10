/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-command-sweep -- exercises the CC3501E's full command surface
 * and measures inter-chip link throughput, for the E1M-AEN801 (Alif
 * Ensemble E8, M55-HE), bench RAM-run via J-Link.
 *
 * WHY THIS APP EXISTS
 * --------------------
 * Its sibling aen-cc3501e-handshake-probe (read that app's own file header
 * first -- it documents the bench this one shares and the failure mode this
 * app's own bar exists to avoid) discriminated ONE PING failure into a named
 * hypothesis. That app's first version printed a confident "not booting"
 * verdict about a part that was DEMONSTRABLY answering, because it only
 * looked at the opcodes it happened to call and never said what it did not
 * try. This app's job is broader -- every opcode the protocol defines -- so
 * that same failure mode scales up: a silent omission here would not be one
 * wrong verdict, it would be a whole command family nobody ever finds out
 * was untested. The rule this app is built around: every
 * ALP_CC3501E_CMD_* opcode is either INVOKED (with its return code
 * classified) or explicitly SKIPPED (with a printed reason) -- never
 * silently absent. See g_all_cmd_opcodes[] and sweep_self_check() below,
 * which is what makes that rule a runtime-checked fact instead of a
 * comment nobody re-verifies when a future opcode gets added.
 *
 * SAFETY BOUNDARIES (non-negotiable, see each STEP below for the same note
 * inline at its call site):
 *   - OTA_BEGIN/WRITE/FINISH/PROMOTE/UPDATE_MODE are NEVER invoked -- these
 *     write the coprocessor's flash and can leave it unbootable. OTA_STATUS
 *     and OTA_ABORT are read-only and ARE invoked.
 *   - RESET is NEVER invoked mid-sweep -- it reboots the bridge and would
 *     invalidate every result after it.
 *   - WIFI_CONNECT_STA and WIFI_AP_START are NEVER invoked -- this app has
 *     no credentials and must not embed any SSID/passphrase.
 *   - CAM_ENABLE is invoked (it is just an LDO-enable pin, not a flash
 *     write), but this bench has no camera fitted -- see its own STEP for
 *     how the outcome is classified either way.
 *
 * ORDER
 * -----
 * Bring-up first (STEP 1), then META -- both needed just to prove the link
 * is alive at all before anything else runs. PART 2 (THROUGHPUT:
 * STREAM_WRITE + SPI1_TRANSFER, swept across sizes) runs IMMEDIATELY after
 * that, while the link is known good. Only then does the rest of PART 1's
 * opcode COVERAGE sweep run: WIFI 0x1x, SOCKETS 0x2x, BLE 0x3x, OTA 0x4x,
 * STREAM_WRITE 0x45 coverage, GPIO 0x5x, SPI1 0x55-0x57 coverage,
 * CAMERA/POWER 0x6x, DIAGNOSTICS 0x7x last, so its frame counters tally the
 * WHOLE sweep -- including Part 2's own throughput traffic, since Part 2 now
 * runs BEFORE it. Where a family has an enable/disable pair the disable
 * half runs before the next family starts, so no family inherits
 * radio/session state a sibling left armed -- e.g. BLE_GATT_REGISTER runs
 * BEFORE BLE_ADV_START (NimBLE refuses a register while advertising) and
 * BLE_DISABLE is the family's last call.
 *
 * WHY THROUGHPUT RUNS FIRST (bench finding, #2035)
 * --------------------------------------------------
 * Throughput is this app's headline deliverable. Coverage's own job,
 * though, is to deliberately provoke refusals -- SOCK_CONNECT at an
 * unroutable test address, BLE_CONNECT at a dummy peer, WIFI_GET_RSSI while
 * unassociated -- and each of those refusals spends a real timeout finding
 * out what everyone already expects. On the actual bench run this app was
 * built from, one such timeout (SOCK_CONNECT, 3.05 s against 192.0.2.1)
 * left the link answering a mapped error to EVERY opcode after it,
 * including SOCK_CLOSE, which had itself returned ALP_OK 20 ms earlier --
 * not a coincidence: aen-evk-demo independently hit the identical shape
 * behind a 10.6 s GET_MAC. A long or timed-out operation, not a failing
 * one, is what precedes this link's wedge in both cases. Running coverage
 * first would let its own deliberately-provoked refusals wedge the link
 * before Part 2 ever collected a clean sample; running throughput first,
 * right after META proves the link live, means the headline numbers do not
 * depend on coverage having gone smoothly.
 *
 * WEDGE DETECTION
 * -----------------
 * This app does not try to recover from a wedge -- no mid-sweep bridge
 * reset, because that would change what is being measured and the failure
 * shape is worth capturing intact. Instead sweep_report()'s wedge tracker
 * (PART 0 below) counts consecutive genuine FAIL verdicts -- never REFUSED,
 * since coverage provokes those on purpose and they are not a link symptom.
 * At SWEEP_WEDGE_FAIL_THRESHOLD in a row it prints ONE line naming the last
 * opcode that succeeded and the elapsed time of the operation immediately
 * before the first failure in the streak -- that operation, not the
 * failures after it, is the diagnostic signal, per the bench finding above
 * -- then tags every later coverage line "[post-wedge]" so a reader never
 * mistakes a wedge's downstream consequence for an independent result.
 * Coverage still keeps going and still accounts for every opcode either
 * way -- see the self-check in PART 3.
 *
 * Like its sibling, this app NEVER stops early on a failed step -- a failed
 * step's own return code IS the data this app exists to collect.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he -- see this
 * app's README.md for the full west build invocation and the overlay's own
 * header for why the memory placement is load-bearing, not decorative.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/chips/cc3501e.h"
#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up template */

/*
 * ======================================================================
 * PART 0 -- shared reporting + coverage-tracking + wedge-detection plumbing
 * ======================================================================
 *
 * Every family below funnels its calls through sweep_report() (invoked) or
 * sweep_skip() (deliberately not invoked). Both mark the opcode covered in
 * g_covered[]; sweep_self_check() (called once, at the very end) walks
 * every symbol in g_all_cmd_opcodes[] -- enumerated from
 * <alp/protocol/cc3501e.h> by NAME, never hand-copied as a bare hex value,
 * so a future opcode this file forgets to add still shows up as covered ==
 * false rather than silently passing -- and prints a loud failure line for
 * anything neither path touched. That is what turns "every opcode must be
 * accounted for" from a promise into something this app checks on itself.
 *
 * sweep_report() ALSO drives the wedge tracker described in the file header
 * (see WEDGE DETECTION): it is the one place every INVOKED opcode's verdict
 * passes through, so it is the one place that can notice a run of genuine
 * FAILs forming.
 */

/* Opcodes run 0x00..0x71 (<alp/protocol/cc3501e.h>'s CMD_* range); 128 bits
 * covers the whole byte, including the reserved-vendor gap above 0x80 this
 * app never touches. */
#define SWEEP_OPCODE_BITS 128u
static uint8_t g_covered[SWEEP_OPCODE_BITS / 8u];

static void sweep_mark(alp_cc3501e_cmd_t op)
{
	unsigned v = (unsigned)op;
	if (v < SWEEP_OPCODE_BITS) {
		g_covered[v / 8u] |= (uint8_t)(1u << (v % 8u));
	}
}

static bool sweep_is_covered(alp_cc3501e_cmd_t op)
{
	unsigned v = (unsigned)op;
	return (v < SWEEP_OPCODE_BITS) && ((g_covered[v / 8u] & (uint8_t)(1u << (v % 8u))) != 0u);
}

/* How many consecutive genuine FAIL verdicts (never REFUSED -- see the file
 * header) this app treats as "the link is probably wedged, not just having a
 * bad opcode". 3 is the smallest run that distinguishes a real streak from a
 * single unlucky result while still tripping promptly -- the bench capture
 * this detector is modelled on showed SOCK_CLOSE / BLE_ENABLE / STREAM_WRITE
 * fail back to back, i.e. exactly 3, right after the timeout that wedged it. */
#define SWEEP_WEDGE_FAIL_THRESHOLD 3u

/* "Last opcode that returned OK" and "what ran immediately before this
 * call", tracked across every sweep_report()/sweep_skip() call regardless of
 * family -- a wedge does not respect family boundaries, so neither does this
 * bookkeeping. g_wedge_prev_op_* is a SNAPSHOT taken the moment the first
 * FAIL of a new streak is seen, of whatever g_prev_op_* held at that instant
 * -- i.e. the operation that ran right before the wedge, which is the
 * diagnostic signal per the file header, not the failures that follow it. */
static const char *g_last_ok_name             = "(none yet)";
static const char *g_prev_op_name             = "(none yet)";
static int64_t     g_prev_op_elapsed_ms       = 0;
static const char *g_wedge_prev_op_name       = "";
static int64_t     g_wedge_prev_op_elapsed_ms = 0;
static unsigned    g_consec_fails             = 0u;
static bool        g_wedge_suspected          = false;

/** Update the wedge tracker with one INVOKED opcode's outcome and return the
 *  prefix tag its own report line should print -- "[post-wedge] " if a wedge
 *  was ALREADY suspected before this call (never for the call that itself
 *  trips the threshold: that call is presented as an ordinary FAIL, and the
 *  banner below it explains why everything AFTER it is suspect). */
static const char *sweep_wedge_track(const char *name, int64_t elapsed_ms, const char *verdict)
{
	const char *tag = g_wedge_suspected ? "[post-wedge] " : "";

	if (strcmp(verdict, "FAIL") == 0) {
		if (g_consec_fails == 0u) {
			/* First FAIL of a new streak -- snapshot what ran right before
			 * it now, before g_prev_op_* below gets overwritten by THIS
			 * call. */
			g_wedge_prev_op_name       = g_prev_op_name;
			g_wedge_prev_op_elapsed_ms = g_prev_op_elapsed_ms;
		}
		g_consec_fails++;
		if (!g_wedge_suspected && g_consec_fails >= SWEEP_WEDGE_FAIL_THRESHOLD) {
			g_wedge_suspected = true;
			printk("\n  ** SUSPECTED LINK WEDGE: %u consecutive genuine FAILs. Last opcode"
			       " that succeeded was %s. The operation immediately before the FIRST"
			       " failure in this streak was %s, which itself took %lld ms -- that"
			       " operation, not the failures after it, is the diagnostic signal (see"
			       " this app's README). Every result from here on is tagged"
			       " [post-wedge] and is a CONSEQUENCE of this, not an independent"
			       " measurement. Coverage keeps going regardless. **\n\n",
			       g_consec_fails,
			       g_last_ok_name,
			       g_wedge_prev_op_name,
			       (long long)g_wedge_prev_op_elapsed_ms);
		}
	} else {
		g_consec_fails = 0u;
		if (strcmp(verdict, "OK") == 0) {
			g_last_ok_name = name;
		}
	}

	g_prev_op_name       = name;
	g_prev_op_elapsed_ms = elapsed_ms;
	return tag;
}

/** Print one INVOKED opcode's result. @p elapsed_ms is the wall time the
 *  actual driver call took (k_uptime_get() deltas, NOT k_cycle_get_32() --
 *  see the file header's own note on why). @p verdict is a short
 *  caller-chosen label -- "OK", "REFUSED" (an expected refusal given this
 *  bench's actual hardware/credentials), or "FAIL" (anything else) --
 *  decided by the call site from the ACTUAL @p rc against what that
 *  specific opcode can legitimately return here, never inferred
 *  generically: a -2 (ALP_ERR_NOT_READY) means something different on
 *  WIFI_GET_RSSI (not associated -- expected) than it would on PING (link
 *  not initialised -- a real fault), so only the call site that knows which
 *  opcode it is can tell the two apart. */
static void sweep_report(alp_cc3501e_cmd_t op,
                         const char       *name,
                         alp_status_t      rc,
                         int64_t           elapsed_ms,
                         const char       *verdict,
                         const char       *note)
{
	sweep_mark(op);
	const char *wedge_tag = sweep_wedge_track(name, elapsed_ms, verdict);
	printk("  0x%02X %-19s rc=%-3d  elapsed_ms=%-5lld  %-7s  %s%s\n",
	       (unsigned)op,
	       name,
	       (int)rc,
	       (long long)elapsed_ms,
	       verdict,
	       wedge_tag,
	       note);
}

/** Print one deliberately-SKIPPED opcode + why. Not timed -- nothing was
 *  invoked, so there is no call duration to report. Still updates the
 *  "previous operation" the wedge tracker snapshots (as a zero-duration,
 *  never-a-FAIL entry), and still carries the [post-wedge] tag once a wedge
 *  is suspected, purely so a reader scanning the log sees the tag
 *  consistently on every line after the trip, skipped or not. */
static void sweep_skip(alp_cc3501e_cmd_t op, const char *name, const char *reason)
{
	sweep_mark(op);
	const char *tag = g_wedge_suspected ? "[post-wedge] " : "";
	printk("  0x%02X %-19s SKIPPED   %s%s\n", (unsigned)op, name, tag, reason);
	g_prev_op_name       = name;
	g_prev_op_elapsed_ms = 0;
}

/** Every ALP_CC3501E_CMD_* opcode -- by symbol, not by hex literal, so this
 *  table cannot silently drift from <alp/protocol/cc3501e.h>: a future
 *  opcode the header adds fails to compile into this array only if its
 *  symbol is missing, and a renamed/removed one fails the build outright
 *  rather than leaving a stale hex value behind. Async EVT_* opcodes are
 *  NOT here -- they are slave -> master only, drained via
 *  GET_PENDING_EVENTS (0x05, which IS in this table), never invoked by
 *  opcode themselves, so "accounting for" them means draining the queue,
 *  which STEP 2 below does. */
typedef struct {
	alp_cc3501e_cmd_t opcode;
	const char       *name;
} sweep_opcode_t;

static const sweep_opcode_t g_all_cmd_opcodes[] = {
	/* Meta 0x00-0x06 */
	{ ALP_CC3501E_CMD_PING, "PING" },
	{ ALP_CC3501E_CMD_GET_VERSION, "GET_VERSION" },
	{ ALP_CC3501E_CMD_RESET, "RESET" },
	{ ALP_CC3501E_CMD_GET_MAC, "GET_MAC" },
	{ ALP_CC3501E_CMD_GET_DIAG_INFO, "GET_DIAG_INFO" },
	{ ALP_CC3501E_CMD_GET_PENDING_EVENTS, "GET_PENDING_EVENTS" },
	{ ALP_CC3501E_CMD_GET_CAPABILITIES, "GET_CAPABILITIES" },
	/* Wi-Fi 0x10-0x1B (CMD_* only; EVT_WIFI_* 0x18-0x1A are async) */
	{ ALP_CC3501E_CMD_WIFI_SCAN_START, "WIFI_SCAN_START" },
	{ ALP_CC3501E_CMD_WIFI_SCAN_STOP, "WIFI_SCAN_STOP" },
	{ ALP_CC3501E_CMD_WIFI_CONNECT_STA, "WIFI_CONNECT_STA" },
	{ ALP_CC3501E_CMD_WIFI_DISCONNECT, "WIFI_DISCONNECT" },
	{ ALP_CC3501E_CMD_WIFI_AP_START, "WIFI_AP_START" },
	{ ALP_CC3501E_CMD_WIFI_AP_STOP, "WIFI_AP_STOP" },
	{ ALP_CC3501E_CMD_WIFI_GET_RSSI, "WIFI_GET_RSSI" },
	{ ALP_CC3501E_CMD_WIFI_GET_IP, "WIFI_GET_IP" },
	{ ALP_CC3501E_CMD_WIFI_STATUS, "WIFI_STATUS" },
	/* Sockets 0x20-0x26 */
	{ ALP_CC3501E_CMD_SOCK_OPEN, "SOCK_OPEN" },
	{ ALP_CC3501E_CMD_SOCK_CONNECT, "SOCK_CONNECT" },
	{ ALP_CC3501E_CMD_SOCK_SEND, "SOCK_SEND" },
	{ ALP_CC3501E_CMD_SOCK_RECV, "SOCK_RECV" },
	{ ALP_CC3501E_CMD_SOCK_CLOSE, "SOCK_CLOSE" },
	{ ALP_CC3501E_CMD_SOCK_BIND, "SOCK_BIND" },
	{ ALP_CC3501E_CMD_SOCK_LISTEN, "SOCK_LISTEN" },
	/* BLE 0x30-0x3B (CMD_* only; EVT_BLE_* 0x3C-0x3F are async) */
	{ ALP_CC3501E_CMD_BLE_ENABLE, "BLE_ENABLE" },
	{ ALP_CC3501E_CMD_BLE_DISABLE, "BLE_DISABLE" },
	{ ALP_CC3501E_CMD_BLE_ADV_START, "BLE_ADV_START" },
	{ ALP_CC3501E_CMD_BLE_ADV_STOP, "BLE_ADV_STOP" },
	{ ALP_CC3501E_CMD_BLE_SCAN_START, "BLE_SCAN_START" },
	{ ALP_CC3501E_CMD_BLE_SCAN_STOP, "BLE_SCAN_STOP" },
	{ ALP_CC3501E_CMD_BLE_CONNECT, "BLE_CONNECT" },
	{ ALP_CC3501E_CMD_BLE_DISCONNECT, "BLE_DISCONNECT" },
	{ ALP_CC3501E_CMD_BLE_GATT_REGISTER, "BLE_GATT_REGISTER" },
	{ ALP_CC3501E_CMD_BLE_GATT_NOTIFY, "BLE_GATT_NOTIFY" },
	{ ALP_CC3501E_CMD_BLE_GATT_READ, "BLE_GATT_READ" },
	{ ALP_CC3501E_CMD_BLE_GATT_WRITE, "BLE_GATT_WRITE" },
	/* OTA 0x40-0x47 (BEGIN/WRITE/FINISH/PROMOTE/UPDATE_MODE deliberately
	 * skipped -- see the safety-boundary note at the top of this file) */
	{ ALP_CC3501E_CMD_OTA_BEGIN, "OTA_BEGIN" },
	{ ALP_CC3501E_CMD_OTA_WRITE, "OTA_WRITE" },
	{ ALP_CC3501E_CMD_OTA_FINISH, "OTA_FINISH" },
	{ ALP_CC3501E_CMD_OTA_ABORT, "OTA_ABORT" },
	{ ALP_CC3501E_CMD_OTA_STATUS, "OTA_STATUS" },
	{ ALP_CC3501E_CMD_OTA_PROMOTE, "OTA_PROMOTE" },
	{ ALP_CC3501E_CMD_OTA_UPDATE_MODE, "OTA_UPDATE_MODE" },
	/* Stream 0x45 */
	{ ALP_CC3501E_CMD_STREAM_WRITE, "STREAM_WRITE" },
	/* GPIO proxy 0x50-0x53 (EVT_GPIO_INTERRUPT 0x54 is async) */
	{ ALP_CC3501E_CMD_GPIO_CONFIGURE, "GPIO_CONFIGURE" },
	{ ALP_CC3501E_CMD_GPIO_WRITE, "GPIO_WRITE" },
	{ ALP_CC3501E_CMD_GPIO_READ, "GPIO_READ" },
	{ ALP_CC3501E_CMD_GPIO_SET_INTERRUPT, "GPIO_SET_INTERRUPT" },
	/* SPI1 host passthrough 0x55-0x57 */
	{ ALP_CC3501E_CMD_SPI1_CONFIGURE, "SPI1_CONFIGURE" },
	{ ALP_CC3501E_CMD_SPI1_TRANSFER, "SPI1_TRANSFER" },
	{ ALP_CC3501E_CMD_SPI1_RELEASE, "SPI1_RELEASE" },
	/* Camera + power 0x60-0x62 */
	{ ALP_CC3501E_CMD_CAM_ENABLE, "CAM_ENABLE" },
	{ ALP_CC3501E_CMD_CAM_DISABLE, "CAM_DISABLE" },
	{ ALP_CC3501E_CMD_POWER_POLICY, "POWER_POLICY" },
	/* Diagnostics 0x70-0x71 */
	{ ALP_CC3501E_CMD_DIAG_GET_STATS, "DIAG_GET_STATS" },
	{ ALP_CC3501E_CMD_DIAG_LOG_LEVEL, "DIAG_LOG_LEVEL" },
};
#define SWEEP_NUM_CMD_OPCODES (sizeof(g_all_cmd_opcodes) / sizeof(g_all_cmd_opcodes[0]))

/** Walk every known opcode and fail loudly if any was neither invoked nor
 *  explicitly skipped -- the runtime enforcement of this app's one rule.
 *  Returns the number of opcodes found un-accounted-for (0 = clean). */
static unsigned sweep_self_check(void)
{
	unsigned missing = 0;
	printk("\n=== SELF-CHECK: every opcode either invoked or explicitly skipped? ===\n");
	for (size_t i = 0; i < SWEEP_NUM_CMD_OPCODES; i++) {
		if (!sweep_is_covered(g_all_cmd_opcodes[i].opcode)) {
			printk("  MISSING: 0x%02X %s was NEVER reported -- this app has a real gap,"
			       " not just an unlucky bench run\n",
			       (unsigned)g_all_cmd_opcodes[i].opcode,
			       g_all_cmd_opcodes[i].name);
			missing++;
		}
	}
	printk("  %u/%u opcodes accounted for%s\n",
	       (unsigned)(SWEEP_NUM_CMD_OPCODES - missing),
	       (unsigned)SWEEP_NUM_CMD_OPCODES,
	       (missing == 0u) ? " -- clean" : " -- SEE MISSING LINES ABOVE");
	return missing;
}

/*
 * ======================================================================
 * PART 0b -- throughput measurement plumbing (shared by STREAM_WRITE and
 * SPI1_TRANSFER in Part 2)
 * ======================================================================
 */

/** How many times each size is repeated -- "a single sample on this link is
 *  not a measurement". Small enough to keep total bench time bounded across
 *  every size this app sweeps, large enough that a min/max spread means
 *  something. */
#define SWEEP_THROUGHPUT_REPEATS 5u

typedef struct {
	uint32_t     size;             /**< Frame payload size this result is for, bytes. */
	unsigned     ok_count;         /**< How many of SWEEP_THROUGHPUT_REPEATS succeeded. */
	uint32_t     bytes_total;      /**< Sum of @c size over every SUCCESSFUL repeat. */
	int64_t      elapsed_total_ms; /**< Sum of k_uptime_get() deltas over successful repeats. */
	int64_t      elapsed_min_ms;
	int64_t      elapsed_max_ms;
	alp_status_t first_rc; /**< First repeat's own rc -- what Part 1's coverage line cites. */
} throughput_result_t;

/** Print bytes / elapsed / rate as three SEPARATE figures (k_uptime_get() is
 *  milliseconds, so a reader can check bytes_total * 1000 / elapsed_total_ms
 *  by hand), plus the min/max spread across the repeats -- never a single
 *  number standing in for a measurement. Fixed-point KB/s (no float): this
 *  app's printk build does not depend on CONFIG_CBPRINTF_FP_SUPPORT. */
static void throughput_print(const char *label, const throughput_result_t *r)
{
	if (r->ok_count == 0u) {
		printk("  %-14s size=%5u B: 0/%u transfers succeeded -- no rate derived (rc=%d "
		       "on the first attempt)\n",
		       label,
		       r->size,
		       SWEEP_THROUGHPUT_REPEATS,
		       (int)r->first_rc);
		return;
	}
	uint32_t rate_bps = 0u;
	if (r->elapsed_total_ms > 0) {
		rate_bps = (uint32_t)(((uint64_t)r->bytes_total * 1000u) / (uint64_t)r->elapsed_total_ms);
	}
	printk("  %-14s size=%5u B: %u/%u ok  bytes=%u  elapsed_ms=%lld  "
	       "(min=%lld max=%lld per-repeat)  rate=%u B/s (%u.%02u KB/s)%s\n",
	       label,
	       r->size,
	       r->ok_count,
	       SWEEP_THROUGHPUT_REPEATS,
	       r->bytes_total,
	       (long long)r->elapsed_total_ms,
	       (long long)r->elapsed_min_ms,
	       (long long)r->elapsed_max_ms,
	       rate_bps,
	       rate_bps / 1024u,
	       (rate_bps % 1024u) * 100u / 1024u,
	       (r->elapsed_total_ms == 0)
	           ? "  <-- elapsed measured 0 ms in total: below k_uptime_get()'s 1 ms tick,"
	             " rate is not meaningful at this size"
	           : "");
}

/*
 * Bulk payload buffer -- STATIC, not a local. A 4 KB buffer on the stack is
 * exactly the mistake main()'s own cc3501e_t discipline below exists to
 * avoid (see that comment); sharing one static buffer between the
 * STREAM_WRITE and SPI1_TRANSFER sweeps is safe because this app runs them
 * sequentially on a single thread, never concurrently.
 */
static uint8_t g_bulk_buf[ALP_CC3501E_MAX_PAYLOAD];

/** CRC-16/CCITT-FALSE headroom a MAJOR-4 peer's frames reserve (see
 *  ALP_CC3501E_CRC_BYTES's own doc in <alp/protocol/cc3501e.h>): once
 *  @p fw_major negotiates the v4.0 wire, every request's usable payload
 *  ceiling shrinks by the 2-byte trailer the transport appends underneath
 *  the caller. cc3501e_stream_write() already applies this bound itself
 *  (its own ceiling, MAX_PAYLOAD - HEADER_BYTES, is tighter than
 *  cc3501e_request()'s CRC-adjusted one either way), but
 *  cc3501e_spi1_transfer()'s own ALP_CC3501E_SPI1_MAX_XFER ceiling is NOT
 *  CRC-adjusted by the wrapper -- see its own @p len doc -- so this app
 *  computes the MAJOR-4 headroom itself for that sweep rather than risk an
 *  oversized chunk silently failing ALP_ERR_INVAL one layer down. */
static uint16_t sweep_crc_headroom(uint8_t fw_major)
{
	return (fw_major >= (uint8_t)ALP_CC3501E_PROTOCOL_MAJOR) ? (uint16_t)ALP_CC3501E_CRC_BYTES : 0u;
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	printk("\n=== AEN801 CC3501E command sweep + throughput ===\n");

	/*
	 * STATIC, explicitly zero-initialised cc3501e_t -- same discipline as
	 * aen-cc3501e-handshake-probe's main(), for the identical reason: this
	 * type embeds several ALP_CC3501E_MAX_PAYLOAD scratch buffers (~32 KB
	 * total). Declared automatic it asks main() for a 32908-byte stack
	 * frame, and Zephyr's stack-overflow check fires on the very first
	 * `sub sp` -- before this function's own first printk, so not even the
	 * header line escapes. Measured on the M55-HE 2026-09-10 by that
	 * sibling app; this one inherits the same fix rather than
	 * rediscovering the same failure.
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up, byte-identical to aen-evk-demo phase 8
	 * and aen-cc3501e-handshake-probe (same src/cc3501e_bridge.{c,h}
	 * template, same overlay). This also runs cc3501e_reset()'s own
	 * GET_VERSION, which is what fw.fw_proto_major reflects below.
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);
	if (rc != ALP_OK) {
		/* Never stop early (see the file header) -- but say up front that
		 * every opcode below inherits this one root cause, so a reader does
		 * not have to re-derive that from 55 identical ALP_ERR_NOT_READY
		 * lines. */
		printk("  ** bring-up did NOT return ALP_OK: every call below will fail"
		       " ALP_ERR_NOT_READY as a DIRECT consequence of THIS line, not of"
		       " its own opcode. Read this line as the root cause. **\n");
	}
	printk("STEP 1: fw_proto_major=%u fw_proto_minor=%u (as reset() left it)\n",
	       (unsigned)fw.fw_proto_major,
	       (unsigned)fw.fw_proto_minor);

	/*
	 * =================================================================
	 * PART 1 -- command coverage, one family per opcode range, ascending.
	 * META runs first, just below, to prove the link is alive. The
	 * remaining families (WIFI through DIAGNOSTICS) continue AFTER Part 2
	 * -- see ORDER and WHY THROUGHPUT RUNS FIRST in the file header for
	 * why Part 2's throughput sweep is interleaved here, between META and
	 * WIFI, instead of running after every Part 1 family the way its own
	 * numbering alone would suggest.
	 * =================================================================
	 */

	/* ---- META (0x00-0x06) --------------------------------------------- */
	printk("\n--- META (0x00-0x06) ---\n");
	{
		/* RESET (0x02): NEVER invoked mid-sweep -- it reboots the bridge and
		 * would invalidate every result after it (see the file header). */
		sweep_skip(ALP_CC3501E_CMD_RESET,
		           "RESET",
		           "deliberately never invoked mid-sweep -- reboots the bridge and would"
		           " invalidate every result after it; cc3501e_reset()/cc3501e_hard_reset()"
		           " already exercised the equivalent GPIO-level reset during STEP 1's"
		           " bring-up");

		int64_t      t0 = k_uptime_get();
		alp_status_t r  = cc3501e_ping(&fw);
		sweep_report(ALP_CC3501E_CMD_PING,
		             "PING",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");

		uint16_t version = 0u;
		t0               = k_uptime_get();
		r                = cc3501e_get_version(&fw, &version);
		sweep_report(ALP_CC3501E_CMD_GET_VERSION,
		             "GET_VERSION",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             (r == ALP_OK) ? ""
		                           : "a bare liveness round-trip -- see STEP 1's own major/minor");

		uint8_t mac[CC3501E_MAC_LEN] = { 0 };
		t0                           = k_uptime_get();
		r                            = cc3501e_wifi_get_mac(&fw, mac, 3000u);
		sweep_report(ALP_CC3501E_CMD_GET_MAC,
		             "GET_MAC",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");

		alp_cc3501e_diag_info_t diag = { 0 };
		t0                           = k_uptime_get();
		r                            = cc3501e_diag_info(&fw, &diag);
		sweep_report(ALP_CC3501E_CMD_GET_DIAG_INFO,
		             "GET_DIAG_INFO",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");

		/* GET_PENDING_EVENTS drains the queue rather than invoking a
		 * callback path -- this app registers no subscriber, so ALP_OK
		 * with an empty queue is the expected shape (see
		 * cc3501e_poll_events()'s own doc: "no-op returning ALP_OK when no
		 * callback is registered"). */
		t0 = k_uptime_get();
		r  = cc3501e_poll_events(&fw);
		sweep_report(ALP_CC3501E_CMD_GET_PENDING_EVENTS,
		             "GET_PENDING_EVENTS",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "no subscriber registered -- drains + discards");

		uint32_t caps = 0u;
		t0            = k_uptime_get();
		r             = cc3501e_get_capabilities(&fw, &caps);
		sweep_report(ALP_CC3501E_CMD_GET_CAPABILITIES,
		             "GET_CAPABILITIES",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");
		if (r == ALP_OK) {
			printk("  caps=0x%08X (WIFI_STA=%d WIFI_AP=%d SOCK_CLIENT=%d SOCK_LISTEN=%d"
			       " BLE=%d OTA=%d GPIO_PROXY=%d SPI1_MASTER=%d CAMERA=%d POWER_POLICY=%d"
			       " DIAG_STATS=%d EVENTS=%d)\n",
			       (unsigned)caps,
			       (caps & ALP_CC3501E_CAP_WIFI_STA) != 0u,
			       (caps & ALP_CC3501E_CAP_WIFI_AP) != 0u,
			       (caps & ALP_CC3501E_CAP_SOCK_CLIENT) != 0u,
			       (caps & ALP_CC3501E_CAP_SOCK_LISTEN) != 0u,
			       (caps & ALP_CC3501E_CAP_BLE) != 0u,
			       (caps & ALP_CC3501E_CAP_OTA) != 0u,
			       (caps & ALP_CC3501E_CAP_GPIO_PROXY) != 0u,
			       (caps & ALP_CC3501E_CAP_SPI1_MASTER) != 0u,
			       (caps & ALP_CC3501E_CAP_CAMERA) != 0u,
			       (caps & ALP_CC3501E_CAP_POWER_POLICY) != 0u,
			       (caps & ALP_CC3501E_CAP_DIAG_STATS) != 0u,
			       (caps & ALP_CC3501E_CAP_EVENTS) != 0u);
		}
	}

	/*
	 * =================================================================
	 * PART 2 -- throughput: STREAM_WRITE and SPI1_TRANSFER, swept across
	 * frame sizes. Runs HERE, right after META and deliberately BEFORE the
	 * rest of Part 1's coverage families -- see WHY THROUGHPUT RUNS FIRST
	 * in the file header. SOCK_SEND/SOCK_RECV are NOT measured here -- see
	 * the note printed below for why.
	 * =================================================================
	 */
	printk("\n=== PART 2: THROUGHPUT ===\n");

	/* 64 / 256 / 1024 are the sizes the task asks for; the fourth entry
	 * per path is that path's own WIRE CEILING (computed, not guessed):
	 * STREAM_WRITE's is cc3501e_stream_write()'s own bound
	 * (ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES); SPI1_TRANSFER's
	 * is ALP_CC3501E_SPI1_MAX_XFER minus the MAJOR-4 CRC headroom (see
	 * sweep_crc_headroom() above) -- they differ because the two opcodes
	 * define their own ceilings differently, not because of an error here.
	 */
	static const uint32_t k_stream_sizes_fixed[3] = { 64u, 256u, 1024u };
	const uint32_t stream_ceiling = (uint32_t)ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES;
	const uint32_t spi1_ceiling =
	    (uint32_t)ALP_CC3501E_SPI1_MAX_XFER - sweep_crc_headroom(fw.fw_proto_major);

	printk("\n--- STREAM_WRITE throughput (0x45) ---\n");
	memset(g_bulk_buf, 0x5A, sizeof(g_bulk_buf));
	for (size_t i = 0u; i < ARRAY_SIZE(k_stream_sizes_fixed) + 1u; i++) {
		uint32_t size =
		    (i < ARRAY_SIZE(k_stream_sizes_fixed)) ? k_stream_sizes_fixed[i] : stream_ceiling;
		throughput_result_t r = { .size = size, .first_rc = ALP_ERR_NOT_READY };
		for (unsigned rep = 0u; rep < SWEEP_THROUGHPUT_REPEATS; rep++) {
			int64_t      t0  = k_uptime_get();
			alp_status_t src = cc3501e_stream_write(&fw, g_bulk_buf, size);
			int64_t      dt  = k_uptime_get() - t0;
			if (rep == 0u) r.first_rc = src;
			if (src == ALP_OK) {
				r.ok_count++;
				r.bytes_total += size;
				r.elapsed_total_ms += dt;
				r.elapsed_min_ms =
				    (r.ok_count == 1u) ? dt : ((dt < r.elapsed_min_ms) ? dt : r.elapsed_min_ms);
				r.elapsed_max_ms = (dt > r.elapsed_max_ms) ? dt : r.elapsed_max_ms;
			}
			/* STREAM_WRITE is ACKed synchronously by the firmware sink --
			 * no retry loop of this app's own is in play here, so this
			 * gap is bench courtesy between repeats, not the
			 * >250 ms-per-retry desync-avoidance rule the file header's
			 * safety notes describe for a RETRY cadence. */
			k_msleep(5);
		}
		throughput_print("STREAM_WRITE", &r);
	}

	printk("\n--- SPI1_TRANSFER throughput (0x56) ---\n");
	{
		uint32_t     actual_freq = 0u;
		uint16_t     peer_max    = 0u;
		alp_status_t cfg         = cc3501e_spi1_configure(
		    &fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, &actual_freq, &peer_max, 3000u);
		if (cfg != ALP_OK) {
			printk("  SPI1_CONFIGURE -> %d -- cannot sweep SPI1_TRANSFER without a"
			       " configured controller; skipping the size sweep (already accounted"
			       " for as a coverage FAIL/REFUSAL below)\n",
			       (int)cfg);
		} else {
			/* Chunk at whichever is SMALLER: this app's own MAJOR-4-adjusted
			 * ceiling, or the PEER firmware's own max_xfer -- see
			 * cc3501e_spi1_transfer()'s own @param len doc: "chunk at the
			 * max_xfer the peer reported ... not at the bare macro". */
			uint32_t sweep_spi1_ceiling = (peer_max > 0u && (uint32_t)peer_max < spi1_ceiling)
			                                  ? (uint32_t)peer_max
			                                  : spi1_ceiling;
			static const uint32_t k_spi1_sizes_fixed[3] = { 64u, 256u, 1024u };
			memset(g_bulk_buf, 0xC3, sizeof(g_bulk_buf));
			for (size_t i = 0u; i < ARRAY_SIZE(k_spi1_sizes_fixed) + 1u; i++) {
				uint32_t size = (i < ARRAY_SIZE(k_spi1_sizes_fixed)) ? k_spi1_sizes_fixed[i]
				                                                     : sweep_spi1_ceiling;
				if (size > sweep_spi1_ceiling)
					size = sweep_spi1_ceiling; /* clamp the 1024 entry too */
				throughput_result_t r = { .size = size, .first_rc = ALP_ERR_NOT_READY };
				for (unsigned rep = 0u; rep < SWEEP_THROUGHPUT_REPEATS; rep++) {
					int64_t      t0  = k_uptime_get();
					alp_status_t src = cc3501e_spi1_transfer(
					    &fw, g_bulk_buf, NULL, (uint16_t)size, 0xFFu, false, 3000u);
					int64_t dt = k_uptime_get() - t0;
					if (rep == 0u) r.first_rc = src;
					if (src == ALP_OK) {
						r.ok_count++;
						r.bytes_total += size;
						r.elapsed_total_ms += dt;
						r.elapsed_min_ms = (r.ok_count == 1u)
						                       ? dt
						                       : ((dt < r.elapsed_min_ms) ? dt : r.elapsed_min_ms);
						r.elapsed_max_ms = (dt > r.elapsed_max_ms) ? dt : r.elapsed_max_ms;
					}
					/* Worker-routed + poll-by-repeat internally (see
					 * cc3501e_spi1_transfer()'s own @warning on retry
					 * semantics) -- this gap is the SAME bench courtesy as
					 * the STREAM_WRITE loop above, not a desync-avoidance
					 * requirement of this app's own. */
					k_msleep(5);
				}
				throughput_print("SPI1_TRANSFER", &r);
			}
			(void)cc3501e_spi1_release(&fw, 2000u); /* free the bus -- not itself re-reported */
		}
	}

	printk("\n--- SOCK_SEND / SOCK_RECV throughput: NOT MEASURED ---\n");
	printk("  No Wi-Fi association exists on this run (WIFI_CONNECT_STA / WIFI_AP_START"
	       " are deliberately never invoked -- no credentials), so the firmware has no"
	       " route to a real peer. Part 1's SOCKETS coverage below will show"
	       " SOCK_CONNECT/SOCK_SEND/SOCK_RECV refused for exactly that reason. A"
	       " throughput number over a connection that was never established would not"
	       " be a measurement of anything -- see the file header's Part 2 note.\n");

	/*
	 * =================================================================
	 * PART 1 (continued) -- the remaining coverage families, WIFI through
	 * DIAGNOSTICS, in the same ascending-opcode order the file header
	 * describes. This is the half of Part 1 that deliberately provokes
	 * refusals (unassociated Wi-Fi, an unroutable SOCK_CONNECT target, a
	 * dummy BLE peer) -- exactly the timeouts WHY THROUGHPUT RUNS FIRST
	 * describes, which is why Part 2 above already ran.
	 * =================================================================
	 */

	/* ---- WI-FI (0x10-0x1B) --------------------------------------------- */
	printk("\n--- WI-FI (0x10-0x1B) ---\n");
	{
		cc3501e_scan_record_t scan_records[4];
		size_t                scan_count = 0u;
		int64_t               t0         = k_uptime_get();
		alp_status_t          r =
		    cc3501e_wifi_scan(&fw, scan_records, ARRAY_SIZE(scan_records), &scan_count, 15000u);
		sweep_report(ALP_CC3501E_CMD_WIFI_SCAN_START,
		             "WIFI_SCAN_START",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");
		if (r == ALP_OK) {
			printk("  found %u AP(s) (capped at %u)\n",
			       (unsigned)scan_count,
			       (unsigned)ARRAY_SIZE(scan_records));
		}

		/* By the time WIFI_SCAN_START's own poll-by-repeat returns, the
		 * firmware's scan has already completed -- there is nothing left
		 * in flight for STOP to abort. Invoked anyway for coverage; ALP_OK
		 * on an already-idle scanner is the expected shape. */
		t0 = k_uptime_get();
		r  = cc3501e_wifi_scan_stop(&fw);
		sweep_report(ALP_CC3501E_CMD_WIFI_SCAN_STOP,
		             "WIFI_SCAN_STOP",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "scan already completed by the time this runs -- expected no-op");

		/* WIFI_CONNECT_STA / WIFI_AP_START: NEVER invoked -- see the
		 * safety-boundary note at the top of this file. No SSID or
		 * passphrase is embedded anywhere in this app. */
		sweep_skip(ALP_CC3501E_CMD_WIFI_CONNECT_STA,
		           "WIFI_CONNECT_STA",
		           "deliberately never invoked -- needs real AP credentials this app does"
		           " not have and must not embed");
		sweep_skip(ALP_CC3501E_CMD_WIFI_AP_START,
		           "WIFI_AP_START",
		           "deliberately never invoked -- changes radio state (starts a soft-AP +"
		           " DHCP server) and needs credentials this app does not have");

		/* Not associated (never connected), so DISCONNECT is a no-op the
		 * firmware still acknowledges. */
		t0 = k_uptime_get();
		r  = cc3501e_wifi_disconnect(&fw);
		sweep_report(ALP_CC3501E_CMD_WIFI_DISCONNECT,
		             "WIFI_DISCONNECT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "no STA association exists -- expected no-op");

		/* No soft-AP was ever started -- per cc3501e_wifi_ap_stop()'s own
		 * doc, THIS is the one case it documents as conclusively ALP_OK. */
		t0 = k_uptime_get();
		r  = cc3501e_wifi_ap_stop(&fw);
		sweep_report(ALP_CC3501E_CMD_WIFI_AP_STOP,
		             "WIFI_AP_STOP",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "no soft-AP was ever started -- per this call's own doc that is the ONE"
		             " conclusive case (ALP_OK); anything else here is a genuine surprise,"
		             " not an expected refusal");

		int8_t rssi = 0;
		t0          = k_uptime_get();
		r           = cc3501e_wifi_rssi(&fw, &rssi);
		sweep_report(ALP_CC3501E_CMD_WIFI_GET_RSSI,
		             "WIFI_GET_RSSI",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             (r == ALP_ERR_NOT_READY) ? "expected -- not associated to any AP" : "");

		uint8_t ip[4] = { 0 };
		t0            = k_uptime_get();
		r             = cc3501e_wifi_get_ip(&fw, (uint8_t)ALP_CC3501E_WIFI_IFACE_STA, ip);
		sweep_report(ALP_CC3501E_CMD_WIFI_GET_IP,
		             "WIFI_GET_IP",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             (r == ALP_ERR_NOT_READY) ? "expected -- no DHCP lease, never associated" : "");

		alp_cc3501e_wifi_status_t status = { 0 };
		t0                               = k_uptime_get();
		r                                = cc3501e_wifi_status(&fw, &status);
		sweep_report(ALP_CC3501E_CMD_WIFI_STATUS,
		             "WIFI_STATUS",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             (r == ALP_OK) ? "state should read DISCONNECTED (0) -- never connected" : "");
	}

	/* ---- SOCKETS (0x20-0x26) --------------------------------------------
	 * No Wi-Fi association exists (see above -- deliberately, no
	 * credentials), so the firmware IP stack has no route to any real
	 * peer. Every opcode below is still invoked for coverage; CONNECT
	 * targets 192.0.2.1 (RFC 5737 TEST-NET-1, a documentation-only address
	 * that is never reachable, not a real destination) purely so the
	 * opcode goes out on the wire -- no traffic can actually reach
	 * anywhere. This is also why Part 2 above cannot measure SOCK_SEND/
	 * SOCK_RECV throughput -- see that section. */
	printk("\n--- SOCKETS (0x20-0x26) ---\n");
	{
		/* #2035 bench finding: at a 2000-3000 ms timeout, SOCK_CONNECT's
		 * refusal against an unroutable test address burns real wall time
		 * (measured 3.05 s against 192.0.2.1) proving what everyone already
		 * knows it will refuse -- and THAT spend is what wedged this link
		 * on the bench (see WHY THROUGHPUT RUNS FIRST in the file header).
		 * Every opcode on this link completes within 60 ms when the link is
		 * healthy (also bench-measured); 100 ms is the shortest timeout
		 * that still gives poll_by_repeat's worker-routed retry loop a full
		 * round trip's margin above that, so the opcode is still genuinely
		 * exercised -- this is not shortened to the point of proving
		 * nothing, just to the point of not paying for a multi-second
		 * budget every OTHER opcode's generous timeout affords it. */
		const uint32_t k_sock_connect_timeout_ms = 100u;

		uint16_t     srv = 0u;
		int64_t      t0  = k_uptime_get();
		alp_status_t r   = cc3501e_sock_open(
		    &fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &srv, 3000u);
		sweep_report(ALP_CC3501E_CMD_SOCK_OPEN,
		             "SOCK_OPEN",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             (r == ALP_ERR_NOT_READY) ? "expected on a build/state with no IP stack up"
		                                      : "opens a server-side listening socket");

		t0 = k_uptime_get();
		r  = cc3501e_sock_bind(&fw, srv, NULL, 8080u, 3000u);
		sweep_report(ALP_CC3501E_CMD_SOCK_BIND,
		             "SOCK_BIND",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             (r == ALP_ERR_NOT_READY) ? "expected -- no interface exists to bind on" : "");

		t0 = k_uptime_get();
		r  = cc3501e_sock_listen(&fw, srv, 4u, 3000u);
		sweep_report(ALP_CC3501E_CMD_SOCK_LISTEN,
		             "SOCK_LISTEN",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             (r != ALP_OK) ? "expected -- depends on SOCK_BIND above, which itself"
		                             " depends on an IP stack this bench has no interface for"
		                           : "");

		t0 = k_uptime_get();
		r  = cc3501e_sock_close(&fw, srv, 3000u);
		sweep_report(ALP_CC3501E_CMD_SOCK_CLOSE,
		             "SOCK_CLOSE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "closes the listening socket opened above");

		uint16_t cli             = 0u;
		t0                       = k_uptime_get();
		alp_status_t open_client = cc3501e_sock_open(
		    &fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &cli, 3000u);
		/* A second SOCK_OPEN for the client-role opcodes below -- its own
		 * result is folded into SOCK_CONNECT's report rather than printed
		 * twice, since SOCK_OPEN's opcode was already reported above and
		 * this call exercises no opcode SOCK_OPEN's first report did not
		 * already cover. Its own elapsed time is folded into SOCK_CONNECT's
		 * report for the same reason. */
		static const uint8_t k_test_peer_ip[4] = { 192, 0, 2, 1 }; /* RFC 5737 TEST-NET-1 */
		r = (open_client == ALP_OK)
		        ? cc3501e_sock_connect(&fw, cli, k_test_peer_ip, 9u, k_sock_connect_timeout_ms)
		        : open_client;
		sweep_report(ALP_CC3501E_CMD_SOCK_CONNECT,
		             "SOCK_CONNECT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "target is 192.0.2.1 (RFC 5737 TEST-NET-1, never routable) -- no Wi-Fi"
		             " association exists to route through regardless; a timeout/IO refusal"
		             " here is the expected shape, not a link fault. Timeout deliberately"
		             " short -- see k_sock_connect_timeout_ms above.");

		static const uint8_t k_send_bytes[4] = { 'p', 'i', 'n', 'g' };
		size_t               sent            = 0u;
		t0                                   = k_uptime_get();
		r = (open_client == ALP_OK)
		        ? cc3501e_sock_send(&fw, cli, k_send_bytes, sizeof(k_send_bytes), &sent, 2000u)
		        : open_client;
		sweep_report(ALP_CC3501E_CMD_SOCK_SEND,
		             "SOCK_SEND",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "expected refusal -- SOCK_CONNECT above did not establish a real peer");

		uint8_t recv_buf[64];
		size_t  recv_len = 0u;
		t0               = k_uptime_get();
		r = (open_client == ALP_OK)
		        ? cc3501e_sock_recv(&fw, cli, recv_buf, sizeof(recv_buf), &recv_len, 2000u)
		        : open_client;
		sweep_report(ALP_CC3501E_CMD_SOCK_RECV,
		             "SOCK_RECV",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "expected refusal/empty -- no connected peer to receive from");

		t0 = k_uptime_get();
		r  = cc3501e_sock_close(&fw, cli, 3000u);
		sweep_report(ALP_CC3501E_CMD_SOCK_CLOSE,
		             "SOCK_CLOSE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "closes the client socket");
	}

	/* ---- BLE (0x30-0x3B) --------------------------------------------- */
	printk("\n--- BLE (0x30-0x3B) ---\n");
	{
		int64_t      t0 = k_uptime_get();
		alp_status_t r  = cc3501e_ble_enable(&fw, 5000u);
		sweep_report(ALP_CC3501E_CMD_BLE_ENABLE,
		             "BLE_ENABLE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "");
		bool ble_up = (r == ALP_OK);

		cc3501e_ble_scan_record_t ble_records[4];
		size_t                    ble_count = 0u;
		t0                                  = k_uptime_get();
		r = ble_up ? cc3501e_ble_scan(&fw, ble_records, ARRAY_SIZE(ble_records), &ble_count, 8000u)
		           : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_SCAN_START,
		             "BLE_SCAN_START",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             ble_up ? "" : "BLE_ENABLE above did not succeed");

		t0 = k_uptime_get();
		r  = ble_up ? cc3501e_ble_scan_stop(&fw, 2000u) : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_SCAN_STOP,
		             "BLE_SCAN_STOP",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             ble_up ? "scan window already elapsed by the time this runs -- expected"
		                      " no-op"
		                    : "BLE_ENABLE above did not succeed");

		/* GATT_REGISTER MUST run before ADV_START (NimBLE refuses to
		 * re-run ble_gatts_start() while advertising -- see
		 * cc3501e_ble_gatt_register()'s own @note). One minimal service:
		 * a dummy 128-bit UUID, one READable characteristic, no initial
		 * value -- just enough for the descriptor to round-trip and prove
		 * the opcode, not a real GATT profile. */
		uint8_t desc[1u + 16u + 1u + 16u + 1u + 2u];
		size_t  off = 0u;
		desc[off++] = ALP_CC3501E_BLE_GATT_REGISTER_VERSION;
		memset(&desc[off], 0x00, 16u);
		desc[off + 15] = 0xA1; /* dummy service UUID, last byte only, so it is not all-zero */
		off += 16u;
		desc[off++] = 1u; /* num_chars */
		memset(&desc[off], 0x00, 16u);
		desc[off + 15] = 0xA2; /* dummy characteristic UUID */
		off += 16u;
		/* BLE_GATT_REGISTER properties byte -- BT-SIG / NimBLE
		 * BLE_GATT_CHR_F_* bits, per the wire-format doc block above
		 * ALP_CC3501E_CMD_BLE_GATT_REGISTER in <alp/protocol/cc3501e.h>
		 * (READ=0x02). Mirrored locally rather than pulling in the whole
		 * portable <alp/ble.h> surface for one constant, since this app
		 * calls the chips/cc3501e/ wrapper directly. */
		desc[off++] = 0x02u; /* READ */
		desc[off++] = 0u;    /* initial_len LE lo */
		desc[off++] = 0u;    /* initial_len LE hi -- no initial value */

		uint16_t char_handle = 0u;
		size_t   num_handles = 0u;
		t0                   = k_uptime_get();
		r = ble_up
		        ? cc3501e_ble_gatt_register(&fw, desc, off, &char_handle, 1u, &num_handles, 5000u)
		        : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_GATT_REGISTER,
		             "BLE_GATT_REGISTER",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)                                   ? "OK"
		             : (r == ALP_ERR_NOT_READY || r == ALP_ERR_BUSY) ? "REFUSED"
		                                                             : "FAIL",
		             ble_up ? "" : "BLE_ENABLE above did not succeed");

		static const uint8_t k_adv_name[] = { 'A', 'L', 'P' };
		t0                                = k_uptime_get();
		r                                 = ble_up ? cc3501e_ble_adv_start(
		                                                 &fw, false, 200u, 300u, k_adv_name, sizeof(k_adv_name), 3000u)
		                                           : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_ADV_START,
		             "BLE_ADV_START",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "non-connectable, short 3-byte payload -- proves the opcode without"
		             " inviting a real central to connect");
		bool advertising = (r == ALP_OK);

		/* NOTIFY/READ/WRITE all require an ACTIVE BLE connection (see each
		 * wrapper's own @param doc) -- this bench advertises but has no
		 * peer guaranteed to connect, so these are expected refusals
		 * regardless of whether GATT_REGISTER above found a real handle.
		 * handle 0 is used when registration did not hand back one; the
		 * opcode still goes out on the wire either way. */
		uint16_t use_handle = (num_handles > 0u) ? char_handle : 0u;

		static const uint8_t k_notify_val[1] = { 0x2A };
		t0                                   = k_uptime_get();
		r = cc3501e_ble_gatt_notify(&fw, use_handle, k_notify_val, sizeof(k_notify_val), 2000u);
		sweep_report(ALP_CC3501E_CMD_BLE_GATT_NOTIFY,
		             "BLE_GATT_NOTIFY",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "expected refusal -- no BLE central is connected on this bench run");

		uint8_t read_buf[8];
		size_t  read_len = 0u;
		t0               = k_uptime_get();
		r = cc3501e_ble_gatt_read(&fw, use_handle, read_buf, sizeof(read_buf), &read_len, 2000u);
		sweep_report(ALP_CC3501E_CMD_BLE_GATT_READ,
		             "BLE_GATT_READ",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "expected refusal -- no BLE central is connected on this bench run");

		static const uint8_t k_write_val[1] = { 0x01 };
		t0                                  = k_uptime_get();
		r = cc3501e_ble_gatt_write(&fw, use_handle, k_write_val, sizeof(k_write_val), 2000u);
		sweep_report(ALP_CC3501E_CMD_BLE_GATT_WRITE,
		             "BLE_GATT_WRITE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "expected refusal -- no BLE central is connected on this bench run");

		t0 = k_uptime_get();
		r  = advertising ? cc3501e_ble_adv_stop(&fw, 2000u) : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_ADV_STOP,
		             "BLE_ADV_STOP",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             advertising ? "" : "BLE_ADV_START above did not succeed");

		/* BLE_CONNECT (central role): no real peripheral is guaranteed
		 * present on this bench, so an all-zero dummy address is used
		 * purely to exercise the opcode -- exactly the "clean refusal,
		 * not a failure" shape the task's CAM_ENABLE note describes,
		 * applied here to the one other opcode that needs a peer this
		 * bench cannot promise. Bounded to 3 s so a non-existent peer
		 * cannot stall the rest of the sweep. */
		static const uint8_t k_dummy_peer[6] = { 0, 0, 0, 0, 0, 0 };
		t0                                   = k_uptime_get();
		r = ble_up ? cc3501e_ble_connect(&fw, k_dummy_peer, 0u, 3000u) : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_CONNECT,
		             "BLE_CONNECT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "target is an all-zero dummy address -- no real peripheral is guaranteed"
		             " present on this bench, so a timeout here is the expected clean refusal");

		t0 = k_uptime_get();
		r  = ble_up ? cc3501e_ble_disconnect(&fw, 2000u) : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_DISCONNECT,
		             "BLE_DISCONNECT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             "no active connection exists -- expected no-op/refusal");

		t0 = k_uptime_get();
		r  = ble_up ? cc3501e_ble_disable(&fw, 3000u) : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_BLE_DISABLE,
		             "BLE_DISABLE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "leaves BLE off for the remaining families");
	}

	/* ---- OTA (0x40-0x47), READ-ONLY OPCODES ONLY -----------------------
	 * BEGIN/WRITE/FINISH/PROMOTE/UPDATE_MODE write the coprocessor's flash
	 * and can leave it unbootable -- NEVER invoked, per the safety
	 * boundary at the top of this file and the task that specifies it.
	 * STATUS and ABORT are read-only / session-reset-only and ARE safe. */
	printk("\n--- OTA (0x40-0x47), read-only subset ---\n");
	{
		sweep_skip(ALP_CC3501E_CMD_OTA_BEGIN,
		           "OTA_BEGIN",
		           "NEVER invoked -- opens a flash-write session; see the safety boundary"
		           " at the top of this file");
		sweep_skip(ALP_CC3501E_CMD_OTA_WRITE,
		           "OTA_WRITE",
		           "NEVER invoked -- streams image bytes into flash");
		sweep_skip(ALP_CC3501E_CMD_OTA_FINISH,
		           "OTA_FINISH",
		           "NEVER invoked -- stages a flash image for install");
		sweep_skip(ALP_CC3501E_CMD_OTA_PROMOTE,
		           "OTA_PROMOTE",
		           "NEVER invoked -- commits a staged image + reboots the coprocessor");
		sweep_skip(ALP_CC3501E_CMD_OTA_UPDATE_MODE,
		           "OTA_UPDATE_MODE",
		           "NEVER invoked -- warm-reboots the coprocessor into the polled OTA"
		           " loop, which would strand the rest of this sweep (Wi-Fi/BLE/GET_MAC"
		           " all queue forever in that mode)");

		alp_cc3501e_ota_status_t ota = { 0 };
		int64_t                  t0  = k_uptime_get();
		alp_status_t             r   = cc3501e_ota_status(&fw, &ota, 2000u);
		sweep_report(ALP_CC3501E_CMD_OTA_STATUS,
		             "OTA_STATUS",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             (r == ALP_OK) ? "read-only -- safe at any point in a session" : "");
		if (r == ALP_OK) {
			printk("  session state=%u  bytes_written=%u/%u  pending=%u\n",
			       (unsigned)ota.state,
			       (unsigned)ota.bytes_written,
			       (unsigned)ota.total_len,
			       (unsigned)ota.pending);
		}

		/* Read-only in effect: resets any stray IN-PROGRESS session state
		 * to IDLE without writing or committing anything to flash. Safe
		 * to call even when (as expected here) no session was ever
		 * opened. */
		t0 = k_uptime_get();
		r  = cc3501e_ota_abort(&fw, 2000u);
		sweep_report(ALP_CC3501E_CMD_OTA_ABORT,
		             "OTA_ABORT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "no session was ever opened by this app -- resets session state to IDLE"
		             " (never touches flash)");
	}

	/* ---- STREAM_WRITE (0x45), coverage only -----------------------------
	 * One small frame here to account for the opcode; the full sweep
	 * across sizes already ran in Part 2 above. */
	printk("\n--- STREAM (0x45), coverage ---\n");
	{
		memset(g_bulk_buf, 0x5A, 64u);
		int64_t      t0 = k_uptime_get();
		alp_status_t r  = cc3501e_stream_write(&fw, g_bulk_buf, 64u);
		sweep_report(ALP_CC3501E_CMD_STREAM_WRITE,
		             "STREAM_WRITE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "64 B coverage probe -- see Part 2 above for the full size sweep");
	}

	/* ---- GPIO proxy (0x50-0x53) ------------------------------------------
	 * pad 2 = raw CC3501E GPIO2, which the SoM pad map
	 * (metadata/e1m_modules/aen/from-cc3501e.tsv) routes to E1M pad A18 /
	 * IO11 -- NOT one of the reserved bridge/console/READY pads (5, 6, 7,
	 * 8, 9, 16, 17, 27, 28, 29 -- see <alp/chips/cc3501e/gpio.h>'s
	 * cc3501e_gpio_routes[] doc), so driving it cannot desync the link
	 * this app is running over. Configured OUTPUT/no-pull, not via the
	 * portable proxy backend (this app does not build
	 * CONFIG_ALP_SDK_GPIO_CC3501E_PROXY -- see prj.conf) -- these are the
	 * raw chip-level cc3501e_gpio_*() calls the proxy itself is built on.
	 */
	printk("\n--- GPIO proxy (0x50-0x53) ---\n");
	{
		const uint8_t pad = 2u; /* IO11 -> CC3501E GPIO2, unreserved */

		int64_t      t0 = k_uptime_get();
		alp_status_t r  = cc3501e_gpio_configure(
		    &fw, pad, ALP_CC3501E_GPIO_DIR_OUTPUT, ALP_CC3501E_GPIO_PULL_NONE, 2000u);
		sweep_report(ALP_CC3501E_CMD_GPIO_CONFIGURE,
		             "GPIO_CONFIGURE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "pad 2 (IO11) as OUTPUT/no-pull");

		t0 = k_uptime_get();
		r  = cc3501e_gpio_write(&fw, pad, true, 2000u);
		sweep_report(ALP_CC3501E_CMD_GPIO_WRITE,
		             "GPIO_WRITE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "drive high");

		bool level = false;
		t0         = k_uptime_get();
		r          = cc3501e_gpio_read(&fw, pad, &level, 2000u);
		sweep_report(ALP_CC3501E_CMD_GPIO_READ,
		             "GPIO_READ",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             (r == ALP_OK)
		                 ? (level ? "read back high, as driven" : "read back LOW -- unexpected")
		                 : "");

		(void)cc3501e_gpio_write(
		    &fw, pad, false, 2000u); /* return low -- not itself a reported opcode */

		t0 = k_uptime_get();
		r  = cc3501e_gpio_set_interrupt(&fw, pad, ALP_CC3501E_GPIO_EDGE_RISING, true, 2000u);
		sweep_report(ALP_CC3501E_CMD_GPIO_SET_INTERRUPT,
		             "GPIO_SET_INTERRUPT",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "arm rising edge (delivery is deferred -- no host-IRQ line on this rev,"
		             " see the opcode's own doc)");
		/* Disarm again -- leaves the pad in a clean state for whatever
		 * runs next, same "enable/disable pair before the next family"
		 * rule the file header describes. */
		(void)cc3501e_gpio_set_interrupt(&fw, pad, ALP_CC3501E_GPIO_EDGE_NONE, false, 2000u);
	}

	/* ---- SPI1 host passthrough (0x55-0x57), coverage --------------------
	 * The E1M connector's SPI1 lands on the CC3501E (it is the SPI
	 * controller, relaying bytes -- see <alp/chips/cc3501e/core.h>'s own
	 * doc block), NOT on the Alif, and nothing is attached to that
	 * connector on this bench. A TRANSFER still completes: the CC3501E
	 * clocks real bits on an idle bus regardless of what is (or is not)
	 * listening, so this proves the opcode even with rx discarded. The
	 * full size sweep already ran in Part 2 above. */
	printk("\n--- SPI1 host passthrough (0x55-0x57), coverage ---\n");
	{
		uint32_t     actual_freq = 0u;
		uint16_t     max_xfer    = 0u;
		int64_t      t0          = k_uptime_get();
		alp_status_t r           = cc3501e_spi1_configure(
		    &fw, 1000000u, 0u, ALP_CC3501E_SPI1_CS0, &actual_freq, &max_xfer, 3000u);
		sweep_report(ALP_CC3501E_CMD_SPI1_CONFIGURE,
		             "SPI1_CONFIGURE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK)              ? "OK"
		             : (r == ALP_ERR_NOT_READY) ? "REFUSED"
		                                        : "FAIL",
		             (r == ALP_OK) ? ""
		                           : "expected on firmware built without the SPI1 passthrough");
		if (r == ALP_OK) {
			printk(
			    "  actual_freq=%u Hz  max_xfer=%u B\n", (unsigned)actual_freq, (unsigned)max_xfer);
		}
		bool configured = (r == ALP_OK);

		memset(g_bulk_buf, 0xC3, 64u);
		t0 = k_uptime_get();
		r  = configured ? cc3501e_spi1_transfer(&fw, g_bulk_buf, NULL, 64u, 0xFFu, false, 3000u)
		                : ALP_ERR_NOT_READY;
		sweep_report(ALP_CC3501E_CMD_SPI1_TRANSFER,
		             "SPI1_TRANSFER",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "64 B coverage probe, rx discarded, no peripheral on this bench's SPI1"
		             " connector -- see Part 2 above for the full size sweep");

		t0 = k_uptime_get();
		r  = cc3501e_spi1_release(&fw, 2000u);
		sweep_report(ALP_CC3501E_CMD_SPI1_RELEASE,
		             "SPI1_RELEASE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "frees the SPI1 controller -- has no preconditions and cannot fail on"
		             " state");
	}

	/* ---- Camera + power (0x60-0x62) --------------------------------- */
	printk("\n--- Camera + power (0x60-0x62) ---\n");
	{
		int64_t      t0 = k_uptime_get();
		alp_status_t r  = cc3501e_cam_enable(&fw, 0u, true, 2000u);
		sweep_report(ALP_CC3501E_CMD_CAM_ENABLE,
		             "CAM_ENABLE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "REFUSED",
		             (r == ALP_OK) ? "LDO driven -- this is just a GPIO enable pin, so ALP_OK"
		                             " does not by itself prove a camera is present"
		                           : "no camera is fitted on this bench -- a clean refusal here"
		                             " is the expected shape, not a failure");

		t0 = k_uptime_get();
		r  = cc3501e_cam_enable(&fw, 0u, false, 2000u);
		sweep_report(ALP_CC3501E_CMD_CAM_DISABLE,
		             "CAM_DISABLE",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "restores the LDO enable pin to its default-off state");

		alp_cc3501e_power_policy_t policy = {
			.policy               = (uint8_t)ALP_CC3501E_PP_BALANCED,
			.wake_events          = ALP_CC3501E_WAKE_HOST_SPI, /* keep the bridge itself alive */
			.reserved             = 0u,
			.idle_ms_before_sleep = 0u, /* 0 = firmware default */
		};
		bool radio_ok = true;
		t0            = k_uptime_get();
		r             = cc3501e_power_policy(&fw, &policy, &radio_ok, 2000u);
		sweep_report(ALP_CC3501E_CMD_POWER_POLICY,
		             "POWER_POLICY",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "BALANCED preset, HOST_SPI wake kept armed -- the firmware default"
		             " shape, so this leaves the device in the same policy it booted with");
	}

	/* ---- Diagnostics (0x70-0x71), final tally --------------------------
	 * Run LAST, after every other family, so DIAG_GET_STATS tallies the
	 * WHOLE sweep's frame counters -- including Part 2's throughput
	 * traffic, since Part 2 now runs BEFORE this point (see ORDER in the
	 * file header) -- not just this one family's traffic. */
	printk("\n--- Diagnostics (0x70-0x71), final tally ---\n");
	{
		cc3501e_diag_stats_t stats = { 0 };
		int64_t              t0    = k_uptime_get();
		alp_status_t         r     = cc3501e_diag_stats(&fw, &stats);
		sweep_report(ALP_CC3501E_CMD_DIAG_GET_STATS,
		             "DIAG_GET_STATS",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "tallies EVERY frame this whole sweep has sent so far, not just this"
		             " family's");
		if (r == ALP_OK) {
			printk("  frames_ok=%u frames_err=%u worker_execs=%u retry_latch_hits=%u"
			       " has_worker_counters=%s\n",
			       (unsigned)stats.frames_ok,
			       (unsigned)stats.frames_err,
			       (unsigned)stats.worker_execs,
			       (unsigned)stats.retry_latch_hits,
			       stats.has_worker_counters ? "true" : "false");
		}

		t0 = k_uptime_get();
		r  = cc3501e_diag_log_level(&fw, 1u);
		sweep_report(ALP_CC3501E_CMD_DIAG_LOG_LEVEL,
		             "DIAG_LOG_LEVEL",
		             r,
		             k_uptime_get() - t0,
		             (r == ALP_OK) ? "OK" : "FAIL",
		             "sets firmware log verbosity to 1 for the remainder of this boot");
	}

	/*
	 * =================================================================
	 * PART 3 -- self-check: did every opcode get accounted for?
	 * =================================================================
	 */
	unsigned missing = sweep_self_check();

	printk("\n=== DONE: bring-up=%d fw_proto=%u.%u opcodes_missing=%u ===\n",
	       (int)rc,
	       (unsigned)fw.fw_proto_major,
	       (unsigned)fw.fw_proto_minor,
	       missing);

	return 0;
}
