/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: shared command-handler table.
 *
 * Both the SPI transport (default) and the SDIO transport (optional,
 * customer-selectable) feed validated request frames into
 * protocol_dispatch() here -- one command set, one set of reply codes,
 * transport-agnostic (the gd32-bridge SPI + I2C model).
 *
 * v0.1 ("bring-up") scope: the META group only (PING, GET_VERSION,
 * GET_MAC, RESET).  Every other opcode -- Wi-Fi, BLE, GPIO proxy,
 * power, diagnostics -- is rejected with ALP_CC3501E_RESP_ERR_INVALID
 * per the protocol header's contract ("v1 firmware rejects opcodes it
 * does not implement with ALP_CC3501E_RESP_ERR_INVALID").  Those land
 * in v0.2+ and route to TI's SimpleLink Wi-Fi / BLE APIs through the
 * HAL backend; see docs/cc3501e-bridge.md "v0.x roadmap".
 *
 * Handlers that touch hardware (MAC read, self-reset) call the
 * cc3501e_hw_* shims declared in ../hal/cc3501e_hw.h.  The stub backend
 * (hal/cc3501e_hw_stub.c) keeps the protocol path exercisable on the
 * host with no TI SDK; the real bodies live in hal/ti/ and link against
 * TI's SimpleLink CC33xx SDK on the bench build.
 */

#include <string.h>

#include "protocol.h"
#include "../hal/cc3501e_hw.h"

/* --------------------------------------------------------------- */
/* Little-endian helpers (firmware side, parallel to the host).      */
/* --------------------------------------------------------------- */

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

/* --------------------------------------------------------------- */
/* META handlers (opcodes 0x00..0x0F)                                */
/* --------------------------------------------------------------- */

/* PING (0x00): liveness probe.  Empty request, empty reply data --
 * the bare RESP_OK status the transport prepends is the "firmware is
 * alive" signal the host's first post-boot handshake waits for. */
