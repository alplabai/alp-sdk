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
 *
 * The current E1M-AEN rev wires only SCLK/MOSI/MISO -- NO chip-select and
 * NO host-IRQ line (both arrive next rev).  The link is therefore clocked
 * as a sequence of deterministic, fixed-count transfers in lockstep; the
 * framing lives in the host driver (chips/cc3501e/cc3501e.c) and its
 * mirror on the firmware side (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c).
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

/*
 * 8 MHz for first bring-up.  A slave cannot pace SCK, so the safe move is
 * to start slow and raise the clock only once the lockstep is proven on
 * silicon.  (The host driver's settle gap before each reply read is sized
 * for the v0.1 instant-dispatch META path; slow Wi-Fi/BLE replies need the
 * next-rev host-IRQ line, not a faster clock.)
 */
#define CC3501E_SPI_FREQ_HZ                                                                        \
	1000000u /* 1 MHz: SILICON-VALIDATED cold-boot value.  At 8 MHz with the
                                      * Alif SSI rx_delay=0 the master sampled MISO before the CC35's bit
                                      * propagated back over the long on-SoM traces + the crossed-data
                                      * bodge -> reqhdr_rx=0xFFFFFFFF, cold link dead (8 MHz was only
                                      * marginally OK warm).  At 1 MHz the bit period (1 us) >> the MISO
                                      * round-trip, so sampling is clean and cold-boot GET_MAC works
                                      * end-to-end.  Plenty for the control/PING path (radio data rides
                                      * Wi-Fi, not this bridge).  FUTURE: to raise the clock, set the SSI
                                      * rx-delay (alif,dwc-ssi rx-delay DT prop) to cover the round-trip. */

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
 * answered in lockstep. (This example previously carried a local copy; it now
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
	uint8_t      raw[sizeof(alp_cc3501e_diag_info_t)] = { 0 };
	size_t       got                                  = 0;
	alp_status_t s =
	    cc3501e_request(fw, ALP_CC3501E_CMD_GET_DIAG_INFO, NULL, 0, raw, sizeof(raw), &got, 100u);

	if (s == ALP_ERR_INVAL) {
		printf("[cc3501e-bringup] GET_DIAG_INFO -> rejected (v0.1 firmware; "
		       "v2-only command) -- expected\n");
		return;
	}
	if (s != ALP_OK) {
		printf("[cc3501e-bringup] GET_DIAG_INFO -> %d\n", (int)s);
		return;
	}
	if (got < sizeof(alp_cc3501e_diag_info_t)) {
		printf("[cc3501e-bringup] GET_DIAG_INFO -> short reply (%u B)\n", (unsigned)got);
		return;
	}

	/* Both cores are little-endian ARM and the struct is naturally aligned
	 * (matches the packed wire layout), so a straight copy is safe. */
	alp_cc3501e_diag_info_t diag;
	memcpy(&diag, raw, sizeof(diag));
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
		printf("[cc3501e-bringup] bridge bring-up failed (SPI bus %u / WIFI_EN+nRESET "
		       "absent? err=%d) -- check the board overlay\n",
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
	 * staged its reply in lockstep -- the core thing this bring-up checks.
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
	 * the radio bring-up before the PING link is established, and Wlan_Start
	 * disrupts the CS-less link before it has settled (cold first-contact never
	 * recovers).  The soak below reads the MAC only AFTER the link is solidly up
	 * (ping_ok >= threshold) -- "wait until ready to read".  Kept the function for
	 * the scan/connect path, gated the same way later. */
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

		s                             = cc3501e_ping(&fw);
		g_cc3501e_witness.last_status = (uint32_t)s;
		if (s == ALP_OK) {
			g_cc3501e_witness.ping_ok++;
		} else {
			g_cc3501e_witness.ping_fail++;
		}
		printf("[cc3501e-bringup] soak PING #%u -> %d\n", i, (int)s);

		/* Once the link is aligned (PING ok), keep retrying GET_MAC until it
		 * lands -- the boot-time probe can run during the CS-less cold
		 * first-contact misalignment window; retrying here lands the
		 * worker-routed Wi-Fi identity read end-to-end on the stable link. */
		if (g_cc3501e_witness.mac_ok == 0u && s == ALP_OK && g_cc3501e_witness.ping_ok >= 20u) {
			uint8_t      mac[CC3501E_MAC_LEN] = { 0 };
			alp_status_t ms              = cc3501e_wifi_get_mac(&fw, mac, CC3501E_MAC_TIMEOUT_MS);
			g_cc3501e_witness.mac_status = (uint32_t)ms;
			if (ms == ALP_OK) {
				g_cc3501e_witness.mac_ok = 1u;
				g_cc3501e_witness.mac_lo = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
				                           ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
				g_cc3501e_witness.mac_hi = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8);
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
