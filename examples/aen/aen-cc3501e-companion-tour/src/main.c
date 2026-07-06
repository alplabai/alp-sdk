/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-cc3501e-companion-tour -- the CAPSTONE full-surface example for the
 * on-module TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor, driven from the Alif
 * Ensemble E8 host (M55-HE).
 *
 * WHAT THIS IS.  aen-cc3501e-bringup is the *minimal* bring-up: power the
 * CC3501E, PING it, and soak so a J-Link can watch the link.  THIS example
 * is the opposite end -- it exercises the WHOLE companion API in one linear
 * sequence, as living documentation of the hand-written-firmware path:
 *
 *     init  ->  ping  ->  diag info
 *           ->  Wi-Fi scan  ->  Wi-Fi connect  ->  get IP
 *           ->  TCP socket: open -> connect -> send -> recv -> close
 *           ->  Wi-Fi disconnect
 *           ->  BLE enable  ->  BLE scan  ->  BLE disable
 *           ->  proxied-GPIO read
 *
 * All of it goes through the portable companion driver in
 * <alp/chips/cc3501e.h> (chips/cc3501e/cc3501e.c) over the inter-chip SPI1
 * bridge; the app never touches the raw wire protocol.
 *
 * WHY IT IS A DEMO, NOT A GATE.  Every step is guarded and NON-FATAL: a step
 * that fails prints the status and the tour continues to the next.  On a bench
 * with no AP in range the connect times out, with no BLE-built firmware the
 * enable returns NOT_READY, and so on -- the example still runs end to end and
 * shows you the shape of each call.  A real application would branch on these
 * statuses instead of just logging them.
 *
 * WHERE THE HARDWARE SETUP LIVES.  The SoM bring-up (control pins + inter-chip
 * SPI + power/reset sequence) is one call -- cc3501e_bridge_bringup() in
 * cc3501e_bridge.{c,h}, the reusable SoM template shared with the bringup
 * example.  Copy that pair into your own app, or call it directly.
 *
 * This file is ~50 % comment by design: examples are documentation for
 * hand-written firmware, not just runnable code.
 */

#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/chips/cc3501e.h"
#include "alp/e1m_pinout.h" /* ALP_E1M_GPIO_IOxx -- portable E1M pin ids (proxied via the bridge) */

#include "cc3501e_bridge.h" /* cc3501e_bridge_bringup() -- the SoM bring-up helper */

/* ------------------------------------------------------------------ */
/* Tunables -- timeouts + the (build-time) Wi-Fi credentials.         */
/* ------------------------------------------------------------------ */

/* Poll-by-repeat budgets.  Several companion ops kick off a firmware worker
 * (scan / association / BLE bring-up) and answer BUSY until it finishes; the
 * driver re-issues the request until OK or the budget elapses.  These are the
 * *upper bounds* on that wait, generous enough for a real radio op. */
#define TOUR_PING_RETRIES    25u
#define TOUR_PING_GAP_MS     200u
#define TOUR_SCAN_TIMEOUT    8000u
#define TOUR_CONNECT_TIMEOUT 15000u
#define TOUR_BLE_TIMEOUT     30000u
#define TOUR_SOCK_TIMEOUT    5000u

/* How many scan records the witness arrays hold (bounded -- a busy band can
 * report dozens of APs; we print the first handful). */
#define TOUR_SCAN_MAX 16u

/*
 * Wi-Fi STA credentials for the CONNECT step.  DELIBERATELY EMPTY by default --
 * never hardcode bench credentials in a public example.  Set them at build time
 * WITHOUT editing this file, e.g.:
 *
 *   west build ... -- -DEXTRA_CFLAGS="-DTOUR_WIFI_SSID=\\\"myssid\\\" \
 *                                     -DTOUR_WIFI_PASS=\\\"mypass\\\""
 *
 * When TOUR_WIFI_SSID is empty the connect + socket + IP steps are skipped
 * (they need an association), and the tour jumps straight to the BLE section.
 */
