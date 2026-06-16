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

#include <zephyr/kernel.h>
#include <zephyr/fatal.h>

#include "alp/peripheral.h"
#include "alp/chips/cc3501e.h"

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
#define CC3501E_PIN_NRST 1u    /* Alif P15_1_FLEX -- CC3501E reset       */

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
#define CC3501E_SPI_FREQ_HZ 8000000u

/* How long to keep retrying the first PING before falling through to the
 * soak loop anyway (the soak loop keeps logging, so a console-attached
 * run still shows whether the link ever comes up). */
#define CC3501E_PING_RETRIES 25u
#define CC3501E_PING_GAP_MS 200u

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
	uint32_t magic;        /* 0x35334343 once main() starts            */
	uint32_t reset_status; /* (uint32_t)alp_status_t from cc3501e_reset */
	uint32_t ping_ok;      /* count of successful PINGs                 */
	uint32_t ping_fail;    /* count of failed PINGs                     */
	uint32_t last_status;  /* (uint32_t)alp_status_t of the last PING   */
	uint32_t version;      /* protocol version (low 16b) | status<<16   */
	uint32_t phase;        /* progress checkpoint (see CC3501E_PHASE_*)  */
} cc3501e_witness_t;

/* Progress checkpoints written to g_cc3501e_witness.phase so a J-Link can
 * localise where the app got to (read after a fault: .bss survives a halt).
 * 1=entered main, 2=GPIOs configured, 3=SPI opened, 4=reset done,
 * 5=in PING-retry loop, 6=version read, 7=in soak loop. */
#define CC3501E_PHASE_MAIN 1u
#define CC3501E_PHASE_GPIO 2u
#define CC3501E_PHASE_SPI_OPEN 3u
#define CC3501E_PHASE_RESET 4u
#define CC3501E_PHASE_PING 5u
#define CC3501E_PHASE_VERSION 6u
#define CC3501E_PHASE_SOAK 7u

#define CC3501E_WITNESS_MAGIC 0x35334343u /* "CC35" little-endian */

volatile cc3501e_witness_t g_cc3501e_witness __attribute__((used));

/* BRING-UP DIAGNOSTIC (temporary): capture the fatal reason + the stacked
 * exception frame's PC/LR/xPSR into the witness so a J-Link can read exactly
 * where a fault hit, with no console.  last_status = 0xFA0000<reason>;
 * ping_ok=LR, ping_fail=faulting PC, version=xPSR (those counters are 0 on a
 * fault anyway).  Remove once the link is up. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	g_cc3501e_witness.last_status = 0xFA000000u | (reason & 0xFFu);
	if (esf != NULL) {
		const uint32_t *f           = (const uint32_t *)esf;
		g_cc3501e_witness.ping_ok   = f[5]; /* LR  (basic ARM frame) */
		g_cc3501e_witness.ping_fail = f[6]; /* PC  (faulting addr)   */
		g_cc3501e_witness.version   = f[7]; /* xPSR                  */
	}
	for (;;) {
		__asm__ volatile("nop");
	}
}

/* Send a bare PING (META opcode 0x00).  The reply payload is just the
 * status byte -- no data -- so we pass a NULL/0 receive buffer.  Returns
 * the driver status: ALP_OK means the coprocessor parsed the frame and
 * answered in lockstep. */
static alp_status_t cc3501e_ping(cc3501e_t *fw)
{
	return cc3501e_request(fw, ALP_CC3501E_CMD_PING, NULL, 0, NULL, 0, NULL, 100u);
}

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
	       diag.fw_version, diag.reset_cause, diag.role, diag.uptime_ms, diag.free_heap_bytes,
	       diag.last_error);
}

