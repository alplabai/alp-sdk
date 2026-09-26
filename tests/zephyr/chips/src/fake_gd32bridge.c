/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake GD32G553 supervisor-MCU bridge i2c-emul target.  Models just
 * enough of the wire protocol (docs/gd32-bridge-protocol.md §5) for
 * gd32g553_init()'s PING + GET_VERSION liveness handshake: the write
 * side is [reg=0x00][CMD][CRC(CMD) lo,hi] (no request payload for
 * either opcode), and the read side is either a full success reply
 * ([STATUS=0][payload][CRC(STATUS..payload)]) or the SHORT
 * error-envelope shape real firmware answers with while busy
 * ([STATUS][CRC(STATUS) lo,hi] only) -- gd32g553.c's i2c_xfer()
 * decodes exactly that shape as its "error-envelope fallback", the
 * same trap documented on the SPI side.
 *
 * Test control lets a case arm N STATUS_BUSY replies (modelling the
 * post-OTA-COMMIT/ROLLBACK TRIAL window, docs/gd32-bridge-protocol.md
 * §10, where the bridge answers BUSY to every opcode until it decodes
 * a CRC-valid frame) and/or M raw bus failures (modelling the SECOND,
 * confirm-triggered reset's link drop) before it answers normally --
 * driving gd32g553_init()'s retry ladder in chips/gd32g553/gd32g553.c.
 */

#define DT_DRV_COMPAT alp_fake_gd32bridge

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>

#include "alp/protocol/crc16.h"
#include "fakes.h"

struct fake_gd32bridge_data {
	uint8_t  major, minor, patch;
	unsigned busy_replies_remaining; /* answers STATUS_BUSY to ANY opcode */
	unsigned io_fail_remaining;      /* raw bus failure -- models a link drop */
	uint32_t calls_seen;
};

static struct fake_gd32bridge_data *g_fake_gd32bridge;

static void seed_defaults(struct fake_gd32bridge_data *d)
{
	memset(d, 0, sizeof(*d));
	/* Matches GD32G553_HOST_PROTOCOL_MAJOR = 0 so gd32g553_init()'s
	 * major-mismatch gate doesn't fire; minor picked at the current
	 * OTA-trial firmware line for realism, not load-bearing here. */
	d->major = 0u;
	d->minor = 12u;
	d->patch = 0u;
}

/* STATUS_BUSY = 0x03 (docs/gd32-bridge-protocol.md §6). */
#define FAKE_GD32BRIDGE_STATUS_BUSY 0x03u
#define FAKE_GD32BRIDGE_STATUS_OK   0x00u
#define FAKE_GD32BRIDGE_CMD_PING    0x00u
#define FAKE_GD32BRIDGE_CMD_VERSION 0x01u

static void write_ok_reply(uint8_t *buf, size_t len, const uint8_t *payload, size_t payload_len)
{
	buf[0] = FAKE_GD32BRIDGE_STATUS_OK;
	if (payload_len > 0u) memcpy(&buf[1], payload, payload_len);
	const uint16_t crc         = alp_crc16_ccitt_false(buf, 1u + payload_len);
	buf[1u + payload_len]      = (uint8_t)(crc & 0xFFu);
	buf[1u + payload_len + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
	(void)len; /* caller sized buf to exactly what this writes */
}

/* Short error-envelope: STATUS + CRC(STATUS) only -- the shape real
 * firmware answers with for ANY opcode while STATUS_BUSY (no payload,
 * regardless of the opcode's normal reply width).  Bytes past the
 * 3-byte envelope are unread by the host's fallback decode but zeroed
 * here so nothing looks like stale FIFO content by accident. */
static void write_busy_reply(uint8_t *buf, size_t len)
{
	buf[0]             = FAKE_GD32BRIDGE_STATUS_BUSY;
	const uint16_t crc = alp_crc16_ccitt_false(buf, 1u);
	buf[1]             = (uint8_t)(crc & 0xFFu);
	buf[2]             = (uint8_t)((crc >> 8) & 0xFFu);
	if (len > 3u) memset(&buf[3], 0, len - 3u);
}

static int
fake_gd32bridge_transfer(const struct emul *target, struct i2c_msg *msgs, int num_msgs, int addr)
{
	(void)addr;
	struct fake_gd32bridge_data *d = target->data;

	/* Models the link physically dropping (the bridge's second,
	 * confirm-triggered reset) -- a raw bus failure, not a decoded
	 * STATUS_BUSY reply. */
	if (d->io_fail_remaining > 0u) {
		d->io_fail_remaining--;
		return -EIO;
	}

	/* i2c_xfer() always issues a combined write-then-read. */
	if (num_msgs != 2 || (msgs[0].flags & I2C_MSG_READ) != 0 ||
	    (msgs[1].flags & I2C_MSG_READ) == 0) {
		return -EIO;
	}
	if (msgs[0].len < 2u) return -EIO;
	const uint8_t cmd = msgs[0].buf[1];
	d->calls_seen++;

	if (d->busy_replies_remaining > 0u) {
		d->busy_replies_remaining--;
		write_busy_reply(msgs[1].buf, msgs[1].len);
		return 0;
	}

	switch (cmd) {
	case FAKE_GD32BRIDGE_CMD_PING:
		write_ok_reply(msgs[1].buf, msgs[1].len, NULL, 0u);
		return 0;
	case FAKE_GD32BRIDGE_CMD_VERSION: {
		const uint8_t payload[3] = { d->major, d->minor, d->patch };
		write_ok_reply(msgs[1].buf, msgs[1].len, payload, sizeof(payload));
		return 0;
	}
	default:
		/* Unexercised by this fake's test cases -- answer BUSY rather
		 * than silently misbehaving on an opcode nobody armed. */
		write_busy_reply(msgs[1].buf, msgs[1].len);
		return 0;
	}
}

static const struct i2c_emul_api fake_gd32bridge_api = {
	.transfer = fake_gd32bridge_transfer,
};

static int fake_gd32bridge_init(const struct emul *target, const struct device *parent)
{
	(void)parent;
	struct fake_gd32bridge_data *d = target->data;
	g_fake_gd32bridge              = d;
	seed_defaults(d);
	return 0;
}

/* See fake_rv3028c7.c's comment above its FAKE_RV3028C7_DEFINE for why
 * this bare placeholder device is needed. */
#define FAKE_GD32BRIDGE_DEFINE(n) \
	static struct fake_gd32bridge_data fake_gd32bridge_data_##n; \
	DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL, 90, NULL); \
	EMUL_DT_INST_DEFINE( \
	    n, fake_gd32bridge_init, &fake_gd32bridge_data_##n, NULL, &fake_gd32bridge_api, NULL);

DT_INST_FOREACH_STATUS_OKAY(FAKE_GD32BRIDGE_DEFINE)

/* ------------------------------------------------------------------ */
/* Test-side inspection + fault-injection API                          */
/* ------------------------------------------------------------------ */

void fake_gd32bridge_set_version(uint8_t major, uint8_t minor, uint8_t patch)
{
	if (g_fake_gd32bridge == NULL) return;
	g_fake_gd32bridge->major = major;
	g_fake_gd32bridge->minor = minor;
	g_fake_gd32bridge->patch = patch;
}

void fake_gd32bridge_arm_busy_replies(unsigned count)
{
	if (g_fake_gd32bridge == NULL) return;
	g_fake_gd32bridge->busy_replies_remaining = count;
}

void fake_gd32bridge_arm_io_failures(unsigned count)
{
	if (g_fake_gd32bridge == NULL) return;
	g_fake_gd32bridge->io_fail_remaining = count;
}

uint32_t fake_gd32bridge_calls_seen(void)
{
	return g_fake_gd32bridge ? g_fake_gd32bridge->calls_seen : 0u;
}

void fake_gd32bridge_reset(void)
{
	if (g_fake_gd32bridge != NULL) seed_defaults(g_fake_gd32bridge);
}