static alp_cc3501e_resp_t handle_ping(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                      size_t reply_cap, size_t *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	*reply_data_len = 0u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_VERSION (0x01): wire-protocol compatibility gate.  Returns the
 * 16-bit ALP_CC3501E_PROTOCOL_VERSION (LE); the host refuses to talk
 * to a firmware whose value != its compile-time ALP_CC3501E_PROTOCOL_VERSION.
 *
 * This returns the PROTOCOL version, not the firmware RELEASE version
 * (firmware-version.txt), matching the host driver's compatibility
 * check `version != ALP_CC3501E_PROTOCOL_VERSION` and the chip-header
 * doc.  (The diag-struct comment in <alp/protocol/cc3501e.h> that says
 * GET_VERSION returns the release version is a documentation
 * discrepancy -- tracked in DESIGN.md; the release version is reported
 * separately via GET_DIAG_INFO.fw_version in v2 firmware.) */
static alp_cc3501e_resp_t handle_get_version(const uint8_t *req, size_t req_len,
                                             uint8_t *reply_data, size_t reply_cap,
                                             size_t *reply_data_len)
{
	(void)req;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 2u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	put_le16(reply_data, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION);
	*reply_data_len = 2u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_MAC (0x03): the CC3501E's factory MAC (6 bytes, big-endian wire
 * order as TI stores it).  Read from the radio subsystem via the HAL;
 * the stub backend returns the documented all-zero placeholder. */
static alp_cc3501e_resp_t handle_get_mac(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                         size_t reply_cap, size_t *reply_data_len)
{
	(void)req;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 6u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	const int rv = cc3501e_hw_get_mac(reply_data);
	if (rv == CC3501E_HW_ERR_NOTIMPL) return ALP_CC3501E_RESP_ERR_NOT_READY;
	if (rv != CC3501E_HW_OK) return ALP_CC3501E_RESP_ERR_RADIO;
	*reply_data_len = 6u;
	return ALP_CC3501E_RESP_OK;
}

/* RESET (0x02): host-requested soft reset.  Reply OK now; the HAL
 * defers the actual reboot until after the reply has been clocked back
 * to the host (so the host sees the ack), then resets the chip -- which
 * the host detects as the firmware going quiet then re-PINGing alive.
 * On the stub backend the deferred reset is a no-op. */
static alp_cc3501e_resp_t handle_reset(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                       size_t reply_cap, size_t *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	cc3501e_hw_request_reset();
	*reply_data_len = 0u;
	return ALP_CC3501E_RESP_OK;
}

/* --------------------------------------------------------------- */
/* GPIO proxy (0x50..0x5F) + camera enables (0x60..0x61)             */
/* --------------------------------------------------------------- */

/* Map a HAL return code (CC3501E_HW_*) onto a wire response status.
 * NOTIMPL -> NOT_READY (the op is not wired on this backend yet);
 * INVAL -> INVALID; any other failure -> RADIO (generic HW/IO). */
static alp_cc3501e_resp_t hw_to_resp(int rv)
{
	switch (rv) {
	case CC3501E_HW_OK:
		return ALP_CC3501E_RESP_OK;
	case CC3501E_HW_ERR_NOTIMPL:
		return ALP_CC3501E_RESP_ERR_NOT_READY;
	case CC3501E_HW_ERR_INVAL:
		return ALP_CC3501E_RESP_ERR_INVALID;
	default:
		return ALP_CC3501E_RESP_ERR_RADIO;
	}
}

/* GPIO_CONFIGURE (0x50): set direction + pull on a proxied CC3501E pad. */
static alp_cc3501e_resp_t handle_gpio_configure(const uint8_t *req, size_t req_len,
                                                uint8_t *reply_data, size_t reply_cap,
                                                size_t *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_configure_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_configure_t *c = (const alp_cc3501e_gpio_configure_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_configure(c->cc3501e_gpio, c->direction, c->pull));
}

/* GPIO_WRITE (0x51): drive a proxied pad (open-drain semantics in the HAL). */
static alp_cc3501e_resp_t handle_gpio_write(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                            size_t reply_cap, size_t *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_write_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_write_t *w = (const alp_cc3501e_gpio_write_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_write(w->cc3501e_gpio, w->level));
}

/* GPIO_READ (0x52): request payload = 1 byte (cc3501e_gpio); reply data =
 * the sampled level (1 byte). */
static alp_cc3501e_resp_t handle_gpio_read(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                           size_t reply_cap, size_t *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 1u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t            level = 0u;
	alp_cc3501e_resp_t st    = hw_to_resp(cc3501e_hw_gpio_read(req[0], &level));
	if (st == ALP_CC3501E_RESP_OK) {
		reply_data[0]   = level;
		*reply_data_len = 1u;
	}
	return st;
}

/* GPIO_SET_INTERRUPT (0x53): arm/disable an edge IRQ on a proxied pad.  The
 * async EVT_GPIO_INTERRUPT delivery needs the next-rev host-IRQ line; this
 * rev accepts the config (event delivery lands with r2 -- see DESIGN.md). */
static alp_cc3501e_resp_t handle_gpio_set_interrupt(const uint8_t *req, size_t req_len,
                                                    uint8_t *reply_data, size_t reply_cap,
                                                    size_t *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_set_interrupt_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_set_interrupt_t *s = (const alp_cc3501e_gpio_set_interrupt_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_set_interrupt(s->cc3501e_gpio, s->edge, s->enabled));
}

/* CAM_ENABLE (0x60) / CAM_DISABLE (0x61): drive CAM_EN_LDO0/1.  Request
 * payload = 1 byte (which: 0 -> LDO0, 1 -> LDO1). */
static alp_cc3501e_resp_t handle_cam_enable(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                            size_t reply_cap, size_t *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 1u));
}

static alp_cc3501e_resp_t handle_cam_disable(const uint8_t *req, size_t req_len, uint8_t *reply_data,
                                             size_t reply_cap, size_t *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 0u));
}

/* --------------------------------------------------------------- */
/* Dispatch                                                          */
/* --------------------------------------------------------------- */

typedef alp_cc3501e_resp_t (*cmd_handler_t)(const uint8_t *, size_t, uint8_t *, size_t, size_t *);

/* Sparse switch on opcode -- keeps the table small without losing the
 * single-handler-table property.  v0.2+ feature groups slot in here as
 * their HAL bodies land. */
