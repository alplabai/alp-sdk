/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `alp companion spi1` -- CC3501E SPI1 host passthrough (configure / xfer /
 * read / release), Alif companion only.  Command-group TU of the
 * alp_console_companion.c split (#673 Phase 2): registers onto the
 * (alp, companion) dynamic subcommand set the core TU declares.  Shared
 * companion context comes from alp_console_companion_internal.h.
 *
 * WHY THIS GROUP EXISTS.  The E1M connector's SPI1 lands on the CC3501E, not
 * on the Alif (E1M-AEN-2626-R2 netlist: AG10 SPI1_SCLK -> CC35 GPIO_32,
 * AG9 SPI1_MOSI -> GPIO_33, AG8 SPI1_MISO -> GPIO_34, AH9 SPI1_CS0 ->
 * GPIO_31, AH8 SPI1_CS1 -> GPIO_15).  A carrier SPI device is therefore
 * unreachable from the host except by relay: the CC3501E is the CONTROLLER
 * and the host supplies the bytes over the inter-chip bridge.  Nothing else
 * drives that relay on this board rev, so these four verbs are the only way
 * the passthrough can be validated on silicon.  That is why every verb prints
 * the ACTUAL bytes and the ACTUAL clock rather than a status code: a bench
 * operator has to be able to see that MISO carried a real JEDEC ID, and that
 * the divider did not hand back the rate they asked for.
 *
 * NOT the inter-chip bridge.  That link is CC35 SPI0 (GPIO_27/28/29 +
 * GPIO16), configured as a slave; nothing in this file can reach it.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <alp/chips/cc3501e/core.h>
#include <alp/ext/cc3501e/console.h>
#include <alp/peripheral.h>

#include "alp_console.h"
#include "alp_console_companion_internal.h"

#if !IS_ENABLED(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* ---- CC3501E SPI1 host passthrough (Alif companion) --------------------- */

/*
 * All three opcodes are worker-routed in the firmware (a polled 4 KB master
 * transfer must not run in the SPI0 slave ISR), and the worker holds exactly
 * ONE job -- so a Wi-Fi scan in flight makes every SPI1 verb poll-repeat as
 * BUSY until it drains.  10 s outlasts a short radio op without handing the
 * shell a 30 s hang when the bridge is actually wedged.
 */
#define ALP_COMPANION_SPI1_MS 10000u

/*
 * Console scratch, one buffer per direction.  Sized for what a human can type
 * and read back over a UART shell, NOT for the wire ceiling: the protocol
 * allows ALP_CC3501E_SPI1_MAX_XFER (4088) per chunk, but Zephyr's
 * CONFIG_SHELL_CMD_BUFF_SIZE (256 by default) already caps a typed hex
 * argument near 125 bytes, and dumping 4088 bytes as hex is 8176 characters of
 * console output.  Bulk throughput belongs in an application that chunks at
 * max_xfer; this group exists to prove the bus moves the right bytes.
 *
 * Static rather than automatic on purpose.  The shell thread is 4096 bytes
 * (raised from Zephyr's 2048 default in zephyr/kconfigs/core.kconfig for
 * exactly this shell -> cc3501e_request -> SPI chain), so 256 bytes would fit
 * today -- but raising this cap towards max_xfer must stay a one-line edit and
 * must never silently become a stack-overflow decision.  Same reasoning as the
 * sock group's receive buffer.
 */
#define ALP_COMPANION_SPI1_BUF 256u

static uint8_t companion_spi1_tx[ALP_COMPANION_SPI1_BUF];
static uint8_t companion_spi1_rx[ALP_COMPANION_SPI1_BUF];

/*
 * Dump received bytes as offset-prefixed CONTIGUOUS hex, 16 per row.
 * Contiguous (not space-separated) so a row pastes straight back into
 * `spi1 xfer` -- echoing a device's own response at it is the fastest way to
 * tell a live peripheral from a floating MISO reading back as its own MOSI.
 */
static void companion_spi1_dump(const struct shell *sh, const uint8_t *buf, size_t len)
{
	for (size_t off = 0; off < len; off += 16u) {
		size_t n = MIN(len - off, (size_t)16u);
		char   row[33];

		for (size_t i = 0; i < n; i++) {
			(void)snprintf(&row[i * 2u], 3u, "%02x", buf[off + i]);
		}
		shell_print(sh, "  %04x  %s", (unsigned int)off, row);
	}
}

/* Common tail of `xfer` and `read`: report what actually moved, then the
 * bytes.  cs_held is what we asked the firmware to do with CS -- print it so a
 * half-finished hold chain is visible on the console rather than remembered. */
static void companion_spi1_report(const struct shell *sh,
                                  size_t              tx_len,
                                  const uint8_t      *rx,
                                  uint16_t            rx_len,
                                  bool                cs_held)
{
	shell_print(sh,
	            "tx %u B, rx %u B, cs %s",
	            (unsigned int)tx_len,
	            (unsigned int)rx_len,
	            cs_held ? "HELD" : "released");
	if (rx_len != 0u) {
		companion_spi1_dump(sh, rx, rx_len);
	}
}

static int cmd_companion_spi1_configure(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	unsigned long freq;
	unsigned long mode;
	unsigned long cs;

	/* strtoul is 64-bit wide on native_sim while the wire field is uint32, so
	 * round-trip the value instead of comparing against a constant that folds
	 * away on a 32-bit target. */
	if (alp_console_parse_ulong(argv[1], &freq) != 0 || freq == 0u ||
	    (unsigned long)(uint32_t)freq != freq || alp_console_parse_ulong(argv[2], &mode) != 0 ||
	    mode > 3u || alp_console_parse_ulong(argv[3], &cs) != 0 || cs > 1u) {
		shell_error(sh, "usage: alp companion spi1 configure <freq_hz> <mode 0..3> <cs 0|1>");
		return -EINVAL;
	}

	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	/* bits_per_word is not on the command line, and the driver does not read it
	 * back either: 8 is the only value proto v6 accepts and cc3501e_spi1_configure()
	 * pins it internally (CC3501E_SPI1_BITS_PER_WORD in cc3501e_spi.c), so there is
	 * nothing here to request or echo -- just report the fixed value. */
	uint32_t     actual_freq_hz = 0;
	uint16_t     max_xfer       = 0;
	alp_status_t s              = cc3501e_spi1_configure(companion_cc3501e,
	                                                     (uint32_t)freq,
	                                                     (uint8_t)mode,
	                                                     (alp_cc3501e_spi1_cs_t)cs,
	                                                     &actual_freq_hz,
	                                                     &max_xfer,
	                                                     ALP_COMPANION_SPI1_MS);

	if (s != ALP_OK) {
		shell_error(sh, "spi1 configure failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "cs%lu mode %lu bpw 8 (fixed)", cs, mode);
	/* Requested vs actual, always both: a real divider rounds, and a host that
	 * assumes it got its asked-for rate mis-times every peripheral on the bus.
	 * 0 is the TI backend's honest "not measured yet" answer (no divider
	 * read-back exists on this HAL yet) -- print that as "unknown" rather
	 * than a bare 0 Hz, which would read as a real (if implausible) rate. */
	if (actual_freq_hz == 0u) {
		shell_print(sh, "sck unknown actual (%lu Hz requested)", freq);
	} else {
		shell_print(sh, "sck %u Hz actual (%lu Hz requested)", (unsigned int)actual_freq_hz, freq);
	}
	shell_print(sh,
	            "max_xfer %u B per chunk (this console caps one command at %u B)",
	            (unsigned int)max_xfer,
	            (unsigned int)ALP_COMPANION_SPI1_BUF);
	return 0;
}

static int cmd_companion_spi1_xfer(const struct shell *sh, size_t argc, char **argv)
{
	size_t tx_len = 0;

	if (alp_console_parse_hex(argv[1], companion_spi1_tx, sizeof(companion_spi1_tx), &tx_len) !=
	    0) {
		shell_error(sh,
		            "usage: alp companion spi1 xfer <hexbytes> [hold] [norx]  (max %u B)",
		            (unsigned int)sizeof(companion_spi1_tx));
		return -EINVAL;
	}

	bool hold = false;
	bool norx = false;

	for (size_t i = 2; i < argc; i++) {
		if (strcmp(argv[i], "hold") == 0) {
			hold = true;
		} else if (strcmp(argv[i], "norx") == 0) {
			norx = true;
		} else {
			shell_error(sh, "unknown option '%s' (want hold / norx)", argv[i]);
			return -EINVAL;
		}
	}

	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	/* A NULL rx buffer is the NO_RX flag: it deletes a whole direction from a
	 * link that is round-trip bound wherever READY is not readable (a board
	 * without the READY pad's input-enable pinctrl group -- see chips/cc3501e/
	 * cc3501e_sockets.c).  tx_fill is ignored while tx is non-NULL.  There is
	 * no rx_len_out on the wrapper -- the driver guarantees exactly tx_len
	 * bytes in rx on ALP_OK (or ALP_ERR_IO on a short/mismatched reply), so
	 * that is the count to report. */
	const uint16_t rx_len = norx ? 0u : (uint16_t)tx_len;
	alp_status_t   s      = cc3501e_spi1_transfer(companion_cc3501e,
	                                              companion_spi1_tx,
	                                              norx ? NULL : companion_spi1_rx,
	                                              (uint16_t)tx_len,
	                                              0xFFu,
	                                              hold,
	                                              ALP_COMPANION_SPI1_MS);

	if (s != ALP_OK) {
		shell_error(sh, "spi1 xfer failed (%d)", (int)s);
		return -EIO;
	}
	companion_spi1_report(sh, tx_len, companion_spi1_rx, rx_len, hold);
	return 0;
}

/*
 * `read <len> [hold] [<fill>]` -- the NO_TX half.  A flash read or a sensor
 * FIFO drain clocks hundreds of don't-care bytes out; typing them as hex would
 * blow past the shell's command buffer long before the wire limit, and sending
 * them would waste the bridge's scarcest resource.  The two optional tokens are
 * order-free: "hold" is the literal, anything else is the fill byte.
 */
static int cmd_companion_spi1_read(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long len;

	if (alp_console_parse_ulong(argv[1], &len) != 0 || len > ALP_COMPANION_SPI1_BUF) {
		shell_error(sh,
		            "usage: alp companion spi1 read <0..%u> [hold] [<fill hex byte>]",
		            (unsigned int)ALP_COMPANION_SPI1_BUF);
		return -EINVAL;
	}

	bool hold = false;
	/* 0xFF, not 0x00: it is what an idle SPI line and every NOR-flash dummy
	 * cycle expect, so a stuck-low MOSI shows up as 0x00 in a loopback. */
	uint8_t fill = 0xFFu;

	for (size_t i = 2; i < argc; i++) {
		size_t n = 0;

		if (strcmp(argv[i], "hold") == 0) {
			hold = true;
		} else if (alp_console_parse_hex(argv[i], &fill, 1u, &n) != 0) {
			shell_error(sh, "unknown option '%s' (want hold / a 2-digit fill byte)", argv[i]);
			return -EINVAL;
		}
	}

	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	/* len == 0 without `hold` is the standalone CS deassert -- the one case a
	 * dedicated CS opcode would have covered.  Send it with tx non-NULL so the
	 * flags byte is exactly 0, which is what the contract defines as a pure
	 * deassert; NO_TX at len 0 would clock nothing either, but this keeps the
	 * wire literally matching the documented case. */
	/* No rx_len_out on the wrapper -- rx is always the destination here (never
	 * NULL), so the driver guarantees exactly len bytes filled on ALP_OK. */
	const uint8_t *tx     = (len == 0u) ? companion_spi1_tx : NULL;
	const uint16_t rx_len = (uint16_t)len;
	alp_status_t   s      = cc3501e_spi1_transfer(
	    companion_cc3501e, tx, companion_spi1_rx, (uint16_t)len, fill, hold, ALP_COMPANION_SPI1_MS);

	if (s != ALP_OK) {
		shell_error(sh, "spi1 read failed (%d)", (int)s);
		return -EIO;
	}
	companion_spi1_report(sh, 0u, companion_spi1_rx, rx_len, hold);
	return 0;
}

/*
 * The escape hatch.  Deasserts CS, closes the instance and drops the firmware's
 * cached (seq, result) -- and it is defined never to fail on state, so an
 * operator who lost track of a `hold` chain always has a clean way back.
 * Issuing it with nothing open is a success, not an error.
 */
static int cmd_companion_spi1_release(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (companion_cc3501e == NULL) {
		shell_warn(sh, "companion not registered");
		return -ENODEV;
	}

	alp_status_t s = cc3501e_spi1_release(companion_cc3501e, ALP_COMPANION_SPI1_MS);

	if (s != ALP_OK) {
		shell_error(sh, "spi1 release failed (%d)", (int)s);
		return -EIO;
	}
	shell_print(sh, "spi1 released");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    alp_companion_spi1_subcmds,
    SHELL_CMD_ARG(configure,
                  NULL,
                  "configure <freq_hz> <mode 0..3> <cs 0|1>  -- acquire SPI1, print actual SCK",
                  cmd_companion_spi1_configure,
                  4,
                  0),
    SHELL_CMD_ARG(xfer,
                  NULL,
                  "xfer <hexbytes> [hold] [norx]  -- full-duplex chunk, prints the RX bytes",
                  cmd_companion_spi1_xfer,
                  2,
                  2),
    SHELL_CMD_ARG(read,
                  NULL,
                  "read <len> [hold] [<fill>]  -- clock fill bytes out, print what came back",
                  cmd_companion_spi1_read,
                  2,
                  2),
    SHELL_CMD_ARG(release,
                  NULL,
                  "release  -- drop CS, close SPI1, free the bus (never fails)",
                  cmd_companion_spi1_release,
                  1,
                  0),
    SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((alp, companion),
                 spi1,
                 &alp_companion_spi1_subcmds,
                 "CC3501E SPI1 host passthrough (configure / xfer / read / release)",
                 NULL,
                 1,
                 0);
#endif /* !CONFIG_ALP_SDK_V2N_SUPERVISOR */
