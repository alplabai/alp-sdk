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
 *   0x40  tmp112          temp sensor (maintainer-confirmed 2026-09-24)
 *   0x4D  tps628640       LPDDR4X 0.6 V buck (assembly OPTION)
 *   0x52  rv3028c7        RTC (Linux kernel-bound: rtc-rv3028)
 *   0x69  clk_5l35023b    clock generator (maintainer-confirmed 2026-09-24)
 *   0x70  gd32g553        IO-MCU bridge, I2C slave transport (Linux
 *                         kernel-bound: alplab,gd32-bridge-gpio)
 *
 * RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive (metadata/e1m_modules/
 * v2n/core-ownership.yaml) -- the CM33 must never master it.  This is
 * a Linux/Yocto user-space app on the V2N Cortex-A55, following the
 * same `alp_i2c_*` + chip-driver pattern as
 * examples/v2n/v2n-power-monitor.  BRD_I2C = Linux /dev/i2c-8:
 * meta-alp-sdk's e1m-v2n-som.dtsi aliases `i2c8 = &i2c8;`.
 *
 * KNOWN LIMITATION on a running target: the kernel already binds real
 * drivers to 0x52 (rv3028 RTC, bound as rtc0) and 0x70
 * (alplab,gd32-bridge-gpio) -- see meta-alp-sdk's e1m-v2n-som.dtsi
 * `&i2c8` node.  A userspace i2c-dev transaction to an address a
 * kernel driver already owns fails with EBUSY, but only via the
 * I2C_SLAVE ioctl (src/yocto/peripheral_i2c.c) -- the raw I2C_RDWR
 * path a chip driver's own protocol uses does not see that
 * ownership and will never return EBUSY.  Both probes below
 * therefore front-load a plain probe_addr() read (I2C_SLAVE) before
 * ever touching the chip's own protocol, and report SKIP
 * (kernel-owned), not FAIL -- `probe_rtc()` then reads the live time
 * through /dev/rtc0 instead.
 *
 * The flow:
 *   Phase 0 -- bus health: full 0x08..0x77 scan.  Zero ACKs anywhere
 *              means a BUS-level fault (a line held low, missing
 *              pull-ups, wrong pinmux) rather than missing chips;
 *              the report says so explicitly instead of printing
 *              nine cryptic per-device NAKs.  A kernel-owned address
 *              (EBUSY) is reported as present, not as a NAK.
 *   Phase 1 -- per-IC probe, strictly READ-ONLY: nothing in this
 *              example ever writes a voltage, enable, or control
 *              register, and the RTC is read through /dev/rtc0 rather
 *              than the chip driver's own init handshake (which would
 *              both EBUSY against the kernel and, if it ever won that
 *              race, clear the RTC's power-on flag as a side effect --
 *              see probe_rtc()).
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <alp/peripheral.h>
#include <alp/chips/rv3028c7.h>
#include <alp/chips/tmp112.h>
#include <alp/chips/clk_5l35023b.h>
#include <alp/chips/act8760.h>
#include <alp/chips/da9292.h>
#include <alp/chips/tps628640.h>
#include <alp/chips/optiga_trust_m.h>
#include <alp/chips/gd32g553.h>

/* BRD_I2C = Linux /dev/i2c-8 -- see the file header for the DT-alias
 * citation. */
#define V2N_BRD_I2C_BUS_ID 8u

/* One row of the final report. */
typedef enum { R_PASS, R_FAIL, R_SKIP } result_t;

struct report_row {
	const char *name;
	uint8_t     addr;
	result_t    result;
	char        detail[96]; /* long enough for the PROBE_ACK/no-kernel-driver
	                          * sentence below without vsnprintf truncation */
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
	r->name              = name;
	r->addr              = addr;
	r->result            = res;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
	va_end(ap);
}

/* A 1-byte read is the least-invasive ACK probe: every chip on this
 * bus tolerates a register-pointer read, and unlike a write it can
 * never alter device state.  Distinguishes a real NAK from EBUSY (the
 * kernel already owns this address via I2C_SLAVE -- src/yocto/
 * peripheral_i2c.c) so a kernel-owned chip is never misreported as
 * absent. */
typedef enum { PROBE_NAK, PROBE_ACK, PROBE_BUSY } probe_result_t;

static probe_result_t probe_addr(alp_i2c_t *bus, uint8_t addr)
{
	uint8_t      b = 0;
	alp_status_t s = alp_i2c_read(bus, addr, &b, 1);
	if (s == ALP_OK) return PROBE_ACK;
	if (s == ALP_ERR_BUSY) return PROBE_BUSY;
	return PROBE_NAK;
}