int main(void)
{
	printf("\n[cc3501e-bringup] E1M-AEN CC3501E Wi-Fi/BLE coprocessor bring-up\n");
	g_cc3501e_witness.magic = CC3501E_WITNESS_MAGIC; /* marks the struct found over SWD */
	g_cc3501e_witness.phase = CC3501E_PHASE_MAIN;

	/*
	 * Step 1 -- claim the two control GPIOs and make them outputs.
	 *
	 * cc3501e_reset() only WRITES these pins (it does not configure
	 * direction), so we must set them to OUTPUT here first.  If either
	 * cannot be opened the chip can never be powered, so bail loudly --
	 * there is nothing useful to do without WIFI_EN.
	 */
	alp_gpio_t *wifi_en = alp_gpio_open(CC3501E_PIN_WIFI_EN);
	alp_gpio_t *nrst    = alp_gpio_open(CC3501E_PIN_NRST);
	if (wifi_en == NULL || nrst == NULL) {
		/* BRING-UP DIAGNOSTIC: record which open failed + the last error into
		 * the witness (no console).  reset_status = 0xE000_0000 | wifi<<8 |
		 * nrst<<9 | (alp_last_error & 0xFF). */
		g_cc3501e_witness.reset_status = 0xE0000000u | ((wifi_en == NULL) ? 0x100u : 0u) |
		                                 ((nrst == NULL) ? 0x200u : 0u) |
		                                 ((uint32_t)(uint8_t)alp_last_error());
		printf("[cc3501e-bringup] alp_gpio_open failed (WIFI_EN=%p NRST=%p, err=%d) -- "
		       "check the alp,pin-array in the board overlay\n",
		       (void *)wifi_en, (void *)nrst, (int)alp_last_error());
		return 0;
	}
	(void)alp_gpio_configure(wifi_en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	(void)alp_gpio_configure(nrst, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	g_cc3501e_witness.phase = CC3501E_PHASE_GPIO;

	/*
	 * Step 2 -- open the inter-chip SPI bus (Alif = master).
	 *
	 * ALP_SPI_NO_CS: this HW rev wires no chip-select; the driver clocks
	 * the protocol as fixed-count lockstep transfers instead of framing on
	 * a CS edge.  Passing a CS pin here would (wrongly) hand a pad to the
	 * generic gpio-CS path.
	 */
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = CC3501E_SPI_BUS_ID,
	    .freq_hz       = CC3501E_SPI_FREQ_HZ,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8u,
	    .cs_pin_id     = ALP_SPI_NO_CS,
	});
	if (spi == NULL) {
		printf("[cc3501e-bringup] alp_spi_open(bus %u) failed: err=%d -- check the "
		       "alp-spi1 alias / SPI1 node in the board overlay\n",
		       CC3501E_SPI_BUS_ID, (int)alp_last_error());
		alp_gpio_close(wifi_en);
		alp_gpio_close(nrst);
		return 0;
	}
	g_cc3501e_witness.phase = CC3501E_PHASE_SPI_OPEN;

	/*
	 * Step 3 -- bind the driver and run the reset/power sequence.
	 *
	 * cc3501e_init() only records the bus; we then hand it the two control
	 * pins so cc3501e_reset() can sequence WIFI_EN + nRESET per TI SWRU626
	 * (assert reset, gate rails down, raise WIFI_EN, ~5 ms rail ramp,
	 * release nRESET, ~900 ms boot budget).  reset() blocks ~905 ms and
	 * leaves WIFI_EN HIGH (the chip stays powered).
	 */
	cc3501e_t fw;
	(void)cc3501e_init(&fw, spi);
	fw.enable_pin = wifi_en;
	fw.reset_pin  = nrst;

	printf("[cc3501e-bringup] powering + resetting CC3501E (WIFI_EN high, nRESET pulse, "
	       "~900 ms boot)...\n");
	alp_status_t s                 = cc3501e_reset(&fw);
	g_cc3501e_witness.reset_status = (uint32_t)s;
	g_cc3501e_witness.phase        = CC3501E_PHASE_RESET;
	printf("[cc3501e-bringup] cc3501e_reset -> %d%s\n", (int)s,
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
			printf("[cc3501e-bringup] PING ok after %u attempt%s\n", attempt + 1u,
			       (attempt == 0u) ? "" : "s");
			up = true;
			break;
		}
		printf("[cc3501e-bringup] PING attempt %u -> %d (not ready yet?) -- retrying in %u ms\n",
		       attempt, (int)s, CC3501E_PING_GAP_MS);
		k_msleep(CC3501E_PING_GAP_MS);
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
		printf("[cc3501e-bringup] GET_VERSION -> protocol v%u (host expects v%u)%s\n", version,
		       ALP_CC3501E_PROTOCOL_VERSION,
		       (version == ALP_CC3501E_PROTOCOL_VERSION) ? " -- match" : " -- MISMATCH!");
	} else {
		printf("[cc3501e-bringup] GET_VERSION -> %d\n", (int)s);
	}

	/* Step 6 -- extended diagnostics (v2-firmware; v0.1 rejects cleanly). */
	cc3501e_dump_diag(&fw);

	/*
	 * Step 7 -- liveness soak.  Keep PINGing so the link is continuously
	 * verifiable over J-Link, and re-read the version every 8th cycle (an
	 * odd-length reply that stresses the framing residue handling).  This
	 * mirrors the v2n-gd32-bridge-ping soak.
	 */
	printf("[cc3501e-bringup] entering liveness soak (PING every 500 ms)\n");
	g_cc3501e_witness.phase = CC3501E_PHASE_SOAK;
	for (uint32_t i = 0u;; ++i) {
		s                             = cc3501e_ping(&fw);
		g_cc3501e_witness.last_status = (uint32_t)s;
		if (s == ALP_OK) {
			g_cc3501e_witness.ping_ok++;
		} else {
			g_cc3501e_witness.ping_fail++;
		}
		printf("[cc3501e-bringup] soak PING #%u -> %d\n", i, (int)s);

		if ((i % 8u) == 0u) {
			uint16_t     v            = 0u;
			alp_status_t vs           = cc3501e_get_version(&fw, &v);
			g_cc3501e_witness.version = (uint32_t)v | ((uint32_t)(uint8_t)vs << 16);
			printf("[cc3501e-bringup] soak GET_VERSION #%u -> %d (v%u)\n", i, (int)vs, v);
		}
		k_msleep(500);
	}

	/* not reached */
	alp_spi_close(spi);
	alp_gpio_close(wifi_en);
	alp_gpio_close(nrst);
	return 0;
}
