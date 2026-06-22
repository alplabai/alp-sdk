/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp i2c` -- scan a bus, read / write a register, by portable bus_id.
 * Probe idiom: a 1-byte read on each 7-bit address (the portable probe
 * -- some controllers put nothing on the bus for a zero-length write).
 */
#include <errno.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#include <alp/peripheral.h>

#include "alp_console.h"

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_CMD_I2C)

static alp_i2c_t *open_bus(const struct shell *sh, const char *arg)
{
	unsigned long bus;

	if (alp_console_parse_ulong(arg, &bus) != 0) {
		shell_error(sh, "bad bus id");
		return NULL;
	}

	alp_i2c_t *h = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = (uint32_t)bus,
	    .bitrate_hz = 100000,
	});

	if (h == NULL) {
		shell_error(sh, "open bus %lu failed (err %d)", bus, (int)alp_last_error());
	}
	return h;
}

static int cmd_i2c_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	int found = 0;

	for (uint8_t addr = 0x08; addr < 0x78; addr++) {
		uint8_t scratch;

		if (alp_i2c_read(bus, addr, &scratch, 1) == ALP_OK) {
			shell_print(sh, "  0x%02x", addr);
			found++;
		}
	}
	alp_i2c_close(bus);
	shell_print(sh, "scan complete, %d responder(s)", found);
	return 0;
}

static int cmd_i2c_read(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long addr;
	unsigned long reg;
	unsigned long len       = 1;
	unsigned long reg_bytes = 1;

	if (alp_console_parse_ulong(argv[2], &addr) != 0 ||
	    alp_console_parse_ulong(argv[3], &reg) != 0 ||
	    (argc >= 5 && alp_console_parse_ulong(argv[4], &len) != 0) ||
	    (argc == 6 && alp_console_parse_ulong(argv[5], &reg_bytes) != 0) || len == 0 || len > 16 ||
	    reg_bytes == 0 || reg_bytes > 2) {
		shell_error(sh, "usage: alp i2c read <bus> <addr> <reg> [len<=16] [regbytes 1|2]");
		return -EINVAL;
	}

	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	/* Register address, big-endian (MSB first) -- the convention 16-bit
	 * EEPROMs (e.g. 24C128) and most I2C devices use.  reg_bytes defaults
	 * to 1 (8-bit-register sensors); pass 2 for 16-bit-addressed parts. */
	uint8_t r[2];
	size_t  rlen = (size_t)reg_bytes;

	if (reg_bytes == 2) {
		r[0] = (uint8_t)(reg >> 8);
		r[1] = (uint8_t)reg;
	} else {
		r[0] = (uint8_t)reg;
	}

	uint8_t      buf[16];
	alp_status_t s = alp_i2c_write_read(bus, (uint8_t)addr, r, rlen, buf, (size_t)len);

	alp_i2c_close(bus);
	if (s != ALP_OK) {
		shell_error(sh, "read failed (%d)", (int)s);
		return -EIO;
	}
	for (unsigned long i = 0; i < len; i++) {
		shell_fprintf(sh, SHELL_NORMAL, "%02x ", buf[i]);
	}
	shell_print(sh, "");
	return 0;
}

#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
static int cmd_i2c_write(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	unsigned long addr;
	unsigned long reg;
	unsigned long val;

	if (alp_console_parse_ulong(argv[2], &addr) != 0 ||
	    alp_console_parse_ulong(argv[3], &reg) != 0 ||
	    alp_console_parse_ulong(argv[4], &val) != 0) {
		shell_error(sh, "usage: alp i2c write <bus> <addr> <reg> <u8>");
		return -EINVAL;
	}

	alp_i2c_t *bus = open_bus(sh, argv[1]);

	if (bus == NULL) {
		return -EIO;
	}

	uint8_t      payload[2] = { (uint8_t)reg, (uint8_t)val };
	alp_status_t s          = alp_i2c_write(bus, (uint8_t)addr, payload, sizeof(payload));

	alp_i2c_close(bus);
	if (s != ALP_OK) {
		shell_error(sh, "write failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "0x%02lx[0x%02lx] <- 0x%02lx", addr, reg, val);
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_i2c_subcmds,
    SHELL_CMD_ARG(scan, NULL, "scan <bus>", cmd_i2c_scan, 2, 0),
    SHELL_CMD_ARG(read, NULL, "read <bus> <addr> <reg> [len] [regbytes 1|2]", cmd_i2c_read, 4, 2),
#if IS_ENABLED(CONFIG_ALP_SDK_CONSOLE_UNSAFE)
    SHELL_CMD_ARG(write, NULL, "write <bus> <addr> <reg> <u8> (UNSAFE)", cmd_i2c_write, 5, 0),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp), i2c, &alp_i2c_subcmds, "I2C scan / register read-write", NULL, 1, 0);

#endif /* CONFIG_ALP_SDK_CONSOLE_CMD_I2C */