static bool acks(alp_i2c_t *bus, uint8_t addr)
{
	return probe_addr(bus, addr) == PROBE_ACK;
}

/* ------------------------------------------------------------------ */
/* Phase 0: bus health                                                 */
/* ------------------------------------------------------------------ */

static int scan_bus(alp_i2c_t *bus)
{
	int hits = 0;

	printf("Phase 0: scanning 0x08..0x77 ...\n");
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		probe_result_t r = probe_addr(bus, a);
		if (r == PROBE_ACK) {
			printf("  ACK at 0x%02X\n", a);
			hits++;
		} else if (r == PROBE_BUSY) {
			printf("  0x%02X kernel-owned (EBUSY) -- present, see per-IC probe\n", a);
			hits++;
		}
	}
	if (hits == 0) {
		/* The signature of the bus-level fault this example exists
		 * to diagnose: with pull-ups healthy and the mux right, at
		 * least the PMICs always ACK (they are powered whenever the
		 * SoM runs at all).  Zero ACKs = electrical problem. */
		printf("  !! ZERO devices ACK.  This is a BUS-level fault:\n");
		printf("     - a device or short holding SDA/SCL low,\n");
		printf("     - missing/disconnected pull-ups, or\n");
		printf("     - the RIIC8 pinmux not selected on P07/P06.\n");
		printf("     Scope the lines before trusting any result below.\n");
	} else {
		printf("  %d device(s) ACK.\n", hits);
	}
	return hits;
}

/* ------------------------------------------------------------------ */
/* Phase 1: per-IC probes (read-only)                                  */
/* ------------------------------------------------------------------ */

static void probe_rtc(alp_i2c_t *bus)
{
	/* Never call rv3028c7_init() here: on a running target the kernel
	 * already owns 0x52 (rtc-rv3028, bound as rtc0), so init() would
	 * just EBUSY at the I2C_SLAVE ioctl -- and if it ever won that
	 * race instead, it clears the RTC's power-on flag as a bring-up
	 * side effect (chips/rv3028c7/rv3028c7.c) this read-only
	 * diagnostic must not have.  Read the live time through the
	 * kernel's rtc0 device instead.
	 *
	 * Exactly one row per device: PROBE_NAK/PROBE_ACK report and
	 * return immediately; only PROBE_BUSY (kernel-owned) falls
	 * through to /dev/rtc0, which then emits its own single row. */
	probe_result_t r = probe_addr(bus, RV3028C7_I2C_ADDR);
	if (r == PROBE_NAK) {
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL, "no ACK");
		return;
	}
	if (r == PROBE_ACK) {
		report("rv3028c7 RTC",
		       RV3028C7_I2C_ADDR,
		       R_FAIL,
		       "RV3028 answers but no kernel RTC driver is bound (rtc-rv3028 "
		       "missing from the DT/kernel)");
		return;
	}

	/* r == PROBE_BUSY: kernel-owned -- read the live time through rtc0. */
	int fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL, "/dev/rtc0 open: %s", strerror(errno));
		return;
	}
	struct rtc_time rt;
	if (ioctl(fd, RTC_RD_TIME, &rt) < 0) {
		report("rv3028c7 RTC", RV3028C7_I2C_ADDR, R_FAIL, "RTC_RD_TIME: %s", strerror(errno));
		close(fd);
		return;
	}
	close(fd);

	/* A wildly implausible year usually means the backup supply never
	 * charged -- worth knowing on first power-up. */
	int year = rt.tm_year + 1900;
	report("rv3028c7 RTC",
	       RV3028C7_I2C_ADDR,
	       R_PASS,
	       "kernel rtc-rv3028 owns it, %04d-%02d-%02d %02d:%02d:%02d%s",
	       year,
	       rt.tm_mon + 1,
	       rt.tm_mday,
	       rt.tm_hour,
	       rt.tm_min,
	       rt.tm_sec,
	       (year < 2026 || year > 2099) ? " (unset)" : "");
}

