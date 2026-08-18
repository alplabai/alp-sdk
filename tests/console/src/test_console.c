/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <string.h>

#include <alp/version.h> /* ALP_VERSION_STRING -- the single SDK-version source */

/* Run a shell line on the dummy backend and return its captured output. */
static const char *run(const char *line)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	shell_backend_dummy_clear_output(sh);
	(void)shell_execute_cmd(sh, line);

	size_t      len;
	const char *out = shell_backend_dummy_get_output(sh, &len);
	return out;
}

static void *suite_setup(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	WAIT_FOR(shell_ready(sh), 20000, k_msleep(1));
	zassert_true(shell_ready(sh), "timed out waiting for dummy shell backend");
	return NULL;
}

ZTEST(alp_console, test_board_reports_version)
{
	const char *out = run("alp board");

	zassert_not_null(strstr(out, "Alp SDK"), "banner line missing: %s", out);
	zassert_not_null(strstr(out, ALP_VERSION_STRING), "version missing");
}

ZTEST(alp_console, test_mem_rd_reads_known_word)
{
	static volatile uint32_t probe = 0xCAFEF00Du;
	char                     line[48];

	snprintk(line, sizeof(line), "alp mem rd 0x%lx", (unsigned long)(uintptr_t)&probe);
	const char *out = run(line);

	zassert_not_null(strstr(out, "cafef00d"), "expected value in: %s", out);
}

ZTEST(alp_console, test_mem_wr_then_rd_roundtrips)
{
	static volatile uint32_t probe = 0;
	char                     line[64];

	snprintk(line, sizeof(line), "alp mem wr 0x%lx 0x12345678", (unsigned long)(uintptr_t)&probe);
	(void)run(line);
	zassert_equal(probe, 0x12345678u, "write did not land");
}

ZTEST(alp_console, test_gpio_read_runs)
{
	const char *out = run("alp gpio read 0");

	/* On native_sim pin 0 of the emulated gpio_emul0 reads back a
	 * defined level (the gpio-emul reset default is low = 0).
	 * Assert the command resolved and printed a level, not an error. */
	zassert_true(strstr(out, "= 0") || strstr(out, "= 1"), "got: %s", out);
}

ZTEST(alp_console, test_i2c_scan_runs)
{
	const char *out = run("alp i2c scan 0");

	zassert_not_null(strstr(out, "responder"), "scan summary missing: %s", out);
}

ZTEST(alp_console, test_i2c_read_2byte_reg)
{
	/* The sw_fallback echoes the written register-address bytes back on the
	 * read phase.  With regbytes=2 and reg=0x1234 the 2-byte BIG-ENDIAN
	 * address 0x12 0x34 must lead the readback -- proving the command issued
	 * a 2-byte register address (needed for 16-bit-addressed parts like the
	 * 24C128 EEPROM). */
	const char *out = run("alp i2c read 0 0x50 0x1234 4 2");

	zassert_not_null(strstr(out, "12 34"), "2-byte reg addr not issued: %s", out);
}

ZTEST(alp_console, test_i2c_read_1byte_default)
{
	/* No regbytes arg = 1-byte register (backward compatible); the single
	 * address byte 0xab echoes back. */
	const char *out = run("alp i2c read 0 0x50 0xab 2");

	zassert_not_null(strstr(out, "ab"), "1-byte reg addr not issued: %s", out);
}

ZTEST(alp_console, test_adc_read_registers)
{
	const char *out = run("alp adc read 0");

	/* Command must be registered AND its handler must run: on native_sim the
	 * sw-fallback adc open fails, so the handler prints "open ch ...". A raw
	 * value ("raw") is the success token on real hardware. "Unknown command"
	 * means the subcmd is absent. */
	zassert_is_null(strstr(out, "Unknown command"), "adc cmd not registered: %s", out);
	zassert_true(strstr(out, "open ch") != NULL || strstr(out, "raw") != NULL,
	             "adc handler did not run: %s",
	             out);
}

ZTEST(alp_console, test_clk_dump_runs)
{
	const char *out = run("alp clk");

	zassert_not_null(strstr(out, "Hz"), "clk dump missing: %s", out);
	zassert_is_null(strstr(out, "Unknown command"), "clk cmd not registered: %s", out);
}

