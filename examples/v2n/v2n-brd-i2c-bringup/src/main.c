/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-brd-i2c-bringup -- patch-day diagnostic for the V2N SoM's
 * BRD_I2C management bus.
 *
 * BRD_I2C is the SoM's housekeeping bus (Renesas RIIC8, master pads
 * P07/P06).  Eight ICs share it -- see the authoritative table in
 * metadata/e1m_modules/E1M-V2N101.yaml:
 *
 *   0x1E  da9292          secondary PMIC (DEEPX rail on V2N-M1)
 *   0x25  act8760 ADD1    primary PMIC: system + Buck1..6 + GPIOs
 *   0x26  act8760 ADD2    primary PMIC: Buck7 + LDO1..6
 *   0x30  optiga_trust_m  secure element
 *   0x40  tmp112          temp sensor  (metadata addr -- but see below!)
 *   0x4D  tps628640       LPDDR4X 0.6 V buck (assembly OPTION)
 *   0x52  rv3028c7        RTC
 *   0x68  clk_5l35023b    clock generator
 *   0x70  gd32g553        IO-MCU bridge, I2C slave transport
 *
 * The flow:
 *   Phase 0 -- bus health: full 0x08..0x77 scan.  Zero ACKs anywhere
 *              means a BUS-level fault (a line held low, missing
 *              pull-ups, wrong pinmux) rather than missing chips;
 *              the report says so explicitly instead of printing
 *              nine cryptic per-device NAKs.
 *   Phase 1 -- per-IC probe, strictly READ-ONLY toward the PMICs:
 *              nothing in this example ever writes a voltage, enable,
 *              or control register.  (The RTC init does clear its
 *              power-on flag and select 24 h mode -- that is the
 *              documented, side-effect-free bring-up handshake.)
 *
 * TMP112 address note: the SoM metadata says 0x40, but TI's TMP112
 * only decodes 0x48..0x4B (ADD0 strap).  Until silicon settles the
 * question this example probes BOTH and reports which one ACKs --
 * whichever way it lands, fix metadata or BOM, not this file first.
 */

#include <stdarg.h>
#include <stdio.h>
#include <zephyr/kernel.h>

#include <alp/peripheral.h>
#include <alp/chips/rv3028c7.h>
#include <alp/chips/tmp112.h>
#include <alp/chips/clk_5l35023b.h>
#include <alp/chips/act8760.h>
#include <alp/chips/da9292.h>
#include <alp/chips/tps628640.h>
#include <alp/chips/optiga_trust_m.h>
#include <alp/chips/gd32g553.h>

/* One row of the final report. */
typedef enum { R_PASS, R_FAIL, R_SKIP } result_t;

struct report_row {
	const char *name;
	uint8_t     addr;
	result_t    result;
	char        detail[64];
};

#define ROW_MAX 10
static struct report_row rows[ROW_MAX];
static int               row_count;

static void report(const char *name, uint8_t addr, result_t res, const char *fmt, ...)
{
	if (row_count >= ROW_MAX) {
		return;
	}
	struct report_row *r = &rows[row_count++];
	r->name   = name;
	r->addr   = addr;
	r->result = res;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
	va_end(ap);
}

/* A 1-byte read is the least-invasive ACK probe: every chip on this
 * bus tolerates a register-pointer read, and unlike a write it can
 * never alter device state.  The portable backend maps a NAK (or any
 * bus fault) to ALP_ERR_IO. */
static bool acks(alp_i2c_t *bus, uint8_t addr)
{
	uint8_t b = 0;
	return alp_i2c_read(bus, addr, &b, 1) == ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 0: bus health                                                 */
/* ------------------------------------------------------------------ */

static int scan_bus(alp_i2c_t *bus)
{
	int hits = 0;

	printk("Phase 0: scanning 0x08..0x77 ...\n");
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (acks(bus, a)) {
			printk("  ACK at 0x%02X\n", a);
			hits++;
		}
	}
	if (hits == 0) {
		/* The signature of the bus-level fault this example exists
		 * to diagnose: with pull-ups healthy and the mux right, at
		 * least the PMICs always ACK (they are powered whenever the
		 * SoM runs at all).  Zero ACKs = electrical problem. */
		printk("  !! ZERO devices ACK.  This is a BUS-level fault:\n");
		printk("     - a device or short holding SDA/SCL low,\n");
		printk("     - missing/disconnected pull-ups, or\n");
		printk("     - the RIIC8 pinmux not selected on P07/P06.\n");
		printk("     Scope the lines before trusting any result below.\n");
	} else {
		printk("  %d device(s) ACK.\n", hits);
	}
	return hits;
}