static void probe_tmp112(alp_i2c_t *bus)
{
	/* Address confirmed 2026-09-24 (metadata/chips/tmp112.yaml):
	 * ADD0=GND on the fitted TMP112DIDPWR (X2SON-5) straps 0x40, not
	 * the naive-datasheet TMP112_I2C_ADDR_GND=0x48 -- same class of
	 * bug already fixed for AEN (#1978). */
	tmp112_t sens;

	if (tmp112_init(&sens, bus, TMP112_I2C_ADDR_ADDRVAR_GND) != ALP_OK) {
		report("tmp112 temp", TMP112_I2C_ADDR_ADDRVAR_GND, R_FAIL, "no ACK at 0x40");
		return;
	}
	int32_t mc = 0;

	if (tmp112_read_temp_milli_c(&sens, &mc) == ALP_OK) {
		report("tmp112 temp",
		       TMP112_I2C_ADDR_ADDRVAR_GND,
		       R_PASS,
		       "%s%d.%03d degC",
		       (mc < 0) ? "-" : "",
		       (int)((mc < 0 ? -mc : mc) / 1000),
		       (int)((mc < 0 ? -mc : mc) % 1000));
	} else {
		report("tmp112 temp", TMP112_I2C_ADDR_ADDRVAR_GND, R_FAIL, "temperature read failed");
	}
	tmp112_deinit(&sens);
}

static void probe_clkgen(alp_i2c_t *bus)
{
	clk_5l35023b_t clk;

	if (clk_5l35023b_init(&clk, bus, CLK_5L35023B_I2C_ADDR_DEFAULT) != ALP_OK) {
		report("5l35023b clk",
		       CLK_5L35023B_I2C_ADDR_DEFAULT,
		       R_FAIL,
		       "no ACK or address-strap mismatch");
		return;
	}
	uint8_t dash = 0;

	(void)clk_5l35023b_read_dashcode_id(&clk, &dash);
	report("5l35023b clk", CLK_5L35023B_I2C_ADDR_DEFAULT, R_PASS, "dash code 0x%02X", dash);
	clk_5l35023b_deinit(&clk);
}

static void probe_act8760(alp_i2c_t *bus)
{
	act8760_t pmic;

	if (act8760_init(&pmic, bus) != ALP_OK) {
		report(
		    "act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL, "ADD1 (0x25) or ADD2 (0x26) missing");
		return;
	}
	act8760_status_t st;

	if (act8760_get_status(&pmic, &st) == ALP_OK) {
		report("act8760 PMIC",
		       ACT8760_I2C_ADDR_PAGE0,
		       R_PASS,
		       "both slaves; status 0x%02X%s%s",
		       st.raw,
		       st.thermal_warning ? " TWARN!" : "",
		       st.vsys_warning ? " VSYSWARN!" : "");
	} else {
		report("act8760 PMIC", ACT8760_I2C_ADDR_PAGE0, R_FAIL, "status read failed");
	}
	act8760_deinit(&pmic);
}

static void probe_da9292(alp_i2c_t *bus)
{
	/* Read-only here: U-Boot's board_late_init() is the sole writer of
	 * this PMIC's CH2 (DEEPX rail) -- see
	 * meta-alp-sdk/recipes-bsp/u-boot/u-boot/
	 * 0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch.  This probe
	 * only reads DEV_ID/REV_ID + STATUS, matching the "strictly
	 * READ-ONLY toward the PMICs" rule in the file header. */
	da9292_t pmic;

	if (da9292_init(&pmic, bus, DA9292_I2C_ADDR_V2N) != ALP_OK) {
		report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL, "no ACK / bad DEV_ID");
		return;
	}
	da9292_status_t st;

	if (da9292_get_status(&pmic, &st) == ALP_OK) {
		report("da9292 PMIC",
		       DA9292_I2C_ADDR_V2N,
		       R_PASS,
		       "dev 0x%02X rev 0x%02X  CH1 PG=%d  CH2 PG=%d",
		       pmic.dev_id,
		       pmic.rev_id,
		       st.ch1_pg,
		       st.ch2_pg);
	} else {
		report("da9292 PMIC", DA9292_I2C_ADDR_V2N, R_FAIL, "status read failed");
	}
	da9292_deinit(&pmic);
}

static void probe_tps628640(alp_i2c_t *bus)
{
	/* Assembly option: absent on most V2N base builds.  A NAK here
	 * is expected, not a failure. */
	if (!acks(bus, 0x4D)) {
		report("tps628640", 0x4D, R_SKIP, "not populated (assembly option)");
		return;
	}
	tps628640_t buck;

	/* Assembly option: it can ACK yet report not-ready (absent OTP, partially
	 * powered rail, or intentionally not readable in this build).  That is an
	 * optional-BOM condition, not a board failure -- skip, don't fail.  A true
	 * protocol/data error (any other non-OK) stays R_FAIL. */
	alp_status_t st = tps628640_init(&buck, bus, 0x4D, 600);

	if (st == ALP_ERR_NOT_READY) {
		report("tps628640", 0x4D, R_SKIP, "ACKs but not ready (assembly option / partial rail)");
		return;
	}
	if (st != ALP_OK) {
		report("tps628640", 0x4D, R_FAIL, "ACKs but VOUT1 read failed");
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
		report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_FAIL, "no ACK on I2C_STATE");
		return;
	}
	report("optiga trust m", OPTIGA_TRUST_M_I2C_ADDR, R_PASS, "I2C_STATE readable");
	optiga_trust_m_deinit(&se);
}