#ifndef TOUR_WIFI_SSID
#define TOUR_WIFI_SSID ""
#endif
#ifndef TOUR_WIFI_PASS
#define TOUR_WIFI_PASS ""
#endif
/* Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE (alp_cc3501e_wifi_connect_t). */
#ifndef TOUR_WIFI_SECURITY
#define TOUR_WIFI_SECURITY 1u
#endif

/*
 * Destination for the TCP socket round-trip (only used when connected).  The
 * default is example.com's canonical IPv4 (93.184.216.34) on port 80 -- change
 * it to a host reachable from your bench network.  ip[] is network order,
 * ip[0] = the most-significant octet (a.b.c.d), which is exactly what
 * cc3501e_wifi_get_ip() also hands back, so a lease can feed a connect directly.
 */
static const uint8_t TOUR_TCP_IP[4] = { 93, 184, 216, 34 };
#define TOUR_TCP_PORT 80u

/* ------------------------------------------------------------------ */
/* Tour steps.  Each is self-contained + non-fatal: it logs its own      */
/* outcome and returns void so main() reads as the sequence-of-steps      */
/* documentation it is meant to be.                                       */
/* ------------------------------------------------------------------ */

/* Step: PING until the coprocessor answers (or the retry budget elapses).
 * reset() already waited out the boot budget, so the first PING usually
 * lands; the loop absorbs residual ramp/boot jitter.  Returns true if up. */
static bool tour_ping(cc3501e_t *fw)
{
	for (unsigned i = 0u; i < TOUR_PING_RETRIES; ++i) {
		alp_status_t s = cc3501e_ping(fw);
		if (s == ALP_OK) {
			printf("[tour] PING ok after %u attempt%s\n", i + 1u, (i == 0u) ? "" : "s");
			return true;
		}
		alp_delay_ms(TOUR_PING_GAP_MS);
	}
	printf("[tour] PING never answered -- check WIFI_EN power, the SPI1 pinmux, and that "
	       "the CC3501E is running its firmware\n");
	return false;
}

/* Step: protocol version + extended diagnostics.  GET_VERSION returns the wire
 * *protocol* version (the host compat gate); GET_DIAG_INFO decodes the 16-byte
 * firmware snapshot (release version, reset cause, active role, uptime, heap). */
static void tour_diag(cc3501e_t *fw)
{
	uint16_t     version = 0u;
	alp_status_t s       = cc3501e_get_version(fw, &version);
	if (s == ALP_OK) {
		printf("[tour] GET_VERSION -> protocol v%u (host expects v%u)%s\n",
		       version,
		       ALP_CC3501E_PROTOCOL_VERSION,
		       (version == ALP_CC3501E_PROTOCOL_VERSION) ? " -- match" : " -- MISMATCH!");
	} else {
		printf("[tour] GET_VERSION -> %d\n", (int)s);
	}

	/* GET_DIAG_INFO is a v2-firmware feature; v0.1 firmware rejects it with
	 * ALP_ERR_INVAL, which still proves the request round-trips + the error
	 * path is wired.  Decode the struct only on success. */
	alp_cc3501e_diag_info_t diag;
	s = cc3501e_diag_info(fw, &diag);
	if (s == ALP_OK) {
		printf("[tour] diag: fw=0x%04x reset_cause=%u role=%u uptime=%u ms "
		       "free_heap=%u B last_error=%u\n",
		       diag.fw_version,
		       diag.reset_cause,
		       diag.role,
		       diag.uptime_ms,
		       diag.free_heap_bytes,
		       diag.last_error);
	} else if (s == ALP_ERR_INVAL) {
		printf("[tour] GET_DIAG_INFO -> rejected (v0.1 firmware; v2-only) -- expected\n");
	} else {
		printf("[tour] GET_DIAG_INFO -> %d\n", (int)s);
	}
}