/* ------------------------------------------------------------------ */
/* Phase 1: per-IC probes (read-only)                                  */
/* ------------------------------------------------------------------ */

static void probe_rtc(alp_i2c_t *bus)
{
	rv3028c7_t rtc;

	if (rv3028c7_init(&rtc, bus) != ALP_OK) {
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL,
		       "no ACK / status read failed");
		return;
	}
	rv3028c7_time_t t;
	if (rv3028c7_get_time(&rtc, &t) == ALP_OK) {
		/* A wildly implausible year usually means the backup supply
		 * never charged -- worth knowing on first power-up. */
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_PASS,
		       "%04u-%02u-%02u %02u:%02u:%02u%s",
		       t.year, t.month, t.day,
		       t.hour, t.minute, t.second,
		       (t.year < 2026 || t.year > 2099) ? " (time not set)" : "");
	} else {
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL,
		       "probe OK but time read failed");
	}
	rv3028c7_deinit(&rtc);
}

static void probe_tmp112(alp_i2c_t *bus)
{
	/* Address discrepancy under test: metadata says 0x40, the TMP112
	 * datasheet says 0x48..0x4B.  Find where (and whether) it ACKs. */
	const uint8_t candidates[] = {0x40, TMP112_I2C_ADDR_GND, 0x49, 0x4A, 0x4B};
	uint8_t       found        = 0;

	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (acks(bus, candidates[i])) {
			found = candidates[i];
			break;
		}
	}
	if (found == 0) {
		report("tmp112 temp", 0x40, R_FAIL,
		       "no ACK at 0x40 nor 0x48..0x4B");
		return;
	}
	if (found == 0x40) {
		/* ACKs at the metadata address -- but the driver (faithfully
		 * to the datasheet) refuses 0x40.  Surface the conflict; do
		 * NOT quietly loosen the driver. */
		report("tmp112 temp", 0x40, R_FAIL,
		       "ACKs at 0x40 -- not a TMP112 address; fix metadata or BOM");
		return;
	}
	tmp112_t sens;

	if (tmp112_init(&sens, bus, found) != ALP_OK) {
		report("tmp112 temp", found, R_FAIL,
		       "ACKs but CONF fingerprint mismatch");
		return;
	}
	int32_t mc = 0;

	if (tmp112_read_temp_milli_c(&sens, &mc) == ALP_OK) {
		report("tmp112 temp", found, R_PASS,
		       "%d.%03d degC",
		       (int)(mc / 1000),
		       (int)((mc < 0 ? -mc : mc) % 1000));
	} else {
		report("tmp112 temp", found, R_FAIL, "temperature read failed");
	}
	tmp112_deinit(&sens);
}

static void probe_clkgen(alp_i2c_t *bus)
{
	clk_5l35023b_t clk;

	if (clk_5l35023b_init(&clk, bus, CLK_5L35023B_I2C_ADDR_DEFAULT) != ALP_OK) {
		report("5l35023b clk", CLK_5L35023B_I2C_ADDR_DEFAULT, R_FAIL,
		       "no ACK or address-strap mismatch");
		return;
	}
	uint8_t dash = 0;

	(void)clk_5l35023b_read_dashcode_id(&clk, &dash);
	report("5l35023b clk", CLK_5L35023B_I2C_ADDR_DEFAULT, R_PASS,
	       "dash code 0x%02X", dash);
	clk_5l35023b_deinit(&clk);
}

static void probe_act8760(alp_i2c_t *bus)
{
	act8760_t pmic;

	if (act8760_init(&pmic, bus) != ALP_OK) {
		report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL,
		       "ADD1 (0x25) or ADD2 (0x26) missing");
		return;
	}
	act8760_status_t st;

	if (act8760_get_status(&pmic, &st) == ALP_OK) {
		report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_PASS,
		       "both slaves; status 0x%02X%s%s", st.raw,
		       st.thermal_warning ? " TWARN!" : "",
		       st.vsys_warning    ? " VSYSWARN!" : "");
	} else {
		report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL,
		       "status read failed");
	}
	act8760_deinit(&pmic);
}

