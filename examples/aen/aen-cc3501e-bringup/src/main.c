/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-bringup -- power, reset, and PING the on-module TI CC3501E
 * Wi-Fi 6 + BLE 5.4 coprocessor from the Alif Ensemble E8 host (M55-HE).
 *
 * This is the *Alif (host) side* of the inter-chip bring-up; its peer is
 * the ALP-authored firmware that runs on the CC3501E's own Cortex-M33
 * (firmware/cc3501e/, embedded in this repo per ADR 0015 -- like the
 * gd32-bridge).  It is the AEN sibling of
 * examples/v2n/v2n-gd32-bridge-ping: same shape (open the link, retry
 * until the coprocessor answers, then soak so the link stays
 * continuously verifiable over J-Link), different coprocessor.
 *
 * WHY THIS APP EXISTS: the CC3501E's supply is *host-gated* -- it has no
 * power until the Alif drives WIFI_EN high.  So a J-Link cannot even
 * attach to the CC3501E (VTref reads 0 V) until this app runs.  Running
 * it is therefore the gating step for the very first on-silicon PING
 * (and it validates the firmware's reply-arming framing, the one fix
 * that can't be checked off-silicon).
 *
 * Wiring -- all host-driven, from metadata/e1m_modules/aen/inter-chip.tsv:
 *
 *   net          Alif pad       direction   CC3501E pad
 *   ----------   ------------   ---------   ------------------------
 *   WIFI_EN      P15_5          out         (supply gate)
 *   E_WIFI.NRST  P15_1_FLEX     out         (reset)
 *   SPI1.SCK     P14_6          out         GPIO_27  (CC35 SPI0 slave)
 *   SPI1.MOSI    P14_5          out         GPIO_28
 *   SPI1.MISO    P14_4          in          GPIO_29
 *   SPI1.SS0     P14_7          out         CC35 SPI0 CSN
 *   READY        P2_6           in          GPIO_17
 *
 * The current E1M-AEN rev uses the dwc-ssi hardware SS0 chip-select on
 * P14_7 and a READY input on P2_6.  Each protocol phase is framed by SS0;
 * READY tells the host when the slave has re-armed for the next phase.  The
 * framing lives in the host driver (chips/cc3501e/cc3501e.c) and its mirror
 * on the firmware side (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c).
 * This app just opens the bus and calls the driver.
 *
 * This file is ~50 % comment by design: examples are documentation for
 * hand-written firmware, not just runnable code.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/fatal.h>
#include <zephyr/kernel.h> /* k_cycle_get_32 / k_cyc_to_us_floor32 for the DMA stream benchmark */

#include "alp/peripheral.h"
#include "alp/chips/cc3501e.h"

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/*
 * alp,pin-array positional indices for the two SoM-internal CC3501E
 * control nets (declared in boards/<board>.overlay).
 *
 * NOTE: these are NOT E1M edge pads.  WIFI_EN and E_WIFI.NRST are
 * internal Alif<->CC3501E control lines (inter-chip.tsv), so this
 * bring-up app defines its OWN compact 2-entry control array rather than
 * the 52-slot positional E1M map the portable IO examples use.  The
 * indices below must match the order of the `gpios` entries in both the
 * native_sim and the AEN board overlays.
 */
#define CC3501E_PIN_WIFI_EN 0u /* Alif P15_5      -- CC3501E supply gate */
#define CC3501E_PIN_NRST    1u /* Alif P15_1_FLEX -- CC3501E reset       */

/*
 * Inter-chip SPI bus.  bus_id 1 resolves through the `alp-spi1` devicetree
 * alias (the overlay points it at the Alif SPI1 controller on P14_6/5/4).
 * The Alif is master; the CC3501E is the SPI slave.
 */
#define CC3501E_SPI_BUS_ID 1u

/* How long to keep retrying the first PING before falling through to the
 * soak loop anyway (the soak loop keeps logging, so a console-attached
 * run still shows whether the link ever comes up). */
#define CC3501E_PING_RETRIES 25u
#define CC3501E_PING_GAP_MS  200u

/* Poll-by-repeat budgets for the Wi-Fi helpers (firmware kicks off a worker
 * and answers BUSY until it finishes; the host re-issues until OK/timeout). */
#define CC3501E_MAC_TIMEOUT_MS  2000u
#define CC3501E_SCAN_TIMEOUT_MS 8000u
#define CC3501E_CONN_TIMEOUT_MS 15000u

/* Max scan records to collect into the witness-backed array. */
#define CC3501E_SCAN_MAX_RECORDS 16u

/*
 * Wi-Fi STA credentials for the optional CONNECT step.  DELIBERATELY EMPTY
 * by default -- do NOT hardcode bench credentials in a public example.  Set
 * them at build time without editing this file, e.g.:
 *
 *   west build ... -- \
 *     -DCONFIG_... is not used (these are plain C macros); pass via CFLAGS:
 *   west build ... -- -DEXTRA_CFLAGS="-DCC3501E_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DCC3501E_WIFI_PASS=\\\"mypass\\\""
 *
 * or simply edit these two lines locally on the bench (never commit them).
 * When CC3501E_WIFI_SSID is empty the CONNECT call is skipped entirely.
 */
#ifndef CC3501E_WIFI_SSID
#define CC3501E_WIFI_SSID ""
#endif
#ifndef CC3501E_WIFI_PASS
#define CC3501E_WIFI_PASS ""
#endif
/* Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (alp_cc3501e_wifi_connect_t). */
#ifndef CC3501E_WIFI_SECURITY
#define CC3501E_WIFI_SECURITY 1u
#endif

/*
 * SWD-readable bring-up witness.
 *
 * The AEN carrier console (uart5) may not be broken out on every bench, so
 * this struct mirrors the PING result into RAM where a J-Link can read it
 * with no console attached (the gd32-bridge-ping trick).  Find its address
 * in zephyr.map (symbol `g_cc3501e_witness`), then over J-Link:
 *   mem32 <addr> 8     -- magic should read 0x35334343 ("CC35" LE) once
 *                         main() runs; ping_ok increments while the link is
 *                         up; last_status / version are the latest results.
 * `used` keeps it through --gc-sections; volatile stops the compiler from
 * optimising the stores away (nothing in this TU reads the fields back). */
typedef struct {
	uint32_t magic;        /* +0x00  0x35334343 once main() starts            */
	uint32_t reset_status; /* +0x04  (uint32_t)alp_status_t from cc3501e_reset */
	uint32_t ping_ok;      /* +0x08  count of successful PINGs                 */
	uint32_t ping_fail;    /* +0x0C  count of failed PINGs                     */
	uint32_t last_status;  /* +0x10  (uint32_t)alp_status_t of the last PING   */
	uint32_t version;      /* +0x14  protocol version (low 16b) | status<<16   */
	uint32_t phase;        /* +0x18  progress checkpoint (see CC3501E_PHASE_*)  */
	/* --- Wi-Fi bring-up results (cc3501e_wifi_* helpers) --- */
	uint32_t mac_status;  /* +0x1C  (uint32_t)alp_status_t from cc3501e_wifi_get_mac    */
	uint32_t mac_ok;      /* +0x20  1 once a 6-byte MAC was read; 0 otherwise           */
	uint32_t mac_lo;      /* +0x24  MAC bytes [0..3] packed LE (mac[0] in bits 7:0)     */
	uint32_t mac_hi;      /* +0x28  MAC bytes [4..5] in bits 15:0 (mac[4] in bits 7:0)  */
	uint32_t scan_status; /* +0x2C  (uint32_t)alp_status_t from cc3501e_wifi_scan       */
	uint32_t scan_count;  /* +0x30  number of scan records parsed                       */
	int32_t
	    scan_first_rssi; /* +0x34 RSSI dBm of the first scan record (sign-extended); 0 if none */
	/* --- BLE bring-up results (cc3501e_ble_* helpers) --- */
	uint32_t ble_status;  /* +0x38  (uint32_t)alp_status_t from cc3501e_ble_enable        */
	uint32_t ble_enabled; /* +0x3C  1 once the BLE controller + NimBLE host came up        */
	/* --- host peripheral-DMA continuous-stream throughput benchmark --- */
	uint32_t dma_stream_iters; /* +0x40  large TX-DMA transfers completed in the burst     */
	uint32_t dma_stream_us;    /* +0x44  elapsed microseconds for the burst                */
	uint32_t dma_stream_kbps;  /* +0x48  measured throughput, KB/s (bytes*1000/us)         */
} cc3501e_witness_t;

/* Progress checkpoints written to g_cc3501e_witness.phase so a J-Link can
 * localise where the app got to (read after a fault: .bss survives a halt).
 * 1=entered main, 2=GPIOs configured, 3=SPI opened, 4=reset done,
 * 5=in PING-retry loop, 6=version read, 7=Wi-Fi probes, 8=in soak loop. */
#define CC3501E_PHASE_MAIN     1u
#define CC3501E_PHASE_GPIO     2u
#define CC3501E_PHASE_SPI_OPEN 3u
#define CC3501E_PHASE_RESET    4u
#define CC3501E_PHASE_PING     5u
#define CC3501E_PHASE_VERSION  6u
#define CC3501E_PHASE_WIFI     7u
#define CC3501E_PHASE_SOAK     8u

#define CC3501E_WITNESS_MAGIC 0x35334343u /* "CC35" little-endian */

volatile cc3501e_witness_t g_cc3501e_witness __attribute__((used));

/* The CC3501E SoM bring-up (control pins + inter-chip SPI + reset, incl. the AEN
 * LP-pad mux) lives in cc3501e_bridge_bringup() (cc3501e_bridge.{c,h}). */

/* PING (META opcode 0x00) uses the public cc3501e_ping() from <alp/chips/cc3501e.h>
 * -- a bare liveness probe: ALP_OK means the coprocessor parsed the frame and
 * answered over the hardware-framed bridge. (This example previously carried
 * a local copy; it now
 * uses the SDK's.) */

/* Pretty-print the extended diagnostics block (META opcode 0x04).
 *
 * GET_DIAG_INFO is a v2-firmware feature: v0.1 firmware rejects it with
 * ALP_CC3501E_RESP_ERR_INVALID (surfaced here as ALP_ERR_INVAL), which is
 * the EXPECTED answer during this bring-up -- it still proves the request
 * round-trips and the error path is wired.  Once v2 firmware lands, the
 * same call decodes the 16-byte alp_cc3501e_diag_info_t. */
static void cc3501e_dump_diag(cc3501e_t *fw)
{
	/* Decode via the driver's field-by-field API rather than memcpy'ing
	 * the wire bytes over the struct: alp_cc3501e_diag_info_t is a
	 * wire-SCHEMA description (#733), and cc3501e_diag_info() is the codec
	 * that reads each little-endian field explicitly -- no padding
	 * assumptions, no short-reply foot-gun (it validates the length). */
	alp_cc3501e_diag_info_t diag;
	alp_status_t            s = cc3501e_diag_info(fw, &diag);

	if (s == ALP_ERR_INVAL) {
		printf("[cc3501e-bringup] GET_DIAG_INFO -> rejected (v0.1 firmware; "
		       "v2-only command) -- expected\n");
		return;
	}
	if (s != ALP_OK) {
		printf("[cc3501e-bringup] GET_DIAG_INFO -> %d\n", (int)s);
		return;
	}

	printf("[cc3501e-bringup] diag: fw_version=0x%04x reset_cause=%u role=%u "
	       "uptime=%u ms free_heap=%u B last_error=%u\n",
	       diag.fw_version,
	       diag.reset_cause,
	       diag.role,
	       diag.uptime_ms,
	       diag.free_heap_bytes,
	       diag.last_error);
}

/*
 * Drive the Wi-Fi control path once the link is proven (PING/VERSION).
 *
 * This is the point of the bring-up beyond "the link answers": it exercises
 * the firmware's Wi-Fi worker seam from the host -- GET_MAC and SCAN_START
 * are poll-by-repeat (the firmware answers BUSY while a worker runs, the host
 * driver re-issues until OK), so a successful MAC read / scan proves the whole
 * submit -> worker -> reply round-trip, not just META dispatch.  Results are
 * mirrored into the witness so a J-Link reads them with no console.
 *
 * CONNECT is wired but only attempted when CC3501E_WIFI_SSID is non-empty
 * (set at build time on the bench -- never hardcode credentials here).
 */
/*
 * Bench-only network probe: proves the associated link actually carries IP
 * traffic, and measures end-to-end throughput (Wi-Fi + the SPI bridge, which
 * is what a product actually sees).  Both targets are build-time settings so
 * no address is baked into the public example; the step is skipped when
 * CC3501E_SPEEDTEST_IP0 is 0.
 */
#ifndef CC3501E_SPEEDTEST_IP0
#define CC3501E_SPEEDTEST_IP0 0u
#define CC3501E_SPEEDTEST_IP1 0u
#define CC3501E_SPEEDTEST_IP2 0u
#define CC3501E_SPEEDTEST_IP3 0u
#endif
#ifndef CC3501E_SPEEDTEST_PORT
#define CC3501E_SPEEDTEST_PORT 8080u
#endif
#ifndef CC3501E_SPEEDTEST_PATH
#define CC3501E_SPEEDTEST_PATH "/speed.bin"
#endif

static void cc3501e_net_probe(cc3501e_t *fw)
{
	static uint8_t rx[512];

	/* 1) Throughput -- drain a file from a local HTTP server over the link. */
	if ((unsigned)CC3501E_SPEEDTEST_IP0 != 0u) {
		uint16_t      h     = 0u;
		const uint8_t ip[4] = { (uint8_t)CC3501E_SPEEDTEST_IP0,
			                    (uint8_t)CC3501E_SPEEDTEST_IP1,
			                    (uint8_t)CC3501E_SPEEDTEST_IP2,
			                    (uint8_t)CC3501E_SPEEDTEST_IP3 };
		if (cc3501e_sock_open(
		        fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &h, 5000u) ==
		        ALP_OK &&
		    cc3501e_sock_connect(fw, h, ip, (uint16_t)CC3501E_SPEEDTEST_PORT, 10000u) == ALP_OK) {
			static const char req[] = "GET " CC3501E_SPEEDTEST_PATH " HTTP/1.0\r\n\r\n";
			size_t            sent  = 0u;
			(void)cc3501e_sock_send(fw, h, (const uint8_t *)req, sizeof(req) - 1u, &sent, 10000u);
			const int64_t t0     = k_uptime_get();
			uint32_t      total  = 0u;
			uint8_t       misses = 0u;
			for (;;) {
				size_t             got = 0u;
				const alp_status_t rs  = cc3501e_sock_recv(fw, h, rx, sizeof(rx), &got, 2000u);
				/* A gap is NOT end-of-stream.  cc3501e_sock_recv() polls the firmware
				 * and returns non-OK when nothing is buffered YET, so breaking on the
				 * first miss truncated every transfer that needed more than one frame
				 * (a 600 B body stopped at 193 B).  Tolerate a bounded run of empty
				 * reads before declaring the stream finished. */
				if (misses == 0u && rs != ALP_OK) {
					printf("[cc3501e-bringup] NET first-miss rc=%d after %u B\n",
					       (int)rs,
					       (unsigned)total);
				}
				if (rs != ALP_OK || got == 0u) {
					if (++misses >= 40u) {
						break;
					}
					continue;
				}
				misses = 0u;
				total += (uint32_t)got;
				if (total >= 262144u) {
					break; /* 256 KiB is plenty for a rate */
				}
			}
			const uint32_t ms = (uint32_t)(k_uptime_get() - t0);
			printf("[cc3501e-bringup] NET THROUGHPUT %u B in %u ms = %u B/s\n",
			       (unsigned)total,
			       (unsigned)ms,
			       (unsigned)((ms > 0u) ? ((uint64_t)total * 1000u / ms) : 0u));
		}
		(void)cc3501e_sock_close(fw, h, 5000u);
	}
	/* 2) Internet reachability -- 1.1.1.1:80, no DNS needed. */
	{
		uint16_t           h     = 0u;
		const uint8_t      ip[4] = { 1u, 1u, 1u, 1u };
		const alp_status_t os    = cc3501e_sock_open(
		    fw, ALP_CC3501E_SOCK_FAMILY_IPV4, ALP_CC3501E_SOCK_TYPE_STREAM, 0u, &h, 5000u);
		if (os != ALP_OK) {
			printf("[cc3501e-bringup] NET open -> %d\n", (int)os);
		} else {
			const alp_status_t ks = cc3501e_sock_connect(fw, h, ip, 80u, 10000u);
			printf("[cc3501e-bringup] NET connect 1.1.1.1:80 -> %d\n", (int)ks);
			if (ks == ALP_OK) {
				static const char req[] = "GET / HTTP/1.0\r\nHost: one.one.one.one\r\n\r\n";
				size_t            sent  = 0u;
				(void)cc3501e_sock_send(
				    fw, h, (const uint8_t *)req, sizeof(req) - 1u, &sent, 10000u);
				size_t             got = 0u;
				const alp_status_t rs  = cc3501e_sock_recv(fw, h, rx, sizeof(rx), &got, 10000u);
				if (rs == ALP_OK && got >= 12u) {
					rx[11] = (uint8_t)0;
					printf("[cc3501e-bringup] NET INTERNET OK -- %u B, reply starts: %s\n",
					       (unsigned)got,
					       (const char *)rx);
				} else {
					printf("[cc3501e-bringup] NET recv -> %d (%u B)\n", (int)rs, (unsigned)got);
				}
			}
			(void)cc3501e_sock_close(fw, h, 5000u);
		}
	}
}
static void cc3501e_wifi_probe(cc3501e_t *fw)
{
	g_cc3501e_witness.phase = CC3501E_PHASE_WIFI;

	/* --- MAC (poll-by-repeat; proves the worker seam) --- */
	uint8_t      mac[CC3501E_MAC_LEN] = { 0 };
	alp_status_t ms                   = cc3501e_wifi_get_mac(fw, mac, CC3501E_MAC_TIMEOUT_MS);
	g_cc3501e_witness.mac_status      = (uint32_t)ms;
	if (ms == ALP_OK) {
		g_cc3501e_witness.mac_ok = 1u;
		g_cc3501e_witness.mac_lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
		                           ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
		g_cc3501e_witness.mac_hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
		printf("[cc3501e-bringup] GET_MAC -> %02x:%02x:%02x:%02x:%02x:%02x\n",
		       mac[0],
		       mac[1],
		       mac[2],
		       mac[3],
		       mac[4],
		       mac[5]);
	} else {
		printf("[cc3501e-bringup] GET_MAC -> %d (worker seam not up yet?)\n", (int)ms);
	}

	/* --- SCAN (poll-by-repeat; collects packed records) --- */
	static cc3501e_scan_record_t scan[CC3501E_SCAN_MAX_RECORDS];
	size_t                       n = 0;
	alp_status_t                 ss =
	    cc3501e_wifi_scan(fw, scan, CC3501E_SCAN_MAX_RECORDS, &n, CC3501E_SCAN_TIMEOUT_MS);
	g_cc3501e_witness.scan_status = (uint32_t)ss;
	if (ss == ALP_OK) {
		g_cc3501e_witness.scan_count      = (uint32_t)n;
		g_cc3501e_witness.scan_first_rssi = (n > 0u) ? (int32_t)scan[0].rssi_dbm : 0;
		printf("[cc3501e-bringup] WIFI_SCAN -> %u AP(s)\n", (unsigned)n);
		for (size_t i = 0; i < n; ++i) {
			printf("  [%u] \"%s\" ch%u %d dBm sec%u\n",
			       (unsigned)i,
			       scan[i].ssid,
			       scan[i].channel,
			       (int)scan[i].rssi_dbm,
			       (unsigned)scan[i].security_info);
		}
	} else {
		printf("[cc3501e-bringup] WIFI_SCAN -> %d\n", (int)ss);
	}

	/* --- CONNECT (opt-in; SSID set at build time, never hardcoded) --- */
	if (CC3501E_WIFI_SSID[0] != '\0') {
		printf("[cc3501e-bringup] WIFI_CONNECT_STA -> SSID \"%s\" (sec %u)...\n",
		       CC3501E_WIFI_SSID,
		       (unsigned)CC3501E_WIFI_SECURITY);
		alp_status_t cs = cc3501e_wifi_connect(fw,
		                                       CC3501E_WIFI_SSID,
		                                       (uint8_t)CC3501E_WIFI_SECURITY,
		                                       CC3501E_WIFI_PASS,
		                                       CC3501E_CONN_TIMEOUT_MS);
		printf("[cc3501e-bringup] WIFI_CONNECT_STA -> %d\n", (int)cs);
		if (cs == ALP_OK) {
			int8_t rssi = 0;
			if (cc3501e_wifi_rssi(fw, &rssi) == ALP_OK) {
				printf("[cc3501e-bringup] RSSI -> %d dBm\n", (int)rssi);
			}
			uint8_t ip[4] = { 0 };
			if (cc3501e_wifi_get_ip(fw, ip) == ALP_OK) {
				printf("[cc3501e-bringup] IP -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
			}
		}
	} else {
		printf("[cc3501e-bringup] WIFI_CONNECT_STA skipped (CC3501E_WIFI_SSID empty -- "
		       "set it at build time on the bench)\n");
	}
}

/*
 * Step 7.5 (opt-in) -- OTA firmware update to the CC3501E.
 *
 * Demonstrates the host-side OTA contract from <alp/chips/cc3501e.h>:
 *
 *   cc3501e_ota_update(fw, image, len, timeout)
 *       = cc3501e_ota_begin(len)            open a session on the CC35's
 *                                           NON-primary vendor slot
 *       + cc3501e_ota_write(off, chunk, n)  stream the image in page-aligned
 *                                           (256 B) chunks; the firmware RAM-
 *                                           stages each chunk (no flash yet)
 *       + cc3501e_ota_finish()              ONE flash burst: psa_fwu_start +
 *                                           write + install -> STAGED, then a
 *                                           deferred swap-reboot
 *
 * `cc3501e_ota_status()` can be polled at any point for the session state
 * (idle / writing / staged) and the running byte cursor.
 *
 * WHAT A REAL DEPLOYMENT PASSES: `image` must be a genuine SIGNED GPE vendor
 * image (manifest + body) built for the CC3501E -- the same artefact
 * firmware/cc3501e/ produces.  This demo has TWO payload modes:
 *
 *   default            -- a small INERT pattern.  It exercises the host
 *                         encode/stream/framing path end-to-end but is NOT a
 *                         valid image, so on real silicon the firmware's
 *                         psa_fwu_start rejects it at FINISH (an expected, safe
 *                         failure -- nothing gets staged).  Proves the wire
 *                         path only.
 *   -DCC3501E_OTA_REAL -- streams the genuine `cc3501e_ota_candidate[]` (a
 *                         signed GPE vendor image built for the CC3501E, at a
 *                         GPE version ABOVE the running primary so it is a
 *                         FORWARD update).  psa_fwu_start ACCEPTS it, FINISH
 *                         reaches STAGED (`state`=2), and the CC35's own
 *                         `psa_fwu_request_reboot()` swaps it in.  (A DOWNGRADE
 *                         candidate is refused at install, `state`=3/ERROR --
 *                         monotonic anti-rollback.)  Requires the CMake side to
 *                         compile the candidate source -- see CMakeLists.txt.
 *
 * BENCH REALITY (see firmware/cc3501e/BRINGUP_STATUS.md §5): the FULL cycle is
 * silicon-proven on the E1M-AEN801 EVK (2026-07-10) -- stream -> STAGED -> the
 * firmware's own swap-reboot (the bridge drops, then returns) -> the swapped
 * image self-accepts and PERSISTS across a true cold POR.  The swap is driven
 * by the CC35's `psa_fwu_request_reboot()` after FINISH, NOT a host cold POR.
 * `OTA_STATUS reserved[0]` (printed below as `reboot_rc`) surfaces the swap
 * result: 0 = success, non-zero = refused (e.g. a downgrade).  Opt in at build
 * time with `-DCC3501E_OTA_DEMO=ON` (inert blob) or `-DCC3501E_OTA_REAL=ON`
 * (genuine forward candidate); left OFF by default so a normal bring-up run
 * never kicks a (disruptive) flash cycle.
 */
#ifdef CC3501E_OTA_DEMO
#define CC3501E_OTA_DEMO_TIMEOUT_MS 20000u
/* #1610 BENCH: the streaming loop uses a SHORT per-request timeout so a dead
 * link is reported in seconds instead of silently eating 20 s per chunk. */
#define CC3501E_OTA_BENCH_TIMEOUT_MS 3000u
/* The first window flush also runs psa_fwu_start (manifest + slot prep), which
 * is far slower than a plain 4-block flush -- give it real room. */
/* The FIRST flush also runs psa_fwu_start, which prepares/erases the secondary
 * slot on the 8 MB QSPI part -- bench 2026-08-21 measured flush_pending still
 * set after a full 60 s with dev_state=1 (WRITING) and the bridge still
 * ANSWERING, i.e. healthy but slow.  Budget for the erase, not for a plain
 * 4-block flush. */
/* 600 s.  The FIRST flush is where psa_fwu_start consumes the manifest, and on
 * TI's flow that call does slot preparation of its own -- so a session pays a
 * prepare at BEGIN and AGAIN here (the manifest only arrives with the first
 * 16 KiB, so it cannot be merged).  Silicon 2026-08-21: at 300 s the device
 * still reported dev_state=1 (WRITING) dev_cursor=16384 flush=1 and answered
 * STATUS in 16 ms -- alive and mid-flush, just not finished. */
#define CC3501E_OTA_BENCH_FLUSH_WAIT_MS 600000u
#define CC3501E_OTA_BENCH_FLUSH_POLL_MS \
	1u /* was 50u: a 50 ms sleep per poll iteration, several per chunk, dominated the ~265 ms spent on each 256 B chunk (wire time ~0.14 ms) */
/* Cap each STATUS frame so the hold-off budget above is REAL.  Charging only the
 * sleep while cc3501e_ota_status blocked for CC3501E_OTA_BENCH_TIMEOUT_MS made
 * the nominal 60 s hold-off run for up to ~1 h, which is why the 2026-08-21 run
 * sat at off=16384 forever and printed nothing. */
#define CC3501E_OTA_BENCH_FLUSH_POLL_TIMEOUT_MS 200u

/* #1610 DISCRIMINATOR: set to 0 to run the OTA with NO preceding radio activity.
 * Five hypotheses have been refuted on silicon (slow erase, HwiP masking of the
 * erase walk, a 4 MB-vs-8 MB flash-map mismatch, priority inversion on
 * XMEMWFF3's writeMutex, and stack overflow in the manifest crypto) -- BEGIN
 * fails IDENTICALLY at 81 s, 181 s and 361 s every time, which rules out
 * duration entirely.  The remaining structural difference between "works" and
 * "hangs" is that WIFI_SCAN + BLE_ENABLE run immediately before the OTA and
 * bring the Wi-Fi/NWP stack (which also touches flash) up first. */
/* BACK ON.  This was set to 0 as a discriminator and left there; every flush
 * test since has run with the NWP DOWN.  TI's ota_example calls psa_fwu_start
 * from inside its network stack with Wi-Fi/TLS live, so if PSA-FWU delegates
 * manifest verify or slot open to the NWP over HIF it would block forever
 * with the radio off -- which is exactly the observed psa_fwu_start hang. */
#define CC3501E_BENCH_RADIO_BEFORE_OTA \
	1 /* RESOLVED 2026-08-22: the radio was NOT the cause.  psa_fwu_start hung
	   because ota.window was MISALIGNED (struct offset 41 = address 1 mod 4, and
	   the manifest pointer feeds a crypto/DMA path); with aligned(32) on that
	   buffer the full 1095276 B OTA completes.  PSA-FWU does NOT delegate
	   manifest verify to the NWP, so the radio runs before OTA as in production. */

/* Progress print shared by the success and "landed late" paths -- the landed path
 * used to skip it, punching holes in the 4 KiB ladder. */
static void ota_progress(size_t off, size_t total, int64_t t_begin)
{
	if ((off % 4096u) < 256u) {
		printf("[cc3501e-bringup] OTA progress %u/%u B t=%u ms\n",
		       (unsigned)off,
		       (unsigned)total,
		       (unsigned)(k_uptime_get() - t_begin));
	}
}

/* #1610 BENCH SIZE LADDER -- comment out for a full-size image.  Set BELOW
 * CC3501E_OTA_WINDOW (16384) to do zero mid-stream flushes. */
/* Truncated to 20480 = just PAST the first window flush (CC3501E_OTA_WINDOW
 * 16384).  The polled bridge currently runs ~26 B/s, so a full 1,095,276 B
 * image cannot finish in a bench window -- but 20 KiB can, and it exercises
 * the exact boundary that killed every DMA build. */
/* #define CC3501E_OTA_TRUNCATE_LEN 20480u */ /* FULL image */

#ifdef CC3501E_OTA_PROMOTE
/*
 * Promote (unjam) an already-committed pending image.  The over-bridge install
 * leaves the image STAGED and relies on the CC35's OWN `psa_fwu_request_reboot()`
 * (armed at FINISH) to swap it.  An image left pending by a bare reset (which
 * carries no swap request) jams the slot: a fresh `cc3501e_ota_update`
 * short-circuits on the occupied slot and can never re-arm the reboot.
 * `cc3501e_ota_promote()` requests the swap-reboot for that committed image.
 * Build with -DCC3501E_OTA_PROMOTE=ON to swap-boot an image a prior
 * -DCC3501E_OTA_REAL run left STAGED-but-unpromoted.
 */
static void cc3501e_demo_ota_promote(cc3501e_t *fw)
{
	printf("[cc3501e-bringup] OTA: promoting the pending image (cc3501e_ota_promote)...\n");
	alp_status_t s = cc3501e_ota_promote(fw, CC3501E_OTA_DEMO_TIMEOUT_MS);
	if (s == ALP_OK) {
		printf("[cc3501e-bringup] OTA promote acked -- the CC35 swaps+boots the pending "
		       "slot; the bridge drops during its reboot, then GET_VERSION should report "
		       "the new image\n");
	} else if (s == ALP_ERR_NOT_READY) {
		printf("[cc3501e-bringup] OTA promote -> NOT_READY (no PSA-FWU in this build)\n");
	} else {
		printf("[cc3501e-bringup] OTA promote -> %d\n", (int)s);
	}
}
#endif /* CC3501E_OTA_PROMOTE */

static void cc3501e_demo_ota(cc3501e_t *fw)
{
#ifdef CC3501E_OTA_PROMOTE
	/* Unjam/promote mode: do NOT stream -- request the swap for an image a prior
	 * run left STAGED-but-unpromoted (a fresh stream would short-circuit). */
	cc3501e_demo_ota_promote(fw);
	return;
#endif
#ifdef CC3501E_OTA_REAL
	/* Genuine signed GPE vendor image -- the plain radio-free bridge signed at a
	 * version HIGHER than the flashed primary (a FORWARD update), so psa_fwu
	 * accepts it (a downgrade is refused at install).  FINISH reaches a true
	 * STAGED and the CC35's own psa_fwu_request_reboot() swaps it in. */
	extern const unsigned char cc3501e_ota_candidate[];
	extern const unsigned int  cc3501e_ota_candidate_len;
	const uint8_t             *image      = cc3501e_ota_candidate;
	size_t                     image_len  = (size_t)cc3501e_ota_candidate_len;
	const bool                 real_image = true;
	/* #1610 BENCH SIZE LADDER.  CC3501E_OTA_WINDOW is 4*4096 = 16384, so an
	 * image BELOW it performs zero mid-stream flushes and defers all flash work
	 * to FINISH.  Truncating isolates "the mid-stream flush breaks the bridge"
	 * from "any flash blackout breaks the bridge".  A truncated image cannot
	 * pass psa_fwu_finish's verification -- a clean FINISH error is the EXPECTED
	 * result here; the observable is whether the WRITEs land and the bridge
	 * SURVIVES, not whether the update installs. */
#ifdef CC3501E_OTA_TRUNCATE_LEN
	if (image_len > (size_t)(CC3501E_OTA_TRUNCATE_LEN)) {
		image_len = (size_t)(CC3501E_OTA_TRUNCATE_LEN);
	}
#endif
#else
	/* Illustrative inert blob (NOT a signed image -- see the note above). */
	static uint8_t inert[1024];
	for (size_t i = 0; i < sizeof(inert); ++i) {
		inert[i] = (uint8_t)(i * 31u + 7u);
	}
	const uint8_t *image      = inert;
	const size_t   image_len  = sizeof(inert);
	const bool     real_image = false;
#endif

	printf("[cc3501e-bringup] OTA: streaming a %u B %s image (hand-driven begin/write/status, "
	       "#1610 bench)...\n",
	       (unsigned)image_len,
	       real_image ? "SIGNED candidate" : "inert demo");

	/* #1610 BENCH: drive begin/write/status by hand instead of cc3501e_ota_update,
	 * purely for visibility.  update() aborts the session on failure, which wipes
	 * the cursor before it can be read -- so every failed run reported
	 * state=0 written=0/0 and said nothing about WHERE it stopped.  Here the
	 * cursor is logged as it advances and again at the exact failure point. */
	/* Enter OTA UPDATE MODE FIRST -- before BEGIN, never mid-session.  A
	 * callback/DMA SPI_open on the bridge slave PERMANENTLY prevents
	 * psa_fwu_start/psa_fwu_write from returning (silicon 2026-08-21), so without
	 * this the device disappears mid-update no matter what the host does.  The
	 * hand-driven path below bypasses cc3501e_ota_update, which does this for its
	 * callers -- so it has to be done explicitly here.  The device warm-reboots and
	 * the confirm is a 0x47 readback, hence the whole-operation budget.
	 *
	 * Fatal on purpose: continuing in DMA mode cannot work, and a "the OTA hung"
	 * report from that state costs a bench session to re-diagnose. */
	const int64_t      t_mode = k_uptime_get();
	const alp_status_t ms     = cc3501e_ota_update_mode(fw, true, CC3501E_OTA_DEMO_TIMEOUT_MS);
	printf("[cc3501e-bringup] OTA update mode -> %d (%u ms)\n",
	       (int)ms,
	       (unsigned)(k_uptime_get() - t_mode));
	if (ms != ALP_OK) {
		printf("[cc3501e-bringup] OTA: NOT in update mode -- refusing to stream (psa_fwu "
		       "would never return on the DMA bridge)\n");
		return;
	}

	int64_t      t_begin = k_uptime_get();
	alp_status_t s       = ALP_ERR_IO;
	/* BEGIN can legitimately take minutes: if the target slot is not READY the
	 * device erases it (TI's own example prints "erasing flash, please wait...").
	 * Retry and print the DEVICE's own view between attempts -- state/cursor/flush
	 * from OTA_STATUS -- so a slow erase is distinguishable from a dead link.
	 * Without this the run printed NOTHING for the whole wait and every diagnosis
	 * was a guess. */
	for (unsigned attempt = 0u; attempt < 2u; ++attempt) {
		s = cc3501e_ota_begin(fw, (uint32_t)image_len, CC3501E_OTA_DEMO_TIMEOUT_MS);
		if (s == ALP_OK) break;
		alp_cc3501e_ota_status_t bs = { 0 };
		const alp_status_t       bq = cc3501e_ota_status(fw, &bs, 0u);
		printf("[cc3501e-bringup] OTA begin attempt %u -> %d after %u ms; device: "
		       "status=%d state=%d cursor=%d busy=%d\n",
		       attempt,
		       (int)s,
		       (unsigned)(k_uptime_get() - t_begin),
		       (int)bq,
		       (bq == ALP_OK) ? (int)bs.state : -1,
		       (bq == ALP_OK) ? (int)bs.bytes_written : -1,
		       (bq == ALP_OK) ? (int)bs.reserved[1] : -1);
	}
	printf("[cc3501e-bringup] OTA begin -> %d (%u ms)\n",
	       (int)s,
	       (unsigned)(k_uptime_get() - t_begin));
	/* #1610 SIDE-OF-THE-LINK PROBE.  Every failure so far assumed the CC35 died,
	 * but a cold cycle resets BOTH ends so it cannot tell us which one.  The
	 * durations are ALSO entirely explained by the HOST's own timers (361046 ms =
	 * blind settle + confirm budget), i.e. the device contributes nothing -- it is
	 * silent from the first moment, not slow.  So ask directly: after a failed
	 * BEGIN, PING as-is, then do a HOST-ONLY resync (cc3501e_reset re-syncs the
	 * link without touching WIFI_EN/nRESET), then PING again, then a hard reset.
	 * PING recovering after a host-only step means the HOST's SPI desynced and the
	 * CC35 was healthy all along -- a completely different bug from "the device
	 * crashes in psa_fwu". */
	if (s != ALP_OK) {
		const alp_status_t p0 = cc3501e_ping(fw);
		const alp_status_t r1 = cc3501e_reset(fw);
		const alp_status_t p1 = cc3501e_ping(fw);
		const alp_status_t r2 = cc3501e_hard_reset(fw);
		const alp_status_t p2 = cc3501e_ping(fw);
		printf("[cc3501e-bringup] OTA post-fail probe: ping=%d | reset=%d ping=%d | "
		       "hard_reset=%d ping=%d\n",
		       (int)p0,
		       (int)r1,
		       (int)p1,
		       (int)r2,
		       (int)p2);
	}

	if (s == ALP_OK) {
		/* ALP_CC3501E_OTA_MAX_CHUNK = ALP_CC3501E_MAX_PAYLOAD(512) - 4 = 508, so 256
		 * was sending half-empty frames and paying the per-frame gate cost twice as
		 * often.  The device stages chunks in RAM and flushes on its own 4096 B block
		 * boundary, so the host chunk does not need to be page-aligned. */
		const size_t chunk = (size_t)ALP_CC3501E_OTA_MAX_CHUNK;
		size_t       off   = 0u;
		uint32_t     stall = 0u;
		/* Last SUCCESSFUL status read.  Printing a status struct whose read FAILED
		 * reports zeros as if they were device truth -- that is how an earlier run
		 * was misread as "device idle" when STATUS had simply timed out. */
		alp_cc3501e_ota_status_t last_ok = { 0 };
		bool                     have_ok = false;
		while (off < image_len) {
			size_t n = image_len - off;
			if (n > chunk) n = chunk;
			const int64_t t_w     = k_uptime_get();
			s                     = cc3501e_ota_write(fw, (uint32_t)off, image + off, n, 0u);
			const int64_t t_w_end = k_uptime_get();
			if (s == ALP_OK) {
				off += n;
				stall = 0u;
				ota_progress(off, image_len, t_begin);
				continue;
			}
			/* BUSY/IO = the device queued a window flush and did NOT consume this
			 * chunk.  Hold off ALL payload and poll HEADER-ONLY until flush_pending
			 * clears, then retry the SAME chunk (mirrors cc3501e_ota_update). */
			alp_cc3501e_ota_status_t ps        = { 0 };
			alp_status_t             qs        = ALP_ERR_IO;
			bool                     landed    = false;
			const int64_t            t_hold    = k_uptime_get();
			/* 0, NOT -1.  At -1 the "every 30 s" test below is true on the FIRST
			 * iteration of every hold-off, so a 55 ms hold printed a ~110-byte
			 * heartbeat too.  Measured on silicon: 2244 of those lines, 645915 B of
			 * console in one OTA = 56.1 s of blocking UART at 115200 against a 118 s
			 * transfer -- HALF the wall time of the OTA was this example describing
			 * itself.  Starting at 0 keeps the intent (a long wait still reports as it
			 * goes, so a killed run yields its verdict) and costs nothing for the
			 * short hold-offs that dominate a healthy stream. */
			int64_t                  last_beat = 0;
			for (;;) {
				qs = cc3501e_ota_status(fw, &ps, 0u);
				if (qs == ALP_OK) {
					last_ok = ps;
					have_ok = true;
					if (ps.reserved[1] == 0u) {
						if (ps.bytes_written >= (uint32_t)(off + n)) landed = true;
						break;
					}
				}
				/* Real clock, not a model of one: charging a fixed cost per iteration
				 * made a nominal 60 s wait really ~12.5 s. */
				const int64_t held_now = k_uptime_get() - t_hold;
				/* HEARTBEAT.  Report the device's breadcrumb AS WE WAIT, not only at
				 * the end: three runs were killed externally mid-hold-off and every
				 * one lost its whole verdict because the data only printed on exit.
				 * Print-as-you-go so a truncated run still yields the answer. */
				if (held_now / 30000 != last_beat) {
					last_beat = held_now / 30000;
					printf("[cc3501e-bringup] OTA flush wait %u s: status=%d dev_state=%d "
					       "cursor=%d flush=%d stage=%d\n",
					       (unsigned)(held_now / 1000),
					       (int)qs,
					       have_ok ? (int)last_ok.state : -1,
					       have_ok ? (int)last_ok.bytes_written : -1,
					       have_ok ? (int)last_ok.reserved[1] : -1,
					       have_ok ? (int)last_ok.reserved[2] : -1);
				}
				if (held_now >= (int64_t)CC3501E_OTA_BENCH_FLUSH_WAIT_MS) break;
				k_msleep(CC3501E_OTA_BENCH_FLUSH_POLL_MS);
			}
			const unsigned held = (unsigned)(k_uptime_get() - t_hold);
			if (landed) {
				off += n;
				stall = 0u;
				ota_progress(off, image_len, t_begin);
				continue;
			}
			/* stall == 0 here is the NORMAL window-flush boundary -- `landed` cannot be
			 * true when the flush merely cleared without consuming this chunk, so
			 * calling it a failure mislabels the healthy path.  Only a REPEAT is bad. */
			if (stall == 0u) {
				printf("[cc3501e-bringup] OTA window-flush hold-off off=%u write=%d (%u ms) "
				       "status=%d held=%u ms dev_state=%d dev_cursor=%d flush=%d stage=%d "
				       "t=%u ms\n",
				       (unsigned)off,
				       (int)s,
				       (unsigned)(t_w_end - t_w),
				       (int)qs,
				       held,
				       have_ok ? (int)last_ok.state : -1,
				       have_ok ? (int)last_ok.bytes_written : -1,
				       have_ok ? (int)last_ok.reserved[1] : -1,
				       /* stage = the device's own breadcrumb (STATUS reserved[2]),
				        * in TWO encodings -- see include/alp/protocol/cc3501e.h:
				        *   1..0x3F  the psa_fwu_* call that failed the last flush
				        *   0x40|p   no psa fault; low 6 bits = transport phase
				        *   0xC0|p   same, and the bridge is running POLLED
				        * so 64 / 192 on a healthy run is a phase report, NOT a
				        * fault. */
				       have_ok ? (int)last_ok.reserved[2] : -1,
				       (unsigned)(k_uptime_get() - t_begin));
			}
			if (have_ok && last_ok.state == ALP_CC3501E_OTA_STATE_ERROR) {
				printf("[cc3501e-bringup] OTA device latched ERROR at off=%u -- not retrying\n",
				       (unsigned)off);
				break;
			}
			if (++stall > 5u) {
				printf("[cc3501e-bringup] OTA STOPPED at off=%u/%u write=%d status=%d "
				       "dev_state=%d dev_cursor=%d flush=%d stage=%d (last_ok=%d)\n",
				       (unsigned)off,
				       (unsigned)image_len,
				       (int)s,
				       (int)qs,
				       have_ok ? (int)last_ok.state : -1,
				       have_ok ? (int)last_ok.bytes_written : -1,
				       have_ok ? (int)last_ok.reserved[1] : -1,
				       have_ok ? (int)last_ok.reserved[2] : -1,
				       have_ok ? 1 : 0);
				break;
			}
		}
		if (off >= image_len) {
			printf("[cc3501e-bringup] OTA all %u B accepted -- FINISH\n", (unsigned)image_len);
			s = cc3501e_ota_finish(fw, CC3501E_OTA_DEMO_TIMEOUT_MS);
			printf("[cc3501e-bringup] OTA finish -> %d\n", (int)s);
		} else {
			s = ALP_ERR_IO;
		}
	}

	/* LEAVE UPDATE MODE on every path that did not reach FINISH.  A FINISH that
	 * staged takes the device out by itself, but the ERROR-latch break, the
	 * stall break and the short-stream exit above all fall through here with the
	 * device still parked in the radio-dead polled boot -- where WIFI_SCAN,
	 * BLE_ENABLE and GET_MAC queue forever and answer BUSY forever, because
	 * nothing drains the worker on that boot.  The soak that follows this
	 * function would then look permanently broken.  This example is the pattern
	 * customers copy, so it has to model the exit, not just the entry. */
	if (s != ALP_OK) {
		(void)cc3501e_ota_update_mode(fw, false, CC3501E_OTA_DEMO_TIMEOUT_MS);
	}

	/* Read back the session state regardless of the update result -- this is
	 * the field-diagnostic call (`alp companion ota status` uses the same). */
	alp_cc3501e_ota_status_t st = { 0 };
	if (cc3501e_ota_status(fw, &st, CC3501E_OTA_DEMO_TIMEOUT_MS) == ALP_OK) {
		/* state: 1=WRITING, 2=STAGED (FINISH ok), 3=ERROR (FINISH rejected the
		 * image -- e.g. anti-rollback on a downgrade).  reserved[0] = the last
		 * swap-reboot rc: 0 = success/none, non-zero = the swap was REFUSED. */
		printf("[cc3501e-bringup] OTA status: state=%u written=%u/%u B reboot_rc=%d\n",
		       (unsigned)st.state,
		       (unsigned)st.bytes_written,
		       (unsigned)st.total_len,
		       (int)(int8_t)st.reserved[0]);
	}
	/* #1610: did the CC3501E RESET mid-stream?  A session that reads IDLE with
	 * total_len=0 after a stream either never opened or was wiped by a reboot --
	 * and only uptime can tell those apart.  Compare against the uptime printed at
	 * bring-up: a small value here means the device restarted under us. */
	{
		alp_cc3501e_diag_info_t di2 = { 0 };
		if (cc3501e_diag_info(fw, &di2) == ALP_OK) {
			printf("[cc3501e-bringup] OTA post-diag: uptime=%u ms reset_cause=%u "
			       "last_error=%u free_heap=%u\n",
			       (unsigned)di2.uptime_ms,
			       (unsigned)di2.reset_cause,
			       (unsigned)di2.last_error,
			       (unsigned)di2.free_heap_bytes);
		}
	}

	if (s == ALP_OK) {
		printf("[cc3501e-bringup] OTA -> STAGED (image accepted by psa_fwu); the CC35's "
		       "own psa_fwu_request_reboot() swaps it in -- the bridge drops, then "
		       "reboots into the new image (a forward image; a radio-free candidate "
		       "makes WIFI_SCAN go NOT_READY, proving the swap)\n");
	} else if (s == ALP_ERR_NOT_READY) {
		printf("[cc3501e-bringup] OTA -> NOT_READY (no PSA-FWU in this CC3501E image) "
		       "-- expected on a non-OTA firmware build\n");
	} else if (real_image) {
		/* A genuine FORWARD image should reach STAGED; a non-OK here is a real fault.
		 * state=3 (ERROR) with a downgrade image means anti-rollback refused it at
		 * install -- use a candidate version above the primary.  Reset the session. */
		printf("[cc3501e-bringup] OTA -> %d (signed image did NOT stage; state=%u -- if "
		       "ERROR(3), the candidate is likely a downgrade the SBL refused); "
		       "aborting the session\n",
		       (int)s,
		       (unsigned)st.state);
		(void)cc3501e_ota_abort(fw, CC3501E_OTA_DEMO_TIMEOUT_MS);
	} else {
		/* An inert blob fails at FINISH (image validation) -- the host stream +
		 * framing still round-tripped, which is what the default mode proves.  Reset
		 * the half-open session so the slot is clean for a real image. */
		printf("[cc3501e-bringup] OTA -> %d (inert blob rejected at FINISH as expected); "
		       "aborting the session\n",
		       (int)s);
		(void)cc3501e_ota_abort(fw, CC3501E_OTA_DEMO_TIMEOUT_MS);
	}
}
#endif /* CC3501E_OTA_DEMO */

/* The OTA-demo block above defines CC3501E_BENCH_RADIO_BEFORE_OTA, but the
 * liveness soak references it unconditionally.  Without this default the
 * example fails to compile in its DEFAULT configuration (every CC3501E_OTA_*
 * option is OFF by default in CMakeLists.txt), which is exactly the
 * configuration twister builds.  Default 1: bring the radio up normally.
 */
#ifndef CC3501E_BENCH_RADIO_BEFORE_OTA
#define CC3501E_BENCH_RADIO_BEFORE_OTA 1
#endif

int main(void)
{
	printf("\n[cc3501e-bringup] E1M-AEN CC3501E Wi-Fi/BLE coprocessor bring-up\n");
	g_cc3501e_witness.magic = CC3501E_WITNESS_MAGIC; /* marks the struct found over SWD */
	g_cc3501e_witness.phase = CC3501E_PHASE_MAIN;

	/*
	 * Bring up the SoM's CC3501E coprocessor in ONE call -- cc3501e_bridge_bringup()
	 * (cc3501e_bridge.{c,h}, the reusable SoM bring-up template): opens the inter-chip
	 * SPI bridge + the WIFI_EN/nRESET control pins, binds them, attaches the GPIO proxy,
	 * and runs the power+reset sequence (TI SWRU626 + the Puya cold-boot hard-reset).
	 * An application just copies that pattern; here we wrap it with the SWD witness so
	 * a console-less bench read sees where it got to.
	 */
	g_cc3501e_witness.phase = CC3501E_PHASE_GPIO;
	cc3501e_t    fw;
	alp_status_t s                 = cc3501e_bridge_bringup(&fw);
	g_cc3501e_witness.reset_status = (uint32_t)s;
	g_cc3501e_witness.phase        = CC3501E_PHASE_RESET;
	if (s == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) {
		/* The backend authority itself says the part/pins are absent on this
		 * SoC -- a bench/board limitation, not a bug in this app (same rule
		 * as an ALP_ERR_NOSUPPORT boot-authority answer elsewhere in this
		 * diff: aen-alp-rpc/src/main.c, aen-dualcore-doorbell/src/main.c). */
		printf("[cc3501e-bringup] bridge bring-up skipped (SPI bus %u / WIFI_EN+nRESET "
		       "absent? err=%d) -- check the board overlay\n",
		       CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		printf("RESULT SKIP: cc3501e_bridge_bringup -> NOT_PRESENT_ON_THIS_SOC (SPI bus %u / "
		       "WIFI_EN+nRESET absent? err=%d)\n",
		       CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		return 0;
	}
	printf("[cc3501e-bringup] cc3501e bridge bring-up -> %d%s\n",
	       (int)s,
	       (s == ALP_ERR_NOSUPPORT) ? " (control pins not bound?)" : "");

	/*
	 * Step 4 -- retry PING until the coprocessor answers.
	 *
	 * reset() already waited out the boot budget, so the first PING
	 * usually lands; the retry loop just absorbs any residual ramp/boot
	 * jitter.  A serviced PING proves the firmware parsed a frame and
	 * staged its reply over the hardware-framed bridge -- the core thing this
	 * bring-up checks.
	 */
	g_cc3501e_witness.phase = CC3501E_PHASE_PING;
	bool up                 = false;
	for (unsigned attempt = 0u; attempt < CC3501E_PING_RETRIES; ++attempt) {
		s = cc3501e_ping(&fw);
		if (s == ALP_OK) {
			printf("[cc3501e-bringup] PING ok after %u attempt%s\n",
			       attempt + 1u,
			       (attempt == 0u) ? "" : "s");
			up = true;
			break;
		}
		printf("[cc3501e-bringup] PING attempt %u -> %d (not ready yet?) -- retrying in %u ms\n",
		       attempt,
		       (int)s,
		       CC3501E_PING_GAP_MS);
		alp_delay_ms(CC3501E_PING_GAP_MS);
	}
	if (!up) {
		printf("[cc3501e-bringup] coprocessor never answered PING -- check power "
		       "(WIFI_EN), the SPI1 pinmux, and that the CC3501E is running its "
		       "firmware; entering soak so the link can be probed live\n");
	}

	/*
	 * Step 5 -- read + check the protocol version.
	 *
	 * GET_VERSION returns the *protocol* version; it must match
	 * ALP_CC3501E_PROTOCOL_VERSION for the wire contract to hold.
	 */
	uint16_t version          = 0u;
	s                         = cc3501e_get_version(&fw, &version);
	g_cc3501e_witness.version = (uint32_t)version | ((uint32_t)(uint8_t)s << 16);
	g_cc3501e_witness.phase   = CC3501E_PHASE_VERSION;
	if (s == ALP_OK) {
		printf("[cc3501e-bringup] GET_VERSION -> protocol v%u (host expects v%u)%s\n",
		       version,
		       ALP_CC3501E_PROTOCOL_VERSION,
		       (version == ALP_CC3501E_PROTOCOL_VERSION) ? " -- match" : " -- MISMATCH!");
	} else {
		printf("[cc3501e-bringup] GET_VERSION -> %d\n", (int)s);
	}

	/* Step 6 -- extended diagnostics (v2-firmware; v0.1 rejects cleanly). */
	cc3501e_dump_diag(&fw);

	/*
	 * Step 7 -- drive the Wi-Fi control path (GET_MAC + SCAN, optional
	 * CONNECT).  This is the bring-up's reason to exist beyond a bare PING:
	 * GET_MAC / SCAN are poll-by-repeat, so a success proves the firmware's
	 * Wi-Fi worker seam (submit -> worker -> reply) from the host.  Results
	 * land in the witness for a console-less J-Link read.  Skipped harmlessly
	 * if the link never came up (the helpers just time out and record it).
	 */
	/* DEFERRED: do NOT read the radio (GET_MAC -> Wlan_Start) here -- that fires
	 * the radio bring-up before the bridge is proven alive.  The soak below reads
	 * the MAC only AFTER the link is solidly up (ping_ok >= threshold) -- "wait
	 * until ready to read".  Kept the function for the scan/connect path, gated
	 * the same way later. */
	(void)cc3501e_wifi_probe;

	/*
	 * Step 8 -- liveness soak.  Keep PINGing so the link is continuously
	 * verifiable over J-Link, and re-read the version every 8th cycle (an
	 * odd-length reply that stresses the framing residue handling).  This
	 * mirrors the v2n-gd32-bridge-ping soak.
	 */
	printf("[cc3501e-bringup] entering liveness soak (PING every 500 ms)\n");
	g_cc3501e_witness.phase = CC3501E_PHASE_SOAK;
#ifdef CC3501E_DMA_STREAM_BENCH
	bool stream_done = false;
#endif
#ifdef CC3501E_OTA_DEMO
	bool ota_done = false;
#endif
	/* Bounded bench verdict, printed once -- same "checkpoint inside a
	 * forever loop" shape as examples/peripheral-io/blink: the soak below
	 * must never return (see the "not reached" note past the loop), so the
	 * RESULT line has to fire from inside it instead of after it.  Gated on
	 * the witness's OWN accumulated ping_ok/ping_fail counters (not on
	 * merely reaching this line), so it is unreachable from a run that
	 * never actually PINGed the coprocessor successfully. */
	bool result_printed = false;
	for (uint32_t i = 0u;; ++i) {
		/*
		 * Run-once FRAMED bulk-stream throughput benchmark.  Once the link is up,
		 * send MAX-payload frames via CMD_STREAM_WRITE back-to-back: each frame's
		 * payload phase (508 B, well over CONFIG_SPI_DW_ALIF_DMA_MIN_LEN) rides the
		 * host peripheral-DMA path (evtrtr0 -> DMA0, no CPU FIFO shuffling), and
		 * the firmware sinks + ACKs every frame so the link stays framed -- real
		 * bulk data over the bridge, not throwaway clocking.  Records KB/s.
		 */
#ifdef CC3501E_DMA_STREAM_BENCH
		/* Opt-in bulk-stream throughput benchmark (build with
		 * -DEXTRA_CFLAGS=-DCC3501E_DMA_STREAM_BENCH).  Framed + ACKed, so it does
		 * NOT desync the bridge -- the soak PINGs keep working afterwards. */
		if (!stream_done && g_cc3501e_witness.ping_ok >= 20u) {
			/* One frame = the largest payload a request carries (MAX_PAYLOAD minus
			 * the 4-byte header).  The frame buffer may live in DTCM: the PL330
			 * driver remaps it via local_to_global() so the AXI master reaches it. */
			enum { FRAME_LEN = ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES };
			static uint8_t frame[FRAME_LEN];
			stream_done = true;
			for (uint32_t k = 0u; k < FRAME_LEN; ++k) {
				frame[k] = (uint8_t)k;
			}
			const uint32_t frames = 512u;
			uint32_t       ok     = 0u;
			uint32_t       t0     = k_cycle_get_32();
			for (uint32_t k = 0u; k < frames; ++k) {
				if (cc3501e_stream_write(&fw, frame, (size_t)FRAME_LEN) == ALP_OK) {
					ok++;
				}
			}
			uint32_t us                        = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
			g_cc3501e_witness.dma_stream_iters = ok;
			g_cc3501e_witness.dma_stream_us    = us;
			g_cc3501e_witness.dma_stream_kbps =
			    (us > 0u) ? (uint32_t)(((uint64_t)ok * FRAME_LEN * 1000u) / us) : 0u;
			printf("[cc3501e-bringup] DMA stream: %u x %u B in %u us -> %u KB/s\n",
			       ok,
			       (unsigned)FRAME_LEN,
			       us,
			       g_cc3501e_witness.dma_stream_kbps);
		}
#endif /* CC3501E_DMA_STREAM_BENCH */

#ifdef CC3501E_OTA_DEMO
		/* One-shot OTA demo, run only once the link is solidly up (same
		 * ping_ok discipline as the DMA bench): OTA's FINISH does a flash burst,
		 * so gate it behind a stable bridge. */
		if (!ota_done && g_cc3501e_witness.ping_ok >= 20u) {
			ota_done = true;
			cc3501e_demo_ota(&fw);
		}
#endif /* CC3501E_OTA_DEMO */

		s                             = cc3501e_ping(&fw);
		g_cc3501e_witness.last_status = (uint32_t)s;
		if (s == ALP_OK) {
			g_cc3501e_witness.ping_ok++;
		} else {
			g_cc3501e_witness.ping_fail++;
		}
		printf("[cc3501e-bringup] soak PING #%u -> %d\n", i, (int)s);

		/* Fire once ping_ok reaches the same 20-PING stability bar the
		 * MAC/scan/BLE gates below already use as "the link is solidly
		 * up".  ping_fail is cumulative for the whole run, so a single
		 * soak PING failure anywhere before this point permanently
		 * flips the verdict to FAIL instead of PASS -- there is no
		 * retroactive path back to PASS once it prints. The reverse is
		 * also one-way and deliberate: once RESULT PASS has printed
		 * (result_printed latches), a LATER ping_fail (a link that dies
		 * at soak PING #21+) does not re-print FAIL -- PASS is a ceiling
		 * on "20 consecutive-so-far PINGs were clean", not a claim about
		 * the rest of the run. The per-PING ok/fail counters keep
		 * accumulating in the witness struct for a bench SWD read either
		 * way. */
		if (!result_printed && g_cc3501e_witness.ping_ok >= 20u) {
			if (g_cc3501e_witness.ping_fail == 0u) {
				printf("RESULT PASS: cc3501e link stable over %u soak PINGs "
				       "(ping_fail=0)\n",
				       g_cc3501e_witness.ping_ok);
			} else {
				printf("RESULT FAIL: cc3501e link unstable -- %u soak PING "
				       "failure(s) alongside %u ok\n",
				       g_cc3501e_witness.ping_fail,
				       g_cc3501e_witness.ping_ok);
			}
			result_printed = true;
		}

		/* Once the link is alive (PING ok), keep retrying GET_MAC until it
		 * lands -- retrying here lands the worker-routed Wi-Fi identity read
		 * end-to-end on the stable link. */
		if (g_cc3501e_witness.mac_ok == 0u && s == ALP_OK && g_cc3501e_witness.ping_ok >= 20u) {
			uint8_t      mac[CC3501E_MAC_LEN] = { 0 };
			alp_status_t ms              = cc3501e_wifi_get_mac(&fw, mac, CC3501E_MAC_TIMEOUT_MS);
			g_cc3501e_witness.mac_status = (uint32_t)ms;
			if (ms == ALP_OK) {
				g_cc3501e_witness.mac_ok = 1u;
				g_cc3501e_witness.mac_lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
				                           ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
				g_cc3501e_witness.mac_hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
				/* Is the READY line REAL?  Every "P2_6 reads 0" claim so far came
				 * from a raw register poke at 0x49002050 bit6 taken from a doc,
				 * never from the GPIO driver.  Read it the supported way while the
				 * device is idle (main.c raises READY at boot): 1 = the CC35
				 * GPIO17 -> P2_6 net works and the READY gate can be trusted;
				 * 0 = the line really is dead here and polled update mode cannot
				 * be timed. */
				{
					bool               rdy = false;
					const alp_status_t rs  = (fw.ready_pin != NULL)
					                             ? alp_gpio_read(fw.ready_pin, &rdy)
					                             : ALP_ERR_NOSUPPORT;
					printf("[cc3501e-bringup] READY probe: rc=%d level=%d\n", (int)rs, rdy ? 1 : 0);
				}
				printf("[cc3501e-bringup] soak GET_MAC ok %02x:%02x:%02x:%02x:%02x:%02x\n",
				       mac[0],
				       mac[1],
				       mac[2],
				       mac[3],
				       mac[4],
				       mac[5]);
			}
		}

		/* Once the MAC is in (link proven stable), do a one-shot worker-routed
		 * SCAN -- proves a LIST-returning Wi-Fi op over the bridge.  scan_count>0
		 * = the bench AP was seen.  Gated like GET_MAC so it runs on the stable
		 * link, not the cold first-contact window; retried until it lands. */
		static bool scan_done = false;
		if (!CC3501E_BENCH_RADIO_BEFORE_OTA) scan_done = true; /* #1610 discriminator */
		if (!scan_done && g_cc3501e_witness.mac_ok == 1u && s == ALP_OK) {
			static cc3501e_scan_record_t scan[CC3501E_SCAN_MAX_RECORDS];
			size_t                       n = 0u;
			alp_status_t                 ss =
			    cc3501e_wifi_scan(&fw, scan, CC3501E_SCAN_MAX_RECORDS, &n, CC3501E_SCAN_TIMEOUT_MS);
			g_cc3501e_witness.scan_status = (uint32_t)ss;
			if (ss == ALP_OK) {
				g_cc3501e_witness.scan_count      = (uint32_t)n;
				g_cc3501e_witness.scan_first_rssi = (n > 0u) ? (int32_t)scan[0].rssi_dbm : 0;
				scan_done                         = true;
				printf("[cc3501e-bringup] soak WIFI_SCAN ok -> %u AP(s)\n", (unsigned)n);
			} else {
				printf("[cc3501e-bringup] soak WIFI_SCAN -> %d\n", (int)ss);
			}
		}

		/* After the MAC is in, bring up BLE once (worker-routed: shared-HIF Wi-Fi
		 * start + nimble_host_start ~2s).  ble_enabled=1 = the BLE controller +
		 * NimBLE host came up through the bridge.  Gated/retried like the scan so
		 * it runs on the stable link.  (Firmware without -Ble -> NOT_READY.) */
		static bool ble_done = false;
		if (!CC3501E_BENCH_RADIO_BEFORE_OTA) ble_done = true; /* #1610 discriminator */
		if (!ble_done && g_cc3501e_witness.mac_ok == 1u && s == ALP_OK) {
			alp_status_t bs              = cc3501e_ble_enable(&fw, CC3501E_MAC_TIMEOUT_MS);
			g_cc3501e_witness.ble_status = (uint32_t)bs;
			if (bs == ALP_OK) {
				g_cc3501e_witness.ble_enabled = 1u;
				ble_done                      = true;
				printf("[cc3501e-bringup] soak BLE_ENABLE ok\n");
			} else if (bs == ALP_ERR_NOT_READY) {
				ble_done = true; /* firmware built without -Ble; stop retrying */
				printf("[cc3501e-bringup] soak BLE_ENABLE -> NOT_READY (no -Ble build)\n");
			} else {
				printf("[cc3501e-bringup] soak BLE_ENABLE -> %d\n", (int)bs);
			}
		}

		/* After the SCAN lands, ASSOCIATE once -- only when credentials were given
		 * at build time (never hardcoded; see CC3501E_WIFI_SSID above).
		 *
		 * This is the step the earlier refactor DEFERRED and never re-wired:
		 * cc3501e_wifi_probe() was left as a bare (void) cast, so the whole
		 * CONNECT/RSSI/IP path was dead code the compiler stripped -- Wi-Fi
		 * association had never actually run from this example.  Gated on
		 * scan_done so it runs on the proven-stable link like the SCAN and BLE
		 * steps, and latched to ONE attempt (the link has been observed to wedge
		 * after repeated connects). */
		static bool conn_done = false;
		if (CC3501E_WIFI_SSID[0] == '\0') {
			conn_done = true; /* no credentials compiled in -- nothing to do */
		}
		if (!conn_done && scan_done && s == ALP_OK) {
			/* BOUNDED RETRY, not one-shot.  WIFI_CONNECT is intermittent on this
			 * silicon (observed alternating ALP_OK / ALP_ERR_IO across runs), but the
			 * bridge SURVIVES a failed attempt -- PINGs keep returning 0 immediately
			 * after -- so a retry is safe and does not reproduce the historical
			 * "wedges after repeated connects" failure.  Bounded so an unreachable AP
			 * cannot spin the soak forever. */
			static uint8_t conn_tries = 0u;
			if (++conn_tries >= 5u) {
				conn_done = true;
			}
			const alp_status_t cs = cc3501e_wifi_connect(&fw,
			                                             CC3501E_WIFI_SSID,
			                                             (uint8_t)CC3501E_WIFI_SECURITY,
			                                             CC3501E_WIFI_PASS,
			                                             CC3501E_CONN_TIMEOUT_MS);
			printf("[cc3501e-bringup] soak WIFI_CONNECT -> %d\n", (int)cs);
			if (cs == ALP_OK) {
				conn_done   = true;
				int8_t rssi = 0;
				if (cc3501e_wifi_rssi(&fw, &rssi) == ALP_OK) {
					printf("[cc3501e-bringup] soak RSSI -> %d dBm\n", (int)rssi);
				}
				uint8_t ip[4] = { 0 };
				if (cc3501e_wifi_get_ip(&fw, ip) == ALP_OK) {
					printf(
					    "[cc3501e-bringup] soak IP -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
				}
				cc3501e_net_probe(&fw);
			}
		}

		if ((i % 8u) == 0u) {
			uint16_t     v            = 0u;
			alp_status_t vs           = cc3501e_get_version(&fw, &v);
			g_cc3501e_witness.version = (uint32_t)v | ((uint32_t)(uint8_t)vs << 16);
			printf("[cc3501e-bringup] soak GET_VERSION #%u -> %d (v%u)\n", i, (int)vs, v);
		}
		alp_delay_ms(500);
	}

	/* not reached -- the soak loops forever.  The bridge SPI + control pins are
	 * owned by cc3501e_bridge_bringup(); a real app that tears down would add a
	 * cc3501e_bridge_teardown() helper. */
	return 0;
}