/* Step: run a Wi-Fi scan and print the discovered APs.  SCAN_START is
 * poll-by-repeat (the firmware runs Wlan_Scan on a worker and answers BUSY
 * until it finishes); a success proves the whole submit -> worker -> reply
 * seam from the host.  cc3501e_wifi_sec_name() decodes each record's raw TI
 * SecurityInfo into a human bucket (open/wpa2/wpa3/...). */
static void tour_wifi_scan(cc3501e_t *fw)
{
	static cc3501e_scan_record_t recs[TOUR_SCAN_MAX];
	size_t                       n = 0u;
	alp_status_t s = cc3501e_wifi_scan(fw, recs, TOUR_SCAN_MAX, &n, TOUR_SCAN_TIMEOUT);
	if (s != ALP_OK) {
		printf("[tour] WIFI_SCAN -> %d\n", (int)s);
		return;
	}
	printf("[tour] WIFI_SCAN -> %u AP(s)\n", (unsigned)n);
	for (size_t i = 0u; i < n; ++i) {
		printf("   [%u] \"%s\" ch%u %d dBm %s\n",
		       (unsigned)i,
		       recs[i].ssid,
		       recs[i].channel,
		       (int)recs[i].rssi_dbm,
		       cc3501e_wifi_sec_name(recs[i].security_info));
	}
}

/*
 * Step: connect to an AP, then open a TCP socket and do a tiny HTTP GET.
 *
 * Skipped entirely when TOUR_WIFI_SSID is empty (no credentials -> nothing to
 * associate with).  On a successful association it:
 *   - reads the DHCP lease with cc3501e_wifi_get_ip() (dotted-quad, printable),
 *   - opens a STREAM (TCP) socket, connects it to TOUR_TCP_IP:TOUR_TCP_PORT,
 *   - sends a minimal HTTP request and reads whatever comes back,
 *   - closes the socket.
 * Every socket call is a worker-routed poll-by-repeat over the bridge.  Then it
 * tears the association down again with cc3501e_wifi_disconnect().
 */