alp_cc3501e_resp_t protocol_dispatch(uint8_t cmd, uint8_t flags, const uint8_t *req, size_t req_len,
                                     uint8_t *reply_data, size_t reply_cap, size_t *reply_data_len)
{
	(void)flags; /* v0.1 has no request flag that alters dispatch. */
	*reply_data_len = 0u;

	cmd_handler_t h = NULL;
	switch (cmd) {
	case ALP_CC3501E_CMD_PING:
		h = handle_ping;
		break;
	case ALP_CC3501E_CMD_GET_VERSION:
		h = handle_get_version;
		break;
	case ALP_CC3501E_CMD_GET_MAC:
		h = handle_get_mac;
		break;
	case ALP_CC3501E_CMD_RESET:
		h = handle_reset;
		break;
	/* GPIO proxy + camera enables (v0.4). */
	case ALP_CC3501E_CMD_GPIO_CONFIGURE:
		h = handle_gpio_configure;
		break;
	case ALP_CC3501E_CMD_GPIO_WRITE:
		h = handle_gpio_write;
		break;
	case ALP_CC3501E_CMD_GPIO_READ:
		h = handle_gpio_read;
		break;
	case ALP_CC3501E_CMD_GPIO_SET_INTERRUPT:
		h = handle_gpio_set_interrupt;
		break;
	case ALP_CC3501E_CMD_CAM_ENABLE:
		h = handle_cam_enable;
		break;
	case ALP_CC3501E_CMD_CAM_DISABLE:
		h = handle_cam_disable;
		break;
	default:
		/* Unknown, or a known v1 opcode whose firmware body has not
         * landed yet (all of Wi-Fi / BLE / GPIO / power / diag in
         * v0.1).  The header's contract is RESP_ERR_INVALID. */
		return ALP_CC3501E_RESP_ERR_INVALID;
	}
	return h(req, req_len, reply_data, reply_cap, reply_data_len);
}

/* --------------------------------------------------------------- */
/* Transport-agnostic framing                                        */
/* --------------------------------------------------------------- */

size_t protocol_build_reply(const uint8_t *req_frame, size_t req_len, uint8_t *reply_frame,
                            size_t reply_cap)
{
	/* The caller guarantees a full-size reply buffer; guard anyway. */
	if (reply_cap < CC3501E_REPLY_DATA_OFF) {
		return 0u;
	}

	const uint8_t      cmd_echo = (req_len >= 1u) ? req_frame[0] : 0u;
	alp_cc3501e_resp_t status   = ALP_CC3501E_RESP_ERR_PROTOCOL;
	size_t             data_len = 0u;

	if (req_len >= (size_t)ALP_CC3501E_HEADER_BYTES) {
		const uint8_t  flags       = req_frame[1];
		const uint16_t payload_len = (uint16_t)req_frame[2] | ((uint16_t)req_frame[3] << 8);

		/* Captured byte count must match the declared payload exactly. */
		if ((size_t)ALP_CC3501E_HEADER_BYTES + (size_t)payload_len == req_len) {
			const uint8_t *req = (payload_len > 0u) ? &req_frame[ALP_CC3501E_HEADER_BYTES] : NULL;
			status             = protocol_dispatch(cmd_echo, flags, req, payload_len,
			                                       &reply_frame[CC3501E_REPLY_DATA_OFF],
			                                       reply_cap - CC3501E_REPLY_DATA_OFF, &data_len);
		}
	}

	/* Frame the reply: [cmd | flags=0 | payload_len(LE) | status | data].
     * flags = 0 -> solicited reply (async events set FLAG_ASYNC_EVENT in
     * v0.2+).  payload = status(1) + data. */
	const uint16_t reply_payload          = (uint16_t)(1u + data_len);
	reply_frame[0]                        = cmd_echo;
	reply_frame[1]                        = 0u;
	reply_frame[2]                        = (uint8_t)(reply_payload & 0xFFu);
	reply_frame[3]                        = (uint8_t)((reply_payload >> 8) & 0xFFu);
	reply_frame[CC3501E_REPLY_STATUS_OFF] = (uint8_t)status;
	return (size_t)ALP_CC3501E_HEADER_BYTES + reply_payload;
}
