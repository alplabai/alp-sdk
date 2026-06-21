/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <string.h>

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
	zassert_not_null(strstr(out, CONFIG_ALP_SDK_VERSION), "version missing");
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

ZTEST_SUITE(alp_console, NULL, suite_setup, NULL, NULL, NULL);