static void tour_wifi_connect_and_socket(cc3501e_t *fw)
{
	if (TOUR_WIFI_SSID[0] == '\0') {
		printf("[tour] WIFI_CONNECT skipped (TOUR_WIFI_SSID empty -- set it at build time); "
		       "socket + IP steps need an association, skipping too\n");
		return;
	}

	printf("[tour] WIFI_CONNECT -> SSID \"%s\" (sec %u)...\n",
	       TOUR_WIFI_SSID,
	       (unsigned)TOUR_WIFI_SECURITY);
	alp_status_t s = cc3501e_wifi_connect(
	    fw, TOUR_WIFI_SSID, (uint8_t)TOUR_WIFI_SECURITY, TOUR_WIFI_PASS, TOUR_CONNECT_TIMEOUT);
	if (s != ALP_OK) {
		printf("[tour] WIFI_CONNECT -> %d (no AP in range? wrong PSK?)\n", (int)s);
		return;
	}
	printf("[tour] WIFI_CONNECT -> associated\n");

	/* DHCP lease.  ip[] is network order (ip[0] = MSB), already dotted-quad. */
	uint8_t ip[4] = { 0 };
	if (cc3501e_wifi_get_ip(fw, ip) == ALP_OK) {
		printf("[tour] IP -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
	} else {
		printf("[tour] IP -> not leased yet\n");
	}

	/* Signal strength of the association (a quick sanity read). */
	int8_t rssi = 0;
	if (cc3501e_wifi_rssi(fw, &rssi) == ALP_OK) {
		printf("[tour] RSSI -> %d dBm\n", (int)rssi);
	}

	/* ---- TCP socket round-trip ---- */
	uint16_t sock = 0u;
	s             = cc3501e_sock_open(fw,
	                                  ALP_CC3501E_SOCK_FAMILY_IPV4,
	                                  ALP_CC3501E_SOCK_TYPE_STREAM,
	                                  0u /* default proto = TCP */,
	                                  &sock,
	                                  TOUR_SOCK_TIMEOUT);
	if (s != ALP_OK) {
		printf("[tour] SOCK_OPEN -> %d\n", (int)s);
	} else {
		printf("[tour] SOCK_OPEN -> handle 0x%04x\n", sock);

		s = cc3501e_sock_connect(fw, sock, TOUR_TCP_IP, TOUR_TCP_PORT, TOUR_SOCK_TIMEOUT);
		if (s != ALP_OK) {
			printf("[tour] SOCK_CONNECT -> %d\n", (int)s);
		} else {
			printf("[tour] SOCK_CONNECT -> %u.%u.%u.%u:%u\n",
			       TOUR_TCP_IP[0],
			       TOUR_TCP_IP[1],
			       TOUR_TCP_IP[2],
			       TOUR_TCP_IP[3],
			       (unsigned)TOUR_TCP_PORT);

			static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
			size_t            sent  = 0u;
			s                       = cc3501e_sock_send(
			    fw, sock, (const uint8_t *)req, sizeof(req) - 1u, &sent, TOUR_SOCK_TIMEOUT);
			printf("[tour] SOCK_SEND -> %d (%u/%u bytes queued)\n",
			       (int)s,
			       (unsigned)sent,
			       (unsigned)(sizeof(req) - 1u));

			/* One receive window: print how many bytes came back (a real client
			 * would loop until a zero-length recv after the peer closes). */
			static uint8_t resp[256];
			size_t         got = 0u;
			s = cc3501e_sock_recv(fw, sock, resp, sizeof(resp), &got, TOUR_SOCK_TIMEOUT);
			printf("[tour] SOCK_RECV -> %d (%u bytes)\n", (int)s, (unsigned)got);
		}
		(void)cc3501e_sock_close(fw, sock, TOUR_SOCK_TIMEOUT);
		printf("[tour] SOCK_CLOSE done\n");
	}

	/* Tear the association back down (radio op; poll-by-repeat internally). */
	s = cc3501e_wifi_disconnect(fw);
	printf("[tour] WIFI_DISCONNECT -> %d\n", (int)s);
}

/*
 * Step: bring BLE up, scan for advertisers, then bring it back down.
 *
 * BLE_ENABLE is worker-routed (it starts the shared-HIF Wi-Fi stack then the
 * NimBLE host, ~seconds) so the budget is generous.  Firmware built WITHOUT
 * BLE answers NOT_READY -- handled gracefully.  BLE_SCAN_START runs a NimBLE
 * GAP discovery for a fixed window and returns the de-duplicated reports.
 */
static void tour_ble(cc3501e_t *fw)
{
	alp_status_t s = cc3501e_ble_enable(fw, TOUR_BLE_TIMEOUT);
	if (s == ALP_ERR_NOT_READY) {
		printf("[tour] BLE_ENABLE -> NOT_READY (firmware built without BLE) -- skipping BLE\n");
		return;
	}
	if (s != ALP_OK) {
		printf("[tour] BLE_ENABLE -> %d\n", (int)s);
		return;
	}
	printf("[tour] BLE_ENABLE -> controller + NimBLE host up\n");

	static cc3501e_ble_scan_record_t recs[TOUR_SCAN_MAX];
	size_t                           n = 0u;
	s = cc3501e_ble_scan(fw, recs, TOUR_SCAN_MAX, &n, TOUR_BLE_TIMEOUT);
	if (s == ALP_OK) {
		printf("[tour] BLE_SCAN -> %u advertiser(s)\n", (unsigned)n);
		for (size_t i = 0u; i < n; ++i) {
			printf("   [%u] %02x:%02x:%02x:%02x:%02x:%02x %d dBm \"%s\"\n",
			       (unsigned)i,
			       recs[i].addr[5],
			       recs[i].addr[4],
			       recs[i].addr[3],
			       recs[i].addr[2],
			       recs[i].addr[1],
			       recs[i].addr[0],
			       (int)recs[i].rssi_dbm,
			       recs[i].name);
		}
	} else {
		printf("[tour] BLE_SCAN -> %d\n", (int)s);
	}

	s = cc3501e_ble_disable(fw, TOUR_BLE_TIMEOUT);
	printf("[tour] BLE_DISABLE -> %d\n", (int)s);
}

/*
 * Step: read a CC3501E-proxied E1M IO through the portable GPIO API.
 *
 * With CONFIG_ALP_SDK_GPIO_CC3501E_PROXY on and the SoM route table populated
 * (src/cc3501e_gpio_routes.c), alp_gpio_open(ALP_E1M_GPIO_IOxx) for a mapped IO
 * routes over the inter-chip bridge (cc3501e_gpio_configure/_read) while every
 * other pin delegates to the platform GPIO driver -- the app code is identical
 * either way.  IO15 (S_BMI323.INT1) is a proxied input in the route table.
 */
static void tour_gpio_proxy(void)
{
	alp_gpio_t *io = alp_gpio_open(ALP_E1M_GPIO_IO15);
	if (io == NULL) {
		printf("[tour] GPIO proxy: alp_gpio_open(IO15) -> not available on this board\n");
		return;
	}
	(void)alp_gpio_configure(io, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);
	bool level = false;
	if (alp_gpio_read(io, &level) == ALP_OK) {
		printf("[tour] GPIO proxy: IO15 (over the bridge) reads %d\n", (int)level);
	} else {
		printf("[tour] GPIO proxy: IO15 read failed (err=%d)\n", (int)alp_last_error());
	}
	alp_gpio_close(io);
}

int main(void)
{
	printf("\n[tour] E1M-AEN CC3501E companion full-surface tour\n");

	/*
	 * Step 1 -- bring up the SoM's CC3501E in ONE call.  cc3501e_bridge_bringup()
	 * opens the inter-chip SPI + the WIFI_EN/nRESET control pins, binds them,
	 * attaches the GPIO proxy, and runs the power+reset sequence.  Everything
	 * after this uses the portable cc3501e_* + alp_gpio_* surfaces.
	 */
	cc3501e_t    fw;
	alp_status_t s = cc3501e_bridge_bringup(&fw);
	if (s == ALP_ERR_NOT_PRESENT_ON_THIS_SOC) {
		printf("[tour] bridge bring-up failed (SPI bus %u / WIFI_EN+nRESET absent? err=%d) -- "
		       "check the board overlay\n",
		       CC3501E_BRIDGE_SPI_BUS_ID,
		       (int)alp_last_error());
		return 0;
	}
	printf("[tour] bridge bring-up -> %d%s\n",
	       (int)s,
	       (s == ALP_ERR_NOSUPPORT) ? " (control pins not bound?)" : "");

	/* Step 2 -- liveness.  Every later step is non-fatal, but if the link never
	 * comes up they will all just log errors, so surface it clearly here. */
	if (!tour_ping(&fw)) {
		printf("[tour] link down -- the remaining steps will report errors; continuing so "
		       "the call shapes are still shown\n");
	}

	/* Step 3 -- version + diagnostics. */
	tour_diag(&fw);

	/* Step 4 -- Wi-Fi scan (poll-by-repeat worker seam). */
	tour_wifi_scan(&fw);

	/* Step 5 -- Wi-Fi connect + a TCP socket round-trip + disconnect
	 * (skipped when no credentials are built in). */
	tour_wifi_connect_and_socket(&fw);

	/* Step 6 -- BLE enable -> scan -> disable. */
	tour_ble(&fw);

	/* Step 7 -- a proxied-GPIO read over the bridge. */
	tour_gpio_proxy();

	printf("[tour] full-surface tour complete\n");
	return 0;
}