static void probe_da9292(alp_i2c_t *bus)
{
	da9292_t pmic;

	if (da9292_init(&pmic, bus, DA9292_I2C_ADDR_V2N) != ALP_OK) {
		report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL,
		       "no ACK / bad DEV_ID");
		return;
	}
	da9292_status_t st;

	if (da9292_get_status(&pmic, &st) == ALP_OK) {
		report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_PASS,
		       "dev 0x%02X rev 0x%02X  CH1 PG=%d  CH2 PG=%d",
		       pmic.dev_id, pmic.rev_id, st.ch1_pg, st.ch2_pg);
	} else {
		report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL,
		       "status read failed");
	}
	da9292_deinit(&pmic);
}

static void probe_tps628640(alp_i2c_t *bus)
{
	/* Assembly option: absent on most V2N base builds.  A NAK here
	 * is expected, not a failure. */
	if (!acks(bus, 0x4D)) {
		report("tps628640", 0x4D, R_SKIP,
		       "not populated (assembly option)");
		return;
	}
	tps628640_t buck;

	if (tps628640_init(&buck, bus, 0x4D, 600) != ALP_OK) {
		report("tps628640", 0x4D, R_FAIL,
		       "ACKs but VOUT1 read failed");
		return;
	}
	uint16_t mv = 0;

	(void)tps628640_get_voltage_mv(&buck, &mv);
	report("tps628640", 0x4D, R_PASS, "VOUT1 = %u mV", mv);
	tps628640_deinit(&buck);
}

static void probe_optiga(alp_i2c_t *bus)
{
	optiga_trust_m_t se;

	if (optiga_trust_m_init(&se, bus, OPTIGA_TRUST_M_I2C_ADDR) != ALP_OK) {
		report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_FAIL,
		       "no ACK on I2C_STATE");
		return;
	}
	report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_PASS,
	       "I2C_STATE readable");
	optiga_trust_m_deinit(&se);
}

static void probe_gd32(alp_i2c_t *bus)
{
	/* The supervisor MCU speaks the bridge protocol over BRD_I2C as
	 * its management transport (SPI is the fast path).  init() runs
	 * PING + GET_VERSION and enforces the protocol-major match. */
	gd32g553_t mcu;

	if (gd32g553_init(&mcu, NULL, bus, GD32G553_BRIDGE_DEFAULT_I2C_ADDR) != ALP_OK) {
		report("gd32g553 bridge", GD32G553_BRIDGE_DEFAULT_I2C_ADDR, R_FAIL,
		       "PING/GET_VERSION failed (firmware running? major match?)");
		return;
	}
	report("gd32g553 bridge", GD32G553_BRIDGE_DEFAULT_I2C_ADDR, R_PASS,
	       "fw v%u.%u.%u over I2C",
	       mcu.version.major, mcu.version.minor, mcu.version.patch);
	gd32g553_deinit(&mcu);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printk("\n=== V2N BRD_I2C bring-up diagnostic ===\n");

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
		.bus_id     = 0u,      /* BRD_I2C = bus 0 on the V2N M33 target */
		.bitrate_hz = 400000u, /* every IC on this bus is FM-capable    */
	});
	if (bus == NULL) {
		printk("FATAL: alp_i2c_open(bus 0) failed -- check the board "
		       "overlay wires the alp-i2c0 alias.\n");
		return 1;
	}

	int hits = scan_bus(bus);

	printk("\nPhase 1: per-IC probes (read-only)\n");
	probe_rtc(bus);
	probe_tmp112(bus);
	probe_clkgen(bus);
	probe_act8760(bus);
	probe_da9292(bus);
	probe_tps628640(bus);
	probe_optiga(bus);
	probe_gd32(bus);

	printk("\n==== BRD_I2C report ====\n");
	printk("%-16s %-5s %-5s %s\n", "device", "addr", "res", "detail");
	int fails = 0;

	for (int i = 0; i < row_count; i++) {
		const char *res = rows[i].result == R_PASS ? "PASS"
				: rows[i].result == R_SKIP ? "SKIP" : "FAIL";

		if (rows[i].result == R_FAIL) {
			fails++;
		}
		printk("%-16s 0x%02X  %-5s %s\n",
		       rows[i].name, rows[i].addr, res, rows[i].detail);
	}
	printk("========================\n");
	if (hits == 0) {
		printk("VERDICT: bus-level fault -- fix the electrical problem first.\n");
	} else if (fails == 0) {
		printk("VERDICT: BRD_I2C fully alive.\n");
	} else {
		printk("VERDICT: bus alive, %d device(s) failing -- see rows above.\n",
		       fails);
	}

	alp_i2c_close(bus);
	return 0;
}
