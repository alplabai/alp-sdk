/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * modbus-server -- ADR 0018 Modbus teaching example.
 *
 * `board.yaml` selects `libraries: [modbus]`.  The Alp SDK library layer turns
 * that into Zephyr's real upstream `CONFIG_MODBUS=y` symbol from
 * metadata/libraries/modbus.yaml.  This file then picks a transport: RAW_ADU.
 *
 * RAW_ADU is still the Modbus core.  It just hands complete application data
 * units to a callback instead of moving bytes over UART RTU/ASCII.  That makes
 * it perfect for native_sim and for a teaching example: one in-process client
 * writes holding register 0, reads it back, toggles coil 0, and reads the coil
 * bitmap, while one in-process server owns the register/coil table.
 *
 * On hardware, the application-level callbacks below stay the same.  Only the
 * transport changes: enable a UART-backed `zephyr,modbus-serial` devicetree
 * node and configure RTU or ASCII parameters instead of RAW_0/RAW_1.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <alp/peripheral.h>

#include <zephyr/kernel.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/util.h>

#define MODBUS_UNIT_ID    1u
#define MODBUS_TIMEOUT_MS 1000u
#define MODBUS_REG_COUNT  4u

/* Modbus exposes four logical tables: coils, discrete inputs, input
 * registers, and holding registers.  This example keeps the smallest useful
 * subset: one writable coil bitmap and a short holding-register array. */
static uint16_t holding_regs[MODBUS_REG_COUNT] = {
	0x1111,
	0x2222,
	0x3333,
	0x4444,
};
static uint8_t coils;

/* RAW_ADU gives us named in-memory interfaces.  Zephyr creates RAW_0..RAW_N
 * from CONFIG_MODBUS_NUMOF_RAW_ADU; prj.conf requests two so the client and
 * server can be distinct, just like two UART endpoints would be distinct. */
static int client_iface;
static int server_iface;

/* One scratch ADU is enough because each request is synchronous:
 * client callback submits the request to the server, waits for the response,
 * then submits that response back to the waiting client API call. */
static struct modbus_adu bridge_adu;
K_SEM_DEFINE(response_ready, 0, 1);

static void copy_adu(struct modbus_adu *dst, const struct modbus_adu *src)
{
	dst->trans_id = src->trans_id;
	dst->proto_id = src->proto_id;
	dst->length   = src->length;
	dst->unit_id  = src->unit_id;
	dst->fc       = src->fc;
	dst->crc      = src->crc;
	memcpy(dst->data, src->data, MIN(src->length, sizeof(dst->data)));
}

static int coil_rd(uint16_t addr, bool *state)
{
	if (addr >= 8u) {
		return -ENOTSUP;
	}

	*state = (coils & BIT(addr)) != 0u;
	return 0;
}

static int coil_wr(uint16_t addr, bool state)
{
	if (addr >= 8u) {
		return -ENOTSUP;
	}

	if (state) {
		coils |= BIT(addr);
	} else {
		coils &= (uint8_t)~BIT(addr);
	}

	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(holding_regs)) {
		return -ENOTSUP;
	}

	*reg = holding_regs[addr];
	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_regs)) {
		return -ENOTSUP;
	}

	holding_regs[addr] = reg;
	return 0;
}

