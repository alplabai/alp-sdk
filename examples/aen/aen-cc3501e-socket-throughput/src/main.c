/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-socket-throughput -- measures CC3501E end-to-end TCP socket
 * throughput over a correctly sized window, for the E1M-AEN801 (Alif
 * Ensemble E8, M55-HE), bench RAM-run via J-Link.
 *
 * WHY THIS APP EXISTS -- a measurement that was wrong
 * -----------------------------------------------------
 * A previous sweep (aen-cc3501e-command-sweep's Part 2) reported
 * STREAM_WRITE throughput as 104 KB/s at 64 bytes and 250 KB/s at 256
 * bytes. Those numbers are artifacts, not rates. That sweep's timing used
 * k_uptime_get(), which is millisecond-granular, and it timed FIVE
 * transfers of 256 bytes each -- about 255 microseconds per transfer on a
 * link running near 980 KB/s. Every one of those five transfers landed AT
 * OR BELOW the timer's own tick: the sweep's own min/max columns read
 * "min=1 max=1" (millisecond), which is the timer's floor, not the
 * transfer's actual duration. Dividing a byte count by a millisecond-
 * quantized total makes the reported rate climb with transfer size whether
 * or not per-frame overhead is actually amortizing -- 256 B / 1 ms reads as
 * four times faster than 64 B / 1 ms even when both really took under a
 * millisecond and neither number means anything.
 *
 * The arithmetic checks out exactly: 256 bytes at a real 980 KB/s takes
 * about 255 us, which a 1 ms-tick clock rounds UP to 1 ms, and
 * 256 B / 1 ms = 250 KB/s -- precisely the number that sweep reported. The
 * four-fold gap between 250 KB/s and the ~980 KB/s this link actually runs
 * was entirely the instrument, not the link.
 *
 * The real figure for this link is believed to be near 980 KB/s. The
 * repo's own recorded numbers agree in magnitude: the CC3501E bridge
 * firmware's `hal/ti/cc3501e_hw_ti_sock.c` documents "end-to-end HTTP over
 * the bridge: 8 KB = ~660 kB/s, 16 KB = ~730 kB/s, 64 KB = ~742 kB/s" --
 * climbing toward that ~980 KB/s figure as the transfer amortizes more of
 * its fixed per-frame cost over more data, exactly the shape a genuine rate
 * has and a timer-floor artifact does not.
 *
 * That same firmware file documents the fix, and it is the pattern this
 * app copies: it publishes a WINDOWED rate, accumulating at least 131072
 * bytes (128 KiB) before dividing ONCE, and its own comment explains why a
 * naive per-transfer average is worse than useless: "A cumulative average
 * from socket-open decays toward zero as soon as the transfer finishes and
 * the socket goes idle, which is what made the first attempt read 12 kB/s
 * on a link doing far more." Measure a big window, divide once. This app
 * is that pattern, applied end-to-end from this host's side of the link
 * instead of the coprocessor's radio-only loopback.
 *
 * THE WINDOW SIZE
 * ----------------
 * SOCKTP_BYTE_BUDGET below is at least 4 MiB, deliberately -- see that
 * macro's own comment for the arithmetic. The short version: at ~980 KB/s,
 * 4 MiB takes about 4 seconds, which turns k_uptime_get()'s 1 ms tick into
 * a ~0.02% error on the TOTAL instead of the 4x error a single
 * millisecond-quantized 256-byte transfer produced above. Never shrink
 * this budget back toward the noise floor -- that is exactly the mistake
 * this app exists to not repeat.
 *
 * WHAT THIS APP MEASURES
 * ------------------------
 * Bring the bridge up (STEP 1, byte-identical to aen-evk-demo phase 8 and
 * every aen-cc3501e-* sibling). Associate to Wi-Fi (STEP 2, skipped
 * entirely when no SSID is configured -- see CREDENTIALS below). Open a
 * TCP socket, issue a plain HTTP GET, then read the response BODY in a
 * loop -- skipping past the HTTP headers first, since they are a fixed
 * few-hundred-byte cost that has nothing to do with the link's own
 * throughput -- until the transfer completes or SOCKTP_BYTE_BUDGET is
 * reached, timing the WHOLE window with k_uptime_get() and dividing once
 * at the end, never per-call. The clock starts on the first BODY byte, not
 * on socket-open or on the request being sent -- the same reasoning
 * `cc3501e_hw_ti_sock.c`'s own radio-speedtest loop documents: the gap
 * between arming the socket and the server's first segment is idle time
 * that has nothing to do with the transfer rate and would only dilute it.
 *
 * CREDENTIALS -- never committed
 * ---------------------------------
 * Follows the convention aen-cc3501e-companion-tour's file header
 * establishes (read that app's TOUR_WIFI_SSID comment for the fuller
 * version of this note): credentials arrive as build-time defines, with
 * EMPTY defaults, and this app skips every networked step when the SSID is
 * empty. This app uses its OWN macro names (SOCKTP_WIFI_SSID /
 * SOCKTP_WIFI_PASS / SOCKTP_SERVER_*), not the tour's, so the two examples
 * never fight over a shared -D on a combined build. No real SSID,
 * passphrase, or server address is embedded anywhere in this file -- see
 * the Tunables section below for the exact override syntax.
 *
 * RECV SIZING
 * -------------
 * SOCKTP_RECV_CEILING below is not a round number -- see its own comment
 * for the exact wrapper-internal ceiling it mirrors and why a smaller cap
 * would only add avoidable SOCK_RECV round trips for the same total bytes.
 *
 * BRIDGE-ONLY MODE -- a number without Wi-Fi
 * ---------------------------------------------
 * STEP 4 above needs a live association and a server that answers. When
 * neither is available (the socket path has, as of this writing, never
 * completed a run against this bridge's Wi-Fi association), this app can
 * still produce a REAL, defensible figure for the piece that IS working:
 * the host<->CC3501E SPI link itself, driven with STREAM_WRITE, no radio
 * involved at all.
 *
 * MODE SELECTION -- reuses the SSID-empty condition, not a new define. STEP
 * 3 already treats an empty SOCKTP_WIFI_SSID as "nothing downstream can
 * run"; this app now spends that same idle time on a bridge-only sweep
 * instead of just returning. A credential-free build therefore measures the
 * bridge; a credentialed build still measures end-to-end, exactly as
 * before. Chosen over a dedicated build-time define because the two modes
 * are already mutually exclusive on the SAME condition (no credentials no
 * end-to-end path exists to compare against anyway) -- a second knob would
 * only add a way for the two to disagree with each other for no benefit.
 *
 * NOT COMPARABLE TO END-TO-END -- say so, do not let the two blur. The
 * bridge-only figure below measures ONE segment of the path (host SPI ->
 * CC3501E), with no radio, no association, no IP stack, no server on the
 * other end. It is not, and must never be read as, a substitute for the
 * end-to-end HTTP figure STEP 4 measures when credentials ARE set -- that
 * number includes the radio and everything downstream of it. The bridge
 * firmware's own `hal/ti/cc3501e_hw_ti_sock.c` records that end-to-end
 * figure directly: "end-to-end HTTP over the bridge: 8 KB = ~660 kB/s,
 * 16 KB = ~730 kB/s, 64 KB = ~742 kB/s". That is the END-TO-END number.
 * Whatever this app's bridge-only sweep reports is a DIFFERENT
 * measurement of a DIFFERENT, smaller piece of the path -- see this app's
 * README for the same caveat in the form a reader who skips this file
 * will still see.
 *
 * THE WINDOW, AGAIN -- the exact same discipline as SOCKTP_BYTE_BUDGET
 * above, and for the identical reason: the earlier link-only sweep
 * (aen-cc3501e-command-sweep's Part 2) that produced the false 104 KB/s
 * (64 B) and 250 KB/s (256 B) figures this file's WHY THIS APP EXISTS
 * section dissects was THIS SAME STREAM_WRITE opcode, timed per-repeat
 * instead of over a real window. BRIDGE_BYTE_BUDGET below accumulates
 * before dividing once, per size, for the same reason SOCKTP_BYTE_BUDGET
 * does -- see that macro's own comment for the arithmetic this one mirrors.
 *
 * THE 1024-BYTE TRAP -- do not extend BRIDGE_SIZES
 * -----------------------------------------------------
 * Measured, reproducible 3 of 3 on bench: 64 B and 256 B STREAM_WRITE calls
 * pass 5 of 5, while 1024 B and 4092 B (cc3501e_stream_write()'s own
 * ceiling, ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES) both fail
 * with rc=-5 (ALP_ERR_TIMEOUT) and the link does NOT recover for the rest
 * of that run. BRIDGE_SIZES below sweeps 64 / 128 / 256 / 512 and stops
 * there -- do not add 1024 or above; that would trade a working sweep for a
 * wedged bench session. 512 itself is UNTESTED and may wedge too, which is
 * exactly why the sweep function reports each size AS SOON AS it completes
 * and stops -- never discarding what already succeeded -- on the very first
 * STREAM_WRITE failure, instead of ploughing into a dead link and reporting
 * zeros as if they were data.
 *
 * Build target: alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he -- see this
 * app's README.md for the full west build invocation and the overlay's own
 * header for why the SRAM0 memory placement is load-bearing, not
 * decorative: this app's own SOCK_RECV window rides the identical SPI1
 * link every other AEN801 CC3501E bench app measures, and a faster
 * cache-on/DTCM path would inflate the number on precisely the FIFO-refill
 * axis this measurement exists to characterise.
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
 * Tunables -- timeouts, the (build-time) Wi-Fi credentials + server
 * target, and the window-sizing constants the file header above explains.
 * ======================================================================
 */

/* How many times STEP 2 retries PING before giving up on the bridge ever
 * answering. cc3501e_reset() (called by STEP 1's bring-up) already waited
 * out the boot budget once; this loop only absorbs residual ramp/boot
 * jitter, the same reasoning aen-cc3501e-companion-tour's tour_ping()
 * documents. */
#define SOCKTP_PING_RETRIES 25u
#define SOCKTP_PING_GAP_MS  200u

/* Poll-by-repeat budgets for the one-shot radio + socket ops below. These
 * are UPPER BOUNDS on a real op's own worker-routed wait, not this app's
 * own pacing -- generous for the same reason aen-cc3501e-companion-tour's
 * TOUR_* timeouts are.
 *
 * SOCKTP_CONNECT_TIMEOUT_MS is derived from the bridge firmware's OWN two
 * worst cases for a STA connect (hal/ti/cc3501e_hw_ti_wifi.c), not a round
 * number: the L2 association wait is `osi_SyncObjWait(&wifi_event_sync,
 * 30u * OSI_WAIT_FOR_SECOND)` (30 s -- that comment records WPA2 associating
 * ~15 s in and WPA3-SAE, with its extra SAE commit/confirm exchange + PMF,
 * running slower still, which is why the wait was widened from 15 s to 30 s
 * after a bench-seen WPA3 timeout), and DHCP afterward is bounded by
 * `CC3501E_STA_DHCP_TRIES * CC3501E_STA_DHCP_POLL_US` = 100 * 200 ms = 20 s.
 *
 * There is a THIRD term the original derivation missed: this image does not
 * call cc3501e_hw_wifi_boot_start(), so a connect issued as the first radio op
 * of a boot carries Wlan_Start, a Wlan_Set and a 10 s Wlan_RoleUp INSIDE the
 * connect body, before the association wait even begins.
 *
 * 10 s + 30 s + 20 s = 60 s is the firmware's own worst case for one connect; a
 * caller budget below that races a healthy association even with perfectly
 * accurate host-side accounting.  55000u -- this file's previous value, derived
 * when DHCP was 10 s and the role-up term was missed -- no longer clears it, so
 * by this app's own rule it was buying failures the radio never suffered. */
#define SOCKTP_CONNECT_TIMEOUT_MS 75000u
#define SOCKTP_SOCK_TIMEOUT_MS    5000u
#define SOCKTP_RECV_TIMEOUT_MS    3000u

/* How long to wait, after cc3501e_wifi_connect() itself reports a non-OK
 * status, before reading the independent WIFI_STATUS latch for the real
 * radio verdict -- see the call site below. Long enough to cover the
 * firmware's own worst-case connect budgets (30 s L2 association + 10 s
 * DHCP, see SOCKTP_CONNECT_TIMEOUT_MS's derivation above) even when the
 * failure came from the HOST giving up early, not the radio. */
#define SOCKTP_VERDICT_WAIT_MS 45000u

/*
 * Wi-Fi STA credentials for the CONNECT step. DELIBERATELY EMPTY by
 * default -- never hardcode bench credentials in a public example. Set
 * them at build time WITHOUT editing this file, e.g.:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DSOCKTP_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DSOCKTP_WIFI_PASS=\\\"mypass\\\""
 *
 * When SOCKTP_WIFI_SSID is empty, main() prints one line saying so and
 * returns before touching the radio or the socket API at all -- there is
 * nothing to associate with, so there is nothing to measure.
 */
#ifndef SOCKTP_WIFI_SSID
#define SOCKTP_WIFI_SSID ""
#endif
#ifndef SOCKTP_WIFI_PASS
#define SOCKTP_WIFI_PASS ""
#endif
/* Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (matches
 * CC3501E_WIFI_CONNECT_SEC_* in <alp/chips/cc3501e/wifi.h>). */
#ifndef SOCKTP_WIFI_SECURITY
#define SOCKTP_WIFI_SECURITY 1u
#endif

/*
 * TCP target for the throughput GET, overridable the same way as the
 * credentials above:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DSOCKTP_SERVER_A=192 \
 *     -DSOCKTP_SERVER_B=168 -DSOCKTP_SERVER_C=1 -DSOCKTP_SERVER_D=50 \
 *     -DSOCKTP_SERVER_PORT=8000 -DSOCKTP_HTTP_PATH=\\\"/bigfile.bin\\\""
 *
 * Defaults point at the .1 gateway of a generic 192.168.1/24 bench LAN on
 * port 80, path "/" -- a placeholder, not a real bench server; point it at
 * a host that actually answers with a large body before trusting a
 * result. ip[] is network order (ip[0] = most-significant octet), the same
 * layout cc3501e_wifi_get_ip() hands back.
 */
#ifndef SOCKTP_SERVER_A
#define SOCKTP_SERVER_A 192
#endif
#ifndef SOCKTP_SERVER_B
#define SOCKTP_SERVER_B 168
#endif
#ifndef SOCKTP_SERVER_C
#define SOCKTP_SERVER_C 1
#endif
#ifndef SOCKTP_SERVER_D
#define SOCKTP_SERVER_D 1
#endif
#ifndef SOCKTP_SERVER_PORT
#define SOCKTP_SERVER_PORT 80u
#endif
#ifndef SOCKTP_HTTP_PATH
#define SOCKTP_HTTP_PATH "/"
#endif
static const uint8_t SOCKTP_SERVER_IP[4] = { SOCKTP_SERVER_A,
	                                         SOCKTP_SERVER_B,
	                                         SOCKTP_SERVER_C,
	                                         SOCKTP_SERVER_D };

/* Plain HTTP/1.0 GET, built entirely at COMPILE time via C string-literal
 * concatenation -- no snprintf, no runtime formatting, so there is no
 * dependency on a full libc being linked in. "Connection: close" asks the
 * server to close once the body is sent, which is what lets this app tell
 * "the transfer finished" apart from "nothing is queued yet" below (see
 * SOCKTP_ZERO_STREAK_DONE). The Host header is a fixed placeholder, not
 * the actual dotted-quad target -- this app talks to a bare IP, not a
 * name-based virtual host, so an accurate Host value buys nothing here. */
static const char SOCKTP_HTTP_REQUEST[] =
    "GET " SOCKTP_HTTP_PATH " HTTP/1.0\r\nHost: aen-bench\r\nConnection: close\r\n\r\n";

/*
 * Minimum window this app accumulates before deriving a rate -- see the
 * file header's WHY THIS APP EXISTS for the bug this directly avoids. At
 * roughly 980 KB/s (the figure this link's own firmware end-to-end HTTP
 * numbers -- 8 KB=~660 kB/s, 16 KB=~730 kB/s, 64 KB=~742 kB/s -- converge
 * toward at larger transfers) 4 MiB takes about 4 seconds, so
 * k_uptime_get()'s 1 ms tick is a ~0.02% error on the TOTAL instead of the
 * 4x error a single millisecond-quantized transfer produced. DO NOT shrink
 * this back toward the noise floor -- that is precisely the mistake the
 * file header documents. */
#define SOCKTP_BYTE_BUDGET (4u * 1024u * 1024u) /* 4 MiB */

/* Print a running rate roughly this often, so a stalled transfer is
 * visible on the console instead of looking like a hang -- the byte
 * budget above is big enough that silence for its whole duration would
 * otherwise be indistinguishable from a dead link. */
#define SOCKTP_PROGRESS_STEP_BYTES (512u * 1024u) /* ~512 KiB */

/* Safety net, NOT part of the measurement: aborts the session if it runs
 * this long without completing or hitting the byte budget. ~15x the ~4 s a
 * healthy ~980 KB/s link needs for the 4 MiB default budget -- generous
 * enough not to falsely abort a slow-but-working link, short enough that a
 * genuinely wedged link does not hang the bench session forever. */
#define SOCKTP_SESSION_TIMEOUT_MS (60u * 1000u)

/* How many consecutive zero-length OK results this app treats as "the
 * transfer is over". cc3501e_sock_recv()'s own doc is explicit that a
 * single zero-length OK is ambiguous -- "no data was available within the
 * firmware's receive window, or the peer closed the connection -- the
 * caller polls again to distinguish". Requiring a short RUN of them,
 * rather than trusting the first one, is the same reasoning
 * aen-cc3501e-command-sweep's wedge tracker uses for FAIL verdicts: one
 * empty poll mid-stream is normal jitter, three in a row is a pattern. */
#define SOCKTP_ZERO_STREAK_DONE 3u

/*
 * cc3501e_sock_recv()'s OWN internal ceiling (chips/cc3501e/cc3501e_sockets.c):
 * it clamps whatever `cap` this app passes down to
 * ALP_CC3501E_MAX_PAYLOAD minus the wire recv-reply header
 * (sizeof(alp_cc3501e_sock_recv_resp_t), the PUBLIC struct this app can
 * see, not the private CC3501E_SOCK_RECV_RESP_HDR constant the driver
 * computes the identical value from) minus a further 1-byte margin the
 * driver reserves -- regardless of what cap this app requests. That
 * driver-internal bound (4096 - 24 - 1 = 4071 bytes as of protocol v5) is
 * already tighter than the generic wire-MAJOR-4 CRC-adjusted ceiling
 * (ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_CRC_BYTES = 4094) the same way
 * aen-cc3501e-command-sweep's own sweep_crc_headroom() comment describes
 * for STREAM_WRITE: the wrapper already applies the tighter bound itself,
 * so no further CRC-headroom subtraction belongs here on top of it.
 *
 * Passing exactly this ceiling -- not a round number like 256 or 1024 --
 * means every SOCK_RECV call actually moves the largest chunk the wrapper
 * will ever hand back in one round trip. A smaller cap does not fail; it
 * just adds more round trips for the same total bytes, which is pure
 * per-frame overhead this app's own measurement exists to not hide. */
#define SOCKTP_RECV_CEILING \
	((uint16_t)(ALP_CC3501E_MAX_PAYLOAD - sizeof(alp_cc3501e_sock_recv_resp_t) - 1u))

/* STATIC, not a local -- same discipline as the bulk buffers in every
 * aen-cc3501e-* throughput sibling (see main()'s own cc3501e_t comment
 * below for why that discipline is load-bearing, not optional, in this
 * file). */
static uint8_t g_recv_buf[SOCKTP_RECV_CEILING];

/*
 * ======================================================================
 * HTTP header skip -- this app measures BODY throughput, not "everything
 * that came off the socket". The response headers are a fixed few-hundred
 * -byte cost with nothing to do with the link's own rate; folding them
 * into the timed total would bias every result toward a slightly lower
 * number by a fixed offset that shrinks as a fraction of the total the
 * bigger SOCKTP_BYTE_BUDGET is set, which is exactly the kind of "the
 * number moves for a reason that has nothing to do with what changed"
 * artifact this app exists to avoid elsewhere.
 * ======================================================================
 */

/** Scan @p buf for the "\r\n\r\n" header/body separator, carrying partial
 *  match state in @p match (0..4) across calls so the separator is found
 *  even when it straddles a SOCK_RECV chunk boundary. On a match, sets
 *  @p found and returns the index of the first BODY byte in @p buf --
 *  which may legitimately equal @p len when the separator lands exactly at
 *  the end of this chunk (header done, zero body bytes in THIS chunk, more
 *  body due next call). Clears @p found and returns @p len when the
 *  separator has not completed within this chunk. Distinguishing "found,
 *  landed at the boundary" from "not found yet" via a dedicated out-param
 *  -- rather than the caller inferring it from `return value < len` --
 *  matters: that inference is wrong exactly at the boundary case and would
 *  otherwise silently re-arm the header scan on what is actually body data.
 *
 *  Naive scan-with-restart, not a general substring matcher (no KMP
 *  failure table): "\r\n\r\n" has the one self-overlap a restart already
 *  handles for free (a failed match on '\r' just restarts the match at
 *  state 1 instead of 0), so nothing more general is needed for this one
 *  fixed 4-byte separator. */
static size_t http_skip_header(const uint8_t *buf, size_t len, unsigned *match, bool *found)
{
	static const char sep[4] = { '\r', '\n', '\r', '\n' };

	*found = false;
	for (size_t i = 0u; i < len; i++) {
		if (buf[i] == (uint8_t)sep[*match]) {
			(*match)++;
			if (*match == 4u) {
				*found = true;
				return i + 1u;
			}
		} else {
			*match = (buf[i] == (uint8_t)sep[0]) ? 1u : 0u;
		}
	}
	return len;
}

/*
 * ======================================================================
 * Reporting -- see the file header's "Report, as separate figures" ask:
 * every number below is printed on its own line so the arithmetic
 * (bytes * 1000 / elapsed_ms == rate) can be checked by hand, and the
 * KB/s figure is fixed-point (no float) so this app's printk build does
 * not depend on CONFIG_CBPRINTF_FP_SUPPORT.
 * ======================================================================
 */

static void socktp_report(uint32_t body_bytes,
                          uint32_t wire_bytes,
                          int64_t  elapsed_ms,
                          uint32_t recv_calls,
                          uint32_t body_calls)
{
	printk("\n=== RESULT ===\n");
	printk("  total body bytes received (timed)   = %u\n", body_bytes);
	printk("  total bytes off the wire (hdr+body) = %u\n", wire_bytes);
	printk("  elapsed_ms (whole window, one clock) = %lld\n", (long long)elapsed_ms);
	if (elapsed_ms > 0) {
		uint32_t rate_bps = (uint32_t)(((uint64_t)body_bytes * 1000u) / (uint64_t)elapsed_ms);
		printk("  rate = %u B/s (%u.%02u KB/s)\n",
		       rate_bps,
		       rate_bps / 1024u,
		       (rate_bps % 1024u) * 100u / 1024u);
	} else {
		printk("  rate = not derived -- no body byte was ever timed (elapsed_ms is 0)\n");
	}
	printk("  cc3501e_sock_recv calls (total)      = %u\n", recv_calls);
	if (body_calls > 0u) {
		printk("  cc3501e_sock_recv calls carrying body = %u, mean bytes/call = %u\n",
		       body_calls,
		       body_bytes / body_calls);
	} else {
		printk("  cc3501e_sock_recv calls carrying body = 0 -- no data ever arrived\n");
	}
}

/*
 * ======================================================================
 * BRIDGE-ONLY mode -- see the file header's BRIDGE-ONLY MODE section for
 * what this measures, why it is not comparable to an end-to-end figure, and
 * why the sweep must never go past 512 bytes.
 * ======================================================================
 */

/* Sizes to sweep, ASCENDING -- see the file header's THE 1024-BYTE TRAP.
 * Never add 1024 or above to this list. */
static const uint32_t BRIDGE_SIZES[] = { 64u, 128u, 256u, 512u };

/* Per-size window. >= 131072 (128 KiB) is the hard floor the file header's
 * WHY THIS APP EXISTS section derives; this app uses 1 MiB instead, the
 * same "prefer bigger where it's cheap" call SOCKTP_BYTE_BUDGET makes --
 * at the link's real ~980 KB/s that's about a second per size, pushing the
 * quantisation error another order down for a cost not worth trading away.
 * Smaller sizes take longer per window (more per-frame overhead to
 * amortize) but the whole four-size sweep still finishes in well under a
 * minute at any rate this link has ever measured. */
#define BRIDGE_BYTE_BUDGET (1024u * 1024u) /* 1 MiB, per size */

/* Same reasoning as SOCKTP_PROGRESS_STEP_BYTES -- a stalled bridge should
 * look like a stall, not a hang. */
#define BRIDGE_PROGRESS_STEP_BYTES (128u * 1024u) /* 128 KiB */

/* STATIC, sized for the LARGEST size this sweep ever sends (512 B) -- this
 * app never goes near ALP_CC3501E_MAX_PAYLOAD (see THE 1024-BYTE TRAP), so
 * command-sweep's full-ceiling shared buffer would only be wasted RAM here.
 * Content is a fixed, non-zero pattern (0x5A, matching command-sweep's own
 * STREAM_WRITE fill) because this is a byte-count exercise, not a
 * data-integrity test -- what value is written does not affect the rate. */
static uint8_t g_bridge_buf[512];

typedef struct {
	uint32_t size;        /**< STREAM_WRITE payload size this result is for, bytes. */
	uint32_t bytes_total; /**< Total bytes sent in this size's window (== calls * size). */
	int64_t  elapsed_ms;  /**< The one window, one clock -- see the file header. */
	uint32_t call_count;  /**< STREAM_WRITE calls made to fill this window. */
} bridge_result_t;

/** Same discipline as socktp_report() above: every figure on its own line
 *  so bytes * 1000 / elapsed_ms == rate can be checked by hand. */
static void bridge_report(const bridge_result_t *r)
{
	printk("\n  --- size=%u B ---\n", (unsigned)r->size);
	printk("    total bytes sent (timed)  = %u\n", (unsigned)r->bytes_total);
	printk("    elapsed_ms (whole window) = %lld\n", (long long)r->elapsed_ms);
	if (r->elapsed_ms > 0) {
		uint32_t rate_bps =
		    (uint32_t)(((uint64_t)r->bytes_total * 1000u) / (uint64_t)r->elapsed_ms);
		printk("    rate = %u B/s (%u.%02u KB/s)\n",
		       rate_bps,
		       rate_bps / 1024u,
		       (rate_bps % 1024u) * 100u / 1024u);
	} else {
		printk("    rate = not derived -- elapsed_ms is 0\n");
	}
	printk("    STREAM_WRITE calls        = %u\n", (unsigned)r->call_count);
	printk("    mean bytes/call           = %u (== size -- STREAM_WRITE has no partial "
	       "writes)\n",
	       (r->call_count > 0u) ? (unsigned)(r->bytes_total / r->call_count) : 0u);
}

/** Sweep BRIDGE_SIZES ascending, each over its own BRIDGE_BYTE_BUDGET
 *  window, reporting a size the INSTANT it completes -- so a wedge partway
 *  through (512 B is untested, see the file header) still leaves every
 *  completed size's real numbers on the console rather than losing them
 *  with the run. On the first STREAM_WRITE failure this stops immediately
 *  -- it does NOT try the next size against a link that may now be dead --
 *  and returns the count of sizes it actually completed; @p results holds
 *  exactly that many entries. Nothing downstream of this function can ever
 *  see a size that did not really finish its window: a failed size is
 *  never written into @p results at all. */
static unsigned socktp_bridge_sweep(cc3501e_t *fw, bridge_result_t *results)
{
	unsigned completed = 0u;

	memset(g_bridge_buf, 0x5A, sizeof(g_bridge_buf));

	for (size_t i = 0u; i < ARRAY_SIZE(BRIDGE_SIZES); i++) {
		uint32_t size = BRIDGE_SIZES[i];
		printk("\n--- STREAM_WRITE bridge sweep: size=%u B, window=%u KiB ---\n",
		       (unsigned)size,
		       (unsigned)(BRIDGE_BYTE_BUDGET / 1024u));

		bridge_result_t r             = { .size = size };
		uint32_t        next_progress = BRIDGE_PROGRESS_STEP_BYTES;
		bool            failed        = false;
		int64_t         t0            = k_uptime_get();

		while (r.bytes_total < BRIDGE_BYTE_BUDGET) {
			alp_status_t rc = cc3501e_stream_write(fw, g_bridge_buf, size);
			if (rc != ALP_OK) {
				printk("  ** STREAM_WRITE failed at size=%u B (rc=%d) after %u successful "
				       "call%s (%u bytes). The link may now be wedged -- see the file "
				       "header's THE 1024-BYTE TRAP. Stopping the sweep here; NOT "
				       "attempting the next size. **\n",
				       (unsigned)size,
				       (int)rc,
				       (unsigned)r.call_count,
				       (r.call_count == 1u) ? "" : "s",
				       (unsigned)r.bytes_total);
				failed = true;
				break;
			}
			r.bytes_total += size;
			r.call_count++;
			if (r.bytes_total >= next_progress) {
				printk("  progress: %u KiB / %u KiB\n",
				       (unsigned)(r.bytes_total / 1024u),
				       (unsigned)(BRIDGE_BYTE_BUDGET / 1024u));
				next_progress += BRIDGE_PROGRESS_STEP_BYTES;
			}
		}
		if (failed) {
			break;
		}
		r.elapsed_ms         = k_uptime_get() - t0;
		results[completed++] = r;
		bridge_report(&r);
	}
	return completed;
}

/** Answers the file header's "does the rate curve flatten?" question the
 *  original artifact-only sweep could not: prints every completed size's
 *  rate, then the size-to-size GROWTH (not the raw rates) -- a genuinely
 *  flattening curve shows each step's gain smaller than the last, per-frame
 *  overhead already mostly amortized by the time size doubles again. Needs
 *  at least two completed sizes to say anything; a sweep that failed on its
 *  very first size says so instead of guessing. */
static void bridge_flatten_report(const bridge_result_t *results, unsigned count)
{
	printk("\n  --- rate curve across %u completed size%s ---\n", count, (count == 1u) ? "" : "s");
	if (count < 2u) {
		printk("    fewer than two sizes completed -- cannot say whether the curve "
		       "flattens\n");
		return;
	}

	uint32_t prev_rate  = 0u;
	int32_t  prev_delta = 0;
	bool     flattening = true;
	for (unsigned i = 0u; i < count; i++) {
		uint32_t rate_bps = (results[i].elapsed_ms > 0)
		                        ? (uint32_t)(((uint64_t)results[i].bytes_total * 1000u) /
		                                     (uint64_t)results[i].elapsed_ms)
		                        : 0u;
		printk("    size=%4u B -> %u B/s (%u.%02u KB/s)\n",
		       (unsigned)results[i].size,
		       rate_bps,
		       rate_bps / 1024u,
		       (rate_bps % 1024u) * 100u / 1024u);
		if (i > 0u) {
			int32_t delta = (int32_t)rate_bps - (int32_t)prev_rate;
			if (i > 1u && delta > prev_delta) {
				flattening = false;
			}
			prev_delta = delta;
		}
		prev_rate = rate_bps;
	}
	printk("  verdict: the rate curve %s as size grows (per-step gains %s across the "
	       "completed sizes) -- this is the question the original artifact-only sweep "
	       "could not answer; see this file's WHY THIS APP EXISTS.\n",
	       flattening ? "FLATTENS" : "does NOT flatten",
	       flattening ? "shrink" : "do not shrink monotonically");
}

/* ---------------------------------------------------------------------- */

int main(void)
{
	printk("\n=== AEN801 CC3501E socket throughput ===\n");

	/*
	 * STATIC, explicitly zero-initialised cc3501e_t -- same discipline as
	 * every aen-cc3501e-* throughput sibling, for the identical reason:
	 * this type embeds several ALP_CC3501E_MAX_PAYLOAD scratch buffers
	 * (~32 KB total). Declared automatic it asks main() for a
	 * multi-kilobyte stack frame, and Zephyr's stack-overflow check fires
	 * on the very first `sub sp` -- before this function's own first
	 * printk, so not even the header line escapes. Measured on the M55-HE
	 * 2026-09-10 by aen-cc3501e-handshake-probe; this app inherits the
	 * same fix rather than rediscovering the same failure. g_recv_buf
	 * above is STATIC for the same reason.
	 */
	static cc3501e_t fw = { 0 };

	/*
	 * ---------------------------------------------------------------
	 * STEP 1 -- bring the bridge up, byte-identical to aen-evk-demo
	 * phase 8 and every aen-cc3501e-* sibling (same src/cc3501e_bridge.{c,h}
	 * template, same overlay).
	 * ---------------------------------------------------------------
	 */
	alp_status_t rc = cc3501e_bridge_bringup(&fw);
	printk("STEP 1: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ %u Hz) -> %d\n",
	       (unsigned)CC3501E_BRIDGE_SPI_FREQ_HZ,
	       (int)rc);
	if (rc != ALP_OK) {
		printk("  ** bring-up did NOT return ALP_OK -- nothing downstream of it can work. "
		       "Stopping here. **\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 2 -- confirm the link answers before spending a Wi-Fi connect
	 * budget on it. Same retry shape as aen-cc3501e-companion-tour's
	 * tour_ping().
	 * ---------------------------------------------------------------
	 */
	bool up = false;
	for (unsigned i = 0u; i < SOCKTP_PING_RETRIES; i++) {
		if (cc3501e_ping(&fw) == ALP_OK) {
			printk("STEP 2: PING ok after %u attempt%s\n", i + 1u, (i == 0u) ? "" : "s");
			up = true;
			break;
		}
		k_msleep(SOCKTP_PING_GAP_MS);
	}
	if (!up) {
		printk("STEP 2: PING never answered -- check WIFI_EN power, the SPI1 pinmux, and "
		       "that the CC3501E is running its firmware. Stopping here. **\n");
		return 0;
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 3 -- Wi-Fi association. Skipped entirely when no credentials
	 * are configured -- see CREDENTIALS in the file header.
	 * ---------------------------------------------------------------
	 */
	if (SOCKTP_WIFI_SSID[0] == '\0') {
		/*
		 * BRIDGE-ONLY MODE -- see the file header's BRIDGE-ONLY MODE
		 * section for the full reasoning. No credentials means no
		 * end-to-end path exists to measure anyway, so this app spends
		 * the same idle time STEP 3 used to just return from measuring
		 * the one segment that IS reachable without them: the raw
		 * host<->CC3501E SPI link via STREAM_WRITE, no radio involved.
		 */
		printk("STEP 3: WIFI_CONNECT skipped (SOCKTP_WIFI_SSID empty -- set it at build "
		       "time, see this file's header, for an end-to-end run instead). Selecting "
		       "BRIDGE-ONLY mode: measuring the host<->CC3501E SPI link alone via "
		       "STREAM_WRITE. This figure is NOT comparable to an end-to-end HTTP "
		       "figure -- see the file header's NOT COMPARABLE TO END-TO-END note.\n");

		static bridge_result_t bridge_results[ARRAY_SIZE(BRIDGE_SIZES)];
		unsigned               completed = socktp_bridge_sweep(&fw, bridge_results);
		bridge_flatten_report(bridge_results, completed);
		if (completed < ARRAY_SIZE(BRIDGE_SIZES)) {
			printk("\n  ** sweep stopped early: %u/%u sizes completed. The sizes that did "
			       "NOT complete are reported nowhere above -- they are not data. **\n",
			       completed,
			       (unsigned)ARRAY_SIZE(BRIDGE_SIZES));
		}
		return 0;
	}

	printk("STEP 3: WIFI_CONNECT -> SSID \"%s\" (sec %u)...\n",
	       SOCKTP_WIFI_SSID,
	       (unsigned)SOCKTP_WIFI_SECURITY);
	rc = cc3501e_wifi_connect(&fw,
	                          SOCKTP_WIFI_SSID,
	                          (uint8_t)SOCKTP_WIFI_SECURITY,
	                          SOCKTP_WIFI_PASS,
	                          SOCKTP_CONNECT_TIMEOUT_MS);
	if (rc != ALP_OK) {
		/* A non-OK return here does NOT mean the radio failed to associate --
		 * it can equally mean the HOST gave up on its own timeout_ms budget
		 * while the association was still genuinely running (this is exactly
		 * what a host-side accounting bug in cc3501e_wifi_connect() looks
		 * like from this app: no distinguishing evidence, just ALP_ERR_TIMEOUT).
		 * Wait out the firmware's own worst-case connect budgets, then read
		 * the independent, non-blocking WIFI_STATUS latch: CONNECTED means
		 * the association actually succeeded and only the host's accounting
		 * was wrong; CONN_FAILED/FAIL_REJECTED (or FAIL_KICK) means a real
		 * credentials/security-type/role failure; CONN_FAILED/FAIL_TIMEOUT
		 * means the firmware's own 30 s L2 wait or 20 s DHCP budget expired
		 * for real. */
		printk("STEP 3: WIFI_CONNECT -> %d (host accounting gave up, or the radio genuinely "
		       "failed -- can't tell yet). Waiting %u ms for the firmware's own connect "
		       "budgets to resolve, then reading the real radio verdict off WIFI_STATUS...\n",
		       (int)rc,
		       (unsigned)SOCKTP_VERDICT_WAIT_MS);
		k_msleep(SOCKTP_VERDICT_WAIT_MS);
		alp_cc3501e_wifi_status_t verdict = { 0 };
		alp_status_t              vr      = cc3501e_wifi_status(&fw, &verdict);
		if (vr == ALP_OK) {
			printk("STEP 3: WIFI_STATUS verdict -- state=%u fail_reason=%u "
			       "(CONNECTED=%u: radio associated, host accounting was wrong; "
			       "CONN_FAILED=%u + FAIL_TIMEOUT=%u: firmware's own budget expired; "
			       "CONN_FAILED + any other fail_reason: real credential/security/role "
			       "rejection). Stopping here.\n",
			       (unsigned)verdict.state,
			       (unsigned)verdict.fail_reason,
			       (unsigned)ALP_CC3501E_WIFI_CONNECTED,
			       (unsigned)ALP_CC3501E_WIFI_CONN_FAILED,
			       (unsigned)ALP_CC3501E_WIFI_FAIL_TIMEOUT);
		} else {
			printk("STEP 3: WIFI_STATUS verdict read failed too (rc=%d) -- state/fail_reason "
			       "UNREAD, the real radio outcome stays unknown. Stopping here.\n",
			       (int)vr);
		}
		return 0;
	}
	uint8_t ip[4] = { 0 };
	if (cc3501e_wifi_get_ip(&fw, (uint8_t)ALP_CC3501E_WIFI_IFACE_STA, ip) == ALP_OK) {
		printk("STEP 3: associated, IP -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
	} else {
		printk("STEP 3: associated, IP -> not leased yet\n");
	}

	/*
	 * ---------------------------------------------------------------
	 * STEP 4 -- open a TCP socket, connect it, send the GET, then read
	 * the response BODY in a loop until it completes or the byte budget
	 * is reached. Everything from here down is the measurement itself.
	 * ---------------------------------------------------------------
	 */
	uint16_t sock = 0u;
	rc            = cc3501e_sock_open(&fw,
	                                  ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                  ALP_CC3501E_SOCK_TYPE_STREAM,
	                                  0u /* default proto = TCP */,
	                                  &sock,
	                                  SOCKTP_SOCK_TIMEOUT_MS);
	if (rc != ALP_OK) {
		printk("STEP 4: SOCK_OPEN -> %d. Stopping here.\n", (int)rc);
		(void)cc3501e_wifi_disconnect(&fw);
		return 0;
	}
	printk("STEP 4: SOCK_OPEN -> handle 0x%04x\n", sock);

	rc = cc3501e_sock_connect(
	    &fw, sock, SOCKTP_SERVER_IP, (uint16_t)SOCKTP_SERVER_PORT, SOCKTP_SOCK_TIMEOUT_MS);
	if (rc != ALP_OK) {
		printk("STEP 4: SOCK_CONNECT -> %d (%u.%u.%u.%u:%u unreachable? server down?). "
		       "Stopping here.\n",
		       (int)rc,
		       SOCKTP_SERVER_IP[0],
		       SOCKTP_SERVER_IP[1],
		       SOCKTP_SERVER_IP[2],
		       SOCKTP_SERVER_IP[3],
		       (unsigned)SOCKTP_SERVER_PORT);
		(void)cc3501e_sock_close(&fw, sock, SOCKTP_SOCK_TIMEOUT_MS);
		(void)cc3501e_wifi_disconnect(&fw);
		return 0;
	}
	printk("STEP 4: SOCK_CONNECT -> %u.%u.%u.%u:%u\n",
	       SOCKTP_SERVER_IP[0],
	       SOCKTP_SERVER_IP[1],
	       SOCKTP_SERVER_IP[2],
	       SOCKTP_SERVER_IP[3],
	       (unsigned)SOCKTP_SERVER_PORT);

	size_t sent = 0u;
	rc          = cc3501e_sock_send(&fw,
	                                sock,
	                                (const uint8_t *)SOCKTP_HTTP_REQUEST,
	                                sizeof(SOCKTP_HTTP_REQUEST) - 1u,
	                                &sent,
	                                SOCKTP_SOCK_TIMEOUT_MS);
	printk("STEP 4: SOCK_SEND -> %d (%u/%u bytes queued: \"%s\")\n",
	       (int)rc,
	       (unsigned)sent,
	       (unsigned)(sizeof(SOCKTP_HTTP_REQUEST) - 1u),
	       SOCKTP_HTTP_PATH);

	/* ---- The measured window itself ---- */
	unsigned hdr_match      = 0u;
	bool     hdr_done       = false;
	uint32_t wire_bytes     = 0u; /* everything off the socket, header included */
	uint32_t body_bytes     = 0u; /* body only -- what the rate is derived from */
	uint32_t recv_calls     = 0u;
	uint32_t body_calls     = 0u;
	uint32_t zero_streak    = 0u;
	uint32_t next_progress  = SOCKTP_PROGRESS_STEP_BYTES;
	int64_t  t_start        = 0; /* set on the first BODY byte, not on connect/send */
	int64_t  t_session_open = k_uptime_get();

	if (rc == ALP_OK) {
		for (;;) {
			if (body_bytes >= SOCKTP_BYTE_BUDGET) {
				printk("  byte budget (%u B) reached -- stopping the read loop\n",
				       (unsigned)SOCKTP_BYTE_BUDGET);
				break;
			}
			if ((k_uptime_get() - t_session_open) > (int64_t)SOCKTP_SESSION_TIMEOUT_MS) {
				printk("  ** session safety timeout (%u ms) hit before the byte budget "
				       "or a clean close -- treating this as a stalled link, not a "
				       "result **\n",
				       (unsigned)SOCKTP_SESSION_TIMEOUT_MS);
				break;
			}

			size_t got = 0u;
			rc         = cc3501e_sock_recv(
			    &fw, sock, g_recv_buf, sizeof(g_recv_buf), &got, SOCKTP_RECV_TIMEOUT_MS);
			recv_calls++;
			if (rc != ALP_OK) {
				printk("  SOCK_RECV -> %d -- stopping the read loop\n", (int)rc);
				break;
			}
			if (got == 0u) {
				zero_streak++;
				if (zero_streak >= SOCKTP_ZERO_STREAK_DONE) {
					printk("  %u consecutive empty SOCK_RECV results -- treating the "
					       "transfer as complete\n",
					       (unsigned)zero_streak);
					break;
				}
				continue;
			}
			zero_streak = 0u;
			wire_bytes += (uint32_t)got;

			size_t body_off = 0u;
			if (!hdr_done) {
				bool found = false;
				body_off   = http_skip_header(g_recv_buf, got, &hdr_match, &found);
				if (found) {
					hdr_done = true;
				}
			}
			size_t chunk_body = (body_off < got) ? (got - body_off) : 0u;
			if (chunk_body > 0u) {
				if (t_start == 0) {
					/* Clock starts on the first BODY byte -- see the file
					 * header's note citing cc3501e_hw_ti_sock.c's own
					 * radio-speedtest reasoning for why. */
					t_start = k_uptime_get();
				}
				body_bytes += (uint32_t)chunk_body;
				body_calls++;

				if (body_bytes >= next_progress) {
					int64_t  dt = k_uptime_get() - t_start;
					uint32_t rate_bps =
					    (dt > 0) ? (uint32_t)(((uint64_t)body_bytes * 1000u) / (uint64_t)dt) : 0u;
					printk("  progress: %u KiB, running rate %u B/s (%u.%02u KB/s)\n",
					       (unsigned)(body_bytes / 1024u),
					       rate_bps,
					       rate_bps / 1024u,
					       (rate_bps % 1024u) * 100u / 1024u);
					next_progress += SOCKTP_PROGRESS_STEP_BYTES;
				}
			}
		}
	}
	int64_t elapsed_ms = (t_start != 0) ? (k_uptime_get() - t_start) : 0;

	(void)cc3501e_sock_close(&fw, sock, SOCKTP_SOCK_TIMEOUT_MS);
	printk("STEP 4: SOCK_CLOSE done\n");
	(void)cc3501e_wifi_disconnect(&fw);
	printk("STEP 4: WIFI_DISCONNECT done\n");

	socktp_report(body_bytes, wire_bytes, elapsed_ms, recv_calls, body_calls);
	return 0;
}