static void probe_gd32(alp_i2c_t *bus)
{
	/* The supervisor MCU speaks the bridge protocol over BRD_I2C as
	 * its management transport (SPI is the fast path).  init() runs
	 * PING + GET_VERSION and enforces the protocol-major match.
	 *
	 * On a running target this address is ALSO bound to the kernel's
	 * alplab,gd32-bridge-gpio driver.  gd32g553_init() talks I2C
	 * through alp_i2c_write_read(), which uses the I2C_RDWR ioctl --
	 * unlike I2C_SLAVE, I2C_RDWR does not check against a bound
	 * kernel driver and never returns ALP_ERR_BUSY
	 * (src/yocto/peripheral_i2c.c).  So ownership has to be checked
	 * with a plain probe_addr() (I2C_SLAVE, via alp_i2c_read())
	 * before ever touching the chip -- mirroring probe_rtc(). */
	probe_result_t r = probe_addr(bus, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
	if (r == PROBE_BUSY) {
		report("gd32g553 bridge",
		       GD32G553_BRIDGE_DEFAULT_I2C_ADDR,
		       R_SKIP,
		       "kernel-owned (alplab,gd32-bridge-gpio bound) -- not probed");
		return;
	}

	gd32g553_t mcu;

	alp_status_t s = gd32g553_init(&mcu, NULL, bus, GD32G553_BRIDGE_DEFAULT_I2C_ADDR);
	if (s != ALP_OK) {
		report("gd32g553 bridge",
		       GD32G553_BRIDGE_DEFAULT_I2C_ADDR,
		       R_FAIL,
		       "PING/GET_VERSION failed -- firmware running? major match?");
		return;
	}
	report("gd32g553 bridge",
	       GD32G553_BRIDGE_DEFAULT_I2C_ADDR,
	       R_PASS,
	       "fw v%u.%u.%u over I2C",
	       mcu.version.major,
	       mcu.version.minor,
	       mcu.version.patch);
	gd32g553_deinit(&mcu);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	printf("\n=== V2N BRD_I2C bring-up diagnostic ===\n");

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = V2N_BRD_I2C_BUS_ID, /* BRD_I2C = Linux /dev/i2c-8 */
	    .bitrate_hz = 400000u,            /* every IC on this bus is FM-capable */
	});
	if (bus == NULL) {
		printf("FATAL: alp_i2c_open(bus %u) failed -- check /dev/i2c-8 exists "
		       "and this user has permission to open it.\n",
		       V2N_BRD_I2C_BUS_ID);
		return 1;
	}

	int hits = scan_bus(bus);

	printf("\nPhase 1: per-IC probes (read-only)\n");
	probe_rtc(bus);
	probe_tmp112(bus);
	probe_clkgen(bus);
	probe_act8760(bus);
	probe_da9292(bus);
	probe_tps628640(bus);
	probe_optiga(bus);
	probe_gd32(bus);

	printf("\n==== BRD_I2C report ====\n");
	printf("%-16s %-5s %-5s %s\n", "device", "addr", "res", "detail");
	int fails = 0;

	for (int i = 0; i < row_count; i++) {
		const char *res = rows[i].result == R_PASS   ? "PASS"
		                  : rows[i].result == R_SKIP ? "SKIP"
		                                             : "FAIL";

		if (rows[i].result == R_FAIL) {
			fails++;
		}
		printf("%-16s 0x%02X  %-5s %s\n", rows[i].name, rows[i].addr, res, rows[i].detail);
	}
	printf("========================\n");
	if (hits == 0) {
		printf("VERDICT: bus-level fault -- fix the electrical problem first.\n");
	} else if (fails == 0) {
		printf("VERDICT: BRD_I2C fully alive.\n");
	} else {
		printf("VERDICT: bus alive, %d device(s) failing -- see rows above.\n", fails);
	}

	alp_i2c_close(bus);
	return fails > 0 ? 1 : 0;
}