static struct modbus_user_callbacks server_callbacks = {
	.coil_rd        = coil_rd,
	.coil_wr        = coil_wr,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static int server_raw_cb(const int iface, const struct modbus_adu *adu, void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	/* The server core has already applied the function code to the register
	 * table by the time this callback runs.  The callback's job is only to
	 * hand the response ADU back to whatever transport we are teaching. */
	copy_adu(&bridge_adu, adu);
	k_sem_give(&response_ready);
	return 0;
}

static int client_raw_cb(const int iface, const struct modbus_adu *adu, void *user_data)
{
	int err;

	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	/* This callback stands in for the wire.  A UART-backed RTU client would
	 * transmit bytes here; RAW_ADU submits the complete request directly to
	 * the server interface and lets Zephyr's Modbus worker parse it. */
	copy_adu(&bridge_adu, adu);
	err = modbus_raw_submit_rx(server_iface, &bridge_adu);
	if (err != 0) {
		return err;
	}

	if (k_sem_take(&response_ready, K_MSEC(MODBUS_TIMEOUT_MS)) != 0) {
		return -ETIMEDOUT;
	}

	/* Feed the server's response into the client interface.  The public
	 * modbus_* client call that triggered this callback unblocks after this. */
	return modbus_raw_submit_rx(client_iface, &bridge_adu);
}

static int init_modbus(void)
{
	struct modbus_iface_param server_param = {
		.mode = MODBUS_MODE_RAW,
		.server = {
			.user_cb = &server_callbacks,
			.unit_id = MODBUS_UNIT_ID,
		},
		.rawcb = {
			.raw_tx_cb = server_raw_cb,
			.user_data = NULL,
		},
	};
	struct modbus_iface_param client_param = {
		.mode = MODBUS_MODE_RAW,
		.rx_timeout = MODBUS_TIMEOUT_MS,
		.rawcb = {
			.raw_tx_cb = client_raw_cb,
			.user_data = NULL,
		},
	};
	int err;

	client_iface = modbus_iface_get_by_name("RAW_0");
	server_iface = modbus_iface_get_by_name("RAW_1");
	if (client_iface < 0 || server_iface < 0) {
		printf("[modbus] RAW_ADU interfaces missing; check CONFIG_MODBUS_NUMOF_RAW_ADU\n");
		return -ENODEV;
	}

	err = modbus_init_server(server_iface, server_param);
	if (err != 0) {
		printf("[modbus] server init failed: %d\n", err);
		return err;
	}

	err = modbus_init_client(client_iface, client_param);
	if (err != 0) {
		printf("[modbus] client init failed: %d\n", err);
		return err;
	}

	return 0;
}

int main(void)
{
	uint16_t reg         = 0u;
	uint8_t  coil_bitmap = 0u;
	int      err;

	/* Keep the normal Alp SDK bring-up call even though the Modbus API itself
	 * is Zephyr's third-party subsystem.  Real applications usually mix
	 * portable <alp/...> peripherals with curated third-party protocol stacks. */
	(void)alp_init();

	printf("[modbus] RAW_ADU server example starting\n");

	err = init_modbus();
	if (err != 0) {
		return 0;
	}

	/* Function code 06 writes one holding register.  Holding registers are
	 * 16-bit read/write words: the common place for setpoints, counters, and
	 * configuration values in industrial devices. */
	err = modbus_write_holding_reg(client_iface, MODBUS_UNIT_ID, 0, 0x4d42);
	if (err != 0) {
		printf("[modbus] write holding register failed: %d\n", err);
		return 0;
	}

	/* Function code 03 reads holding registers back.  The client API returns
	 * host-endian uint16_t values; Zephyr's Modbus core handles the wire PDU
	 * encoding and exception responses. */
	err = modbus_read_holding_regs(client_iface, MODBUS_UNIT_ID, 0, &reg, 1);
	if (err != 0) {
		printf("[modbus] read holding register failed: %d\n", err);
		return 0;
	}
	printf("[modbus] holding[0]=0x%04x\n", (unsigned int)reg);

	/* Function code 05 writes one coil.  Coils are single-bit read/write
	 * outputs: relays, enables, or logical flags. */
	err = modbus_write_coil(client_iface, MODBUS_UNIT_ID, 0, true);
	if (err != 0) {
		printf("[modbus] write coil failed: %d\n", err);
		return 0;
	}

	/* Function code 01 packs coil states into bytes.  coil 0 appears in bit 0
	 * of the first response byte, so a successful round trip prints 0x01. */
	err = modbus_read_coils(client_iface, MODBUS_UNIT_ID, 0, &coil_bitmap, 1);
	if (err != 0) {
		printf("[modbus] read coil failed: %d\n", err);
		return 0;
	}
	printf("[modbus] coils[0..7]=0x%02x\n", (unsigned int)coil_bitmap);

	printf("[modbus] done\n");
	return 0;
}