/*
 * `alp companion` command-group registration (#673 Phase 2 split):
 * alp_console_companion.c (core) plus the _wifi/_ble/_diag/_ota/_sock
 * sibling TUs each register their group onto the (alp, companion)
 * dynamic subcommand set from their own file -- these assert every group
 * survived the split and is reachable by name.  No companion is bound
 * (alp_console_companion_set() is never called), so each command exits
 * early via its "companion not registered" guard; the point here is that
 * the shell resolves the command at all, not the CC3501E transaction
 * behind it (that's chips/cc3501e/cc3501e.c's own coverage, see
 * tests/zephyr/cc3501e_host_driver). */
ZTEST(alp_console, test_companion_ver_registered)
{
	const char *out = run("alp companion ver");

	zassert_is_null(strstr(out, "Unknown command"), "companion ver not registered: %s", out);
}

ZTEST(alp_console, test_companion_ping_registered)
{
	const char *out = run("alp companion ping");

	zassert_is_null(strstr(out, "Unknown command"), "companion ping not registered: %s", out);
}

ZTEST(alp_console, test_companion_reset_registered)
{
	const char *out = run("alp companion reset");

	zassert_is_null(strstr(out, "Unknown command"), "companion reset not registered: %s", out);
}

ZTEST(alp_console, test_companion_bench_registered)
{
	const char *out = run("alp companion bench");

	zassert_is_null(strstr(out, "Unknown command"), "companion bench not registered: %s", out);
}

ZTEST(alp_console, test_companion_wifi_group_registered)
{
	const char *out = run("alp companion wifi scan");

	zassert_is_null(strstr(out, "Unknown command"), "companion wifi not registered: %s", out);
}

ZTEST(alp_console, test_companion_ble_group_registered)
{
	const char *out = run("alp companion ble enable");

	zassert_is_null(strstr(out, "Unknown command"), "companion ble not registered: %s", out);
}

/* Sibling of the wifi connect/ap guards above (#1376/#1480): `ble connect`'s
 * 3rd token is the only optional flag and must be "random"; a mistyped token
 * used to be compared `== 0` and otherwise silently ignored, connecting as
 * address type public with no diagnostic. */
ZTEST(alp_console, test_ble_connect_rejects_unrecognised_third_token)
{
	const char *out = run("alp companion ble connect aa:bb:cc:dd:ee:ff randm");

	zassert_not_null(strstr(out, "unrecognised argument"),
	                 "an unrecognised 3rd token must fail loudly, not be dropped: %s",
	                 out);
	/* It must be rejected DURING parsing -- before any companion/state check. */
	zassert_is_null(strstr(out, "companion not registered"),
	                "a usage error must not be reported as a missing companion: %s",
	                out);
}

ZTEST(alp_console, test_ble_connect_still_accepts_random_token)
{
	const char *out = run("alp companion ble connect aa:bb:cc:dd:ee:ff random");

	zassert_is_null(
	    strstr(out, "unrecognised argument"), "\"random\" is the one legal 3rd token: %s", out);
	zassert_not_null(strstr(out, "companion not registered"),
	                 "a well-formed random connect should reach the companion check: %s",
	                 out);
}

ZTEST(alp_console, test_companion_ble_gatt_subgroup_registered)
{
	const char *out = run("alp companion ble gatt read 0");

	zassert_is_null(strstr(out, "Unknown command"), "companion ble gatt not registered: %s", out);
}

ZTEST(alp_console, test_companion_diag_group_registered)
{
	const char *out = run("alp companion diag info");

	zassert_is_null(strstr(out, "Unknown command"), "companion diag not registered: %s", out);
}

ZTEST(alp_console, test_companion_ota_group_registered)
{
	const char *out = run("alp companion ota status");

	zassert_is_null(strstr(out, "Unknown command"), "companion ota not registered: %s", out);
}

ZTEST(alp_console, test_companion_sock_group_registered)
{
	const char *out = run("alp companion sock tcp-get 127.0.0.1 80 /");

	zassert_is_null(strstr(out, "Unknown command"), "companion sock not registered: %s", out);
}

/* #1376/#1480: an unrecognised 4th token used to be silently DROPPED, for both
 * `wifi connect` and `wifi ap`.  The dangerous shape is an UNQUOTED SSID
 * containing a space: it splits across argv[1]/argv[2], the real passphrase
 * lands in argv[3], and the old `strcmp(argv[3], "wpa3") == 0` test ignored it
 * -- so the console printed a confident "connecting"/"ap starting" line for an
 * SSID the user never typed, with their passphrase eaten as a security token.
 * Associating with a truncated SSID is worse than refusing.
 *
 * These six tests (three `wifi connect` + three `wifi ap`, below) are one set
 * and should stay one: each family's first test proves the dangerous form is
 * now refused, and its other test(s) prove the refusal did not simply ban
 * spaces or break the one legal flag.  No companion is registered in this
 * suite, so a command that gets PAST argument validation reports "companion
 * not registered" -- that reply is the marker for "parsed fine", and its
 * absence in the rejection case is the marker for "rejected during
 * parsing". */
ZTEST(alp_console, test_wifi_connect_rejects_unrecognised_fourth_token)
{
	const char *out = run("alp companion wifi connect my ssid secret");

	zassert_not_null(strstr(out, "unrecognised argument"),
	                 "an unrecognised 4th token must fail loudly, not be dropped: %s",
	                 out);
	zassert_not_null(strstr(out, "must be quoted"), "the refusal must say how to fix it: %s", out);
	/* It must be rejected DURING parsing -- before any companion/state check. */
	zassert_is_null(strstr(out, "companion not registered"),
	                "a usage error must not be reported as a missing companion: %s",
	                out);
}

ZTEST(alp_console, test_wifi_connect_accepts_quoted_ssid_with_space)
{
	const char *out = run("alp companion wifi connect \"my ssid\" secret");

	zassert_is_null(strstr(out, "unrecognised argument"),
	                "a QUOTED ssid containing a space must still parse: %s",
	                out);
	/* Parsed cleanly, so it reached the companion check. */
	zassert_not_null(strstr(out, "companion not registered"),
	                 "a well-formed connect should reach the companion check: %s",
	                 out);
}

ZTEST(alp_console, test_wifi_connect_still_accepts_wpa3_token)
{
	const char *out = run("alp companion wifi connect \"my ssid\" secret wpa3");

	zassert_is_null(
	    strstr(out, "unrecognised argument"), "\"wpa3\" is the one legal 4th token: %s", out);
	zassert_not_null(strstr(out, "companion not registered"),
	                 "a well-formed wpa3 connect should reach the companion check: %s",
	                 out);
}

ZTEST(alp_console, test_wifi_ap_rejects_unrecognised_fourth_token)
{
	const char *out = run("alp companion wifi ap my ssid secret");

	zassert_not_null(strstr(out, "unrecognised argument"),
	                 "an unrecognised 4th token must fail loudly, not be dropped: %s",
	                 out);
	zassert_not_null(strstr(out, "must be quoted"), "the refusal must say how to fix it: %s", out);
	/* It must be rejected DURING parsing -- before any companion/state check. */
	zassert_is_null(strstr(out, "companion not registered"),
	                "a usage error must not be reported as a missing companion: %s",
	                out);
}

ZTEST(alp_console, test_wifi_ap_accepts_quoted_ssid_with_space)
{
	const char *out = run("alp companion wifi ap \"my ssid\" secret");

	zassert_is_null(strstr(out, "unrecognised argument"),
	                "a QUOTED ssid containing a space must still parse: %s",
	                out);
	/* Parsed cleanly, so it reached the companion check. */
	zassert_not_null(strstr(out, "companion not registered"),
	                 "a well-formed ap should reach the companion check: %s",
	                 out);
}

ZTEST(alp_console, test_wifi_ap_still_accepts_wpa3_token)
{
	const char *out = run("alp companion wifi ap \"my ssid\" secret wpa3");

	zassert_is_null(
	    strstr(out, "unrecognised argument"), "\"wpa3\" is the one legal 4th token: %s", out);
	zassert_not_null(strstr(out, "companion not registered"),
	                 "a well-formed wpa3 ap should reach the companion check: %s",
	                 out);
}

ZTEST_SUITE(alp_console, NULL, suite_setup, NULL, NULL, NULL);
