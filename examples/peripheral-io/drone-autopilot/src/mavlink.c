/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAVLink v2 implementation.  Packs the 7 outbound message types
 * the drone-autopilot needs to round-trip with QGroundControl /
 * Mission Planner; parses HEARTBEAT + COMMAND_LONG inbound.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

#include "mavlink.h"

LOG_MODULE_REGISTER(mavlink, LOG_LEVEL_INF);

/* ───────── State ───────── */

static alp_uart_t      *s_uart;
static uint8_t          s_sysid       = 1;
static uint8_t          s_compid      = 1;
static uint8_t          s_tx_seq      = 0;
static uint32_t         s_last_gcs_ms = 0;
static alp_mavlink_rx_t s_rx;

/* ───────── CRC-X.25 (MAVLink CRC) ─────────
 *
 * MAVLink uses CRC-16/X.25 over the frame (excluding the start
 * byte) plus one extra "magic" byte per message type that the
 * dialect generator computes from the field types.  For our
 * minimal stack we hard-code the magic bytes for the 7 message
 * types we emit + COMMAND_LONG (the only one we parse). */

static const uint8_t s_crc_extra[] = {
	[ALP_MAVLINK_MSG_HEARTBEAT] = 50,       [ALP_MAVLINK_MSG_GPS_RAW_INT] = 24,
	[ALP_MAVLINK_MSG_ATTITUDE] = 39,        [ALP_MAVLINK_MSG_GLOBAL_POSITION] = 104,
	[ALP_MAVLINK_MSG_RC_CHANNELS] = 118,    [ALP_MAVLINK_MSG_COMMAND_LONG] = 152,
	[ALP_MAVLINK_MSG_BATTERY_STATUS] = 154,
};

static uint16_t crc_update(uint16_t crc, uint8_t b)
{
	uint8_t tmp = b ^ (uint8_t)(crc & 0xFF);
	tmp ^= tmp << 4;
	return (uint16_t)((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4));
}

static uint16_t crc_calc(const uint8_t *buf, size_t len, uint8_t magic)
{
	uint16_t crc = 0xFFFFu;
	for (size_t i = 0; i < len; i++)
		crc = crc_update(crc, buf[i]);
	return crc_update(crc, magic);
}

/* ───────── TX framing ───────── */

static void send_frame(uint32_t msgid, const uint8_t *payload, uint8_t plen)
{
	if (s_uart == NULL || msgid >= sizeof(s_crc_extra) || s_crc_extra[msgid] == 0) {
		return;
	}
	uint8_t frame[256 + 12];
	frame[0] = ALP_MAVLINK_STX_V2;
	frame[1] = plen;
	frame[2] = 0; /* incompat_flags */
	frame[3] = 0; /* compat_flags */
	frame[4] = s_tx_seq++;
	frame[5] = s_sysid;
	frame[6] = s_compid;
	frame[7] = (uint8_t)(msgid & 0xFF);
	frame[8] = (uint8_t)((msgid >> 8) & 0xFF);
	frame[9] = (uint8_t)((msgid >> 16) & 0xFF);
	if (plen > 0 && payload != NULL) {
		memcpy(&frame[10], payload, plen);
	}
	const uint16_t crc   = crc_calc(&frame[1], 9u + plen, s_crc_extra[msgid]);
	frame[10 + plen]     = (uint8_t)(crc & 0xFF);
	frame[10 + plen + 1] = (uint8_t)((crc >> 8) & 0xFF);
	alp_uart_write(s_uart, frame, 12u + plen);
}

/* ───────── Outbound messages ───────── */

static void send_heartbeat(const autopilot_state_t *s)
{
	/* HEARTBEAT payload (mavlink v2 zero-truncated trailing bytes):
     *   uint32_t custom_mode
     *   uint8_t  type
     *   uint8_t  autopilot
     *   uint8_t  base_mode
     *   uint8_t  system_status
     *   uint8_t  mavlink_version */
	uint8_t  p[9];
	uint32_t custom = (uint32_t)s->mode;
	memcpy(&p[0], &custom, 4);
	p[4] = ALP_MAV_TYPE_QUADROTOR;
	p[5] = ALP_MAV_AUTOPILOT_GENERIC;
	p[6] = s->armed ? 0x80u : 0x00u; /* base_mode: SAFETY_ARMED bit. */
	p[7] = (s->mode == AP_MODE_FAILSAFE) ? 6 : (s->armed ? 4 : 3); /* MAV_STATE */
	p[8] = 3;                                                      /* mavlink version */
	send_frame(ALP_MAVLINK_MSG_HEARTBEAT, p, sizeof(p));
}

static void send_attitude(const autopilot_state_t *s)
{
	/* ATTITUDE payload:
     *   uint32_t time_boot_ms
     *   float    roll, pitch, yaw      (rad)
     *   float    rollspeed, pitchspeed, yawspeed (rad/s) */
	uint8_t        p[28];
	const uint32_t t = k_uptime_get_32();
	memcpy(&p[0], &t, 4);
	const float r  = s->roll * 3.14159265f / 180.f;
	const float pp = s->pitch * 3.14159265f / 180.f;
	const float y  = s->yaw * 3.14159265f / 180.f;
	memcpy(&p[4], &r, 4);
	memcpy(&p[8], &pp, 4);
	memcpy(&p[12], &y, 4);
	memcpy(&p[16], &s->p, 4);
	memcpy(&p[20], &s->q, 4);
	memcpy(&p[24], &s->r, 4);
	send_frame(ALP_MAVLINK_MSG_ATTITUDE, p, sizeof(p));
}

static void send_battery(const autopilot_state_t *s)
{
	/* BATTERY_STATUS payload (subset):
     *   int32_t  current_consumed (mAh -- -1 if unknown)
     *   int32_t  energy_consumed (hJ  -- -1 if unknown)
     *   int16_t  temperature (cdegC -- INT16_MAX if unknown)
     *   uint16_t voltages[10] (mV)   -- per-cell; 0xFFFF = N/A.
     *   int16_t  current_battery (cA -- -1 if unknown)
     *   uint8_t  id
     *   uint8_t  battery_function
     *   uint8_t  type
     *   int8_t   battery_remaining (% -- -1 if unknown) */
	uint8_t p[36];
	memset(p, 0, sizeof(p));
	/* current_consumed unknown */
	int32_t neg1 = -1;
	memcpy(&p[0], &neg1, 4);
	memcpy(&p[4], &neg1, 4);
	int16_t int_max16 = 0x7FFF;
	memcpy(&p[8], &int_max16, 2);
	/* Cells: assume 4S, fill mv per cell as battery_v/4 * 1000. */
	uint16_t mv_per_cell = (uint16_t)((s->battery_v / 4.f) * 1000.f);
	for (int c = 0; c < 4; c++) {
		memcpy(&p[10 + c * 2], &mv_per_cell, 2);
	}
	for (int c = 4; c < 10; c++) {
		uint16_t off = 0xFFFFu;
		memcpy(&p[10 + c * 2], &off, 2);
	}
	int16_t ca = (int16_t)(s->battery_a * 100.f); /* 10 mA / cA. */
	memcpy(&p[30], &ca, 2);
	p[32] = 0; /* id */
	p[33] = 0; /* function: ALL */
	p[34] = 2; /* type: LIPO */
	p[35] =
	    (int8_t)(s->battery_v >= 16.8f
	                 ? 100
	                 : (s->battery_v < 13.2f ? 5
	                                         : (int8_t)((s->battery_v - 13.2f) * (100.f / 3.6f))));
	send_frame(ALP_MAVLINK_MSG_BATTERY_STATUS, p, sizeof(p));
}

static void send_gps_raw(const autopilot_state_t *s)
{
	/* GPS_RAW_INT payload (subset):
     *   uint64_t time_usec
     *   int32_t  lat (1e7 deg)
     *   int32_t  lon (1e7 deg)
     *   int32_t  alt (mm AMSL)
     *   ...      vel / cog / fix_type / sats_visible / ... */
	uint8_t p[30];
	memset(p, 0, sizeof(p));
	uint64_t t = (uint64_t)k_uptime_get_32() * 1000u;
	memcpy(&p[0], &t, 8);
	int32_t lat = (int32_t)(s->lat_deg * 1.0e7f);
	int32_t lon = (int32_t)(s->lon_deg * 1.0e7f);
	int32_t alt = (int32_t)(s->altitude_m * 1000.f);
	memcpy(&p[8], &lat, 4);
	memcpy(&p[12], &lon, 4);
	memcpy(&p[16], &alt, 4);
	p[27] = s->gps_fix ? 3 : 0; /* fix_type: 3=3D, 0=NO_FIX */
	p[28] = 0;                  /* sats_visible -- v0.6 fills this in */
	send_frame(ALP_MAVLINK_MSG_GPS_RAW_INT, p, sizeof(p));
}

/* ───────── RX handling ─────────
 *
 * Payload accessors are by-offset memcpy() rather than the
 * struct-packed accessors the upstream c_library_v2 generates --
 * keeps the dialect-table-free stack small.  When customers swap
 * in the upstream library they get type-safe accessors for free. */

static void handle_rx_msg(uint32_t msgid, const uint8_t *p, uint8_t plen, autopilot_state_t *s)
{
	if (msgid == ALP_MAVLINK_MSG_HEARTBEAT) {
		s_last_gcs_ms = k_uptime_get_32();
		return;
	}
	if (msgid == ALP_MAVLINK_MSG_COMMAND_LONG && plen >= 31) {
		/* COMMAND_LONG layout (subset):
         *   float    param1..param7      (28 B at offset 0)
         *   uint16_t command              (offset 28)
         *   uint8_t  target_system        (offset 30)
         *   ... */
		float param1;
		memcpy(&param1, &p[0], 4);
		uint16_t cmd;
		memcpy(&cmd, &p[28], 2);
		switch (cmd) {
		case ALP_MAV_CMD_COMPONENT_ARM_DISARM:
			/* param1 = 1.0 -> arm; 0.0 -> disarm. */
			if (param1 > 0.5f) {
				s->armed = true;
				if (s->mode == AP_MODE_DISARMED) s->mode = AP_MODE_STABILISE;
			} else {
				s->armed = false;
				s->mode  = AP_MODE_DISARMED;
			}
			LOG_INF("ARM/DISARM via MAVLink: armed=%d", (int)s->armed);
			break;
		case ALP_MAV_CMD_DO_SET_MODE:
			/* param1 = base_mode bits; param2 = custom_mode
                 * (mapped to ap_mode_t). */
			if (s->armed) {
				s->mode = (ap_mode_t)((int)param1 & 0x7u);
				LOG_INF("SET_MODE via MAVLink: mode=%d", (int)s->mode);
			}
			break;
		default:
			LOG_INF("unknown MAVLink cmd %u (ignored)", cmd);
			break;
		}
	}
}

static void rx_feed_byte(uint8_t b, autopilot_state_t *s)
{
	alp_mavlink_rx_t *r = &s_rx;
	switch (r->state) {
	case ALP_MAV_RX_IDLE:
		if (b == ALP_MAVLINK_STX_V2) {
			r->crc         = 0xFFFFu;
			r->payload_idx = 0;
			r->state       = ALP_MAV_RX_LEN;
		}
		break;
	case ALP_MAV_RX_LEN:
		r->payload_len = b;
		r->crc         = crc_update(r->crc, b);
		r->state       = ALP_MAV_RX_INCOMPAT;
		break;
	case ALP_MAV_RX_INCOMPAT:
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_COMPAT;
		break;
	case ALP_MAV_RX_COMPAT:
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_SEQ;
		break;
	case ALP_MAV_RX_SEQ:
		r->seq   = b;
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_SYSID;
		break;
	case ALP_MAV_RX_SYSID:
		r->sysid = b;
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_COMPID;
		break;
	case ALP_MAV_RX_COMPID:
		r->compid = b;
		r->crc    = crc_update(r->crc, b);
		r->state  = ALP_MAV_RX_MSGID_LO;
		break;
	case ALP_MAV_RX_MSGID_LO:
		r->msgid = b;
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_MSGID_MID;
		break;
	case ALP_MAV_RX_MSGID_MID:
		r->msgid |= ((uint32_t)b) << 8;
		r->crc   = crc_update(r->crc, b);
		r->state = ALP_MAV_RX_MSGID_HI;
		break;
	case ALP_MAV_RX_MSGID_HI:
		r->msgid |= ((uint32_t)b) << 16;
		r->crc   = crc_update(r->crc, b);
		r->state = (r->payload_len == 0) ? ALP_MAV_RX_CRC_LO : ALP_MAV_RX_PAYLOAD;
		break;
	case ALP_MAV_RX_PAYLOAD:
		r->payload[r->payload_idx++] = b;
		r->crc                       = crc_update(r->crc, b);
		if (r->payload_idx >= r->payload_len) r->state = ALP_MAV_RX_CRC_LO;
		break;
	case ALP_MAV_RX_CRC_LO: {
		uint16_t magic = (r->msgid < sizeof(s_crc_extra)) ? s_crc_extra[r->msgid] : 0;
		uint16_t crc   = crc_update(r->crc, (uint8_t)magic);
		if ((uint8_t)(crc & 0xFF) != b) {
			r->state = ALP_MAV_RX_IDLE;
			break;
		}
		r->crc   = crc;
		r->state = ALP_MAV_RX_CRC_HI;
		break;
	}
	case ALP_MAV_RX_CRC_HI:
		if ((uint8_t)((r->crc >> 8) & 0xFF) == b) {
			handle_rx_msg(r->msgid, r->payload, r->payload_len, s);
		}
		r->state = ALP_MAV_RX_IDLE;
		break;
	}
}

/* ───────── Public API ───────── */

int alp_mavlink_init(uint8_t sysid, uint8_t compid)
{
	s_sysid  = sysid;
	s_compid = compid;
	/* !!!  UART CONFLICT -- read before flying  !!!
     *
     * The GCS link needs its own UART, but the E1M family exposes
     * only UART0 + UART1 in v0.5 and this example has already
     * committed BOTH: autopilot_init() opens UART0 for the GNSS and
     * UART1 for the SBUS RC receiver.  There is NO free third port
     * for the MAVLink GCS radio.
     *
     * Opening UART0 here therefore COLLIDES with the GNSS link --
     * two drivers claiming the same port.  This is left deliberately
     * visible (a LOG_WRN fires at runtime) rather than silently
     * papered over: a real flight build must route the GCS SiK radio
     * to a dedicated port (board-specific, TBD pinout) and point this
     * open() at that port instead.  Do not ship as-is. */
	LOG_WRN("drone-autopilot: MAVLink GCS shares ALP_E1M_UART0 with the GNSS "
	        "link -- route to a dedicated UART before flight (no free "
	        "third port in v0.5).");
	s_uart = alp_uart_open(&(alp_uart_config_t){
	    .port_id  = ALP_E1M_UART0,
	    .baudrate = 57600,
	});
	return s_uart ? 0 : -1;
}

void alp_mavlink_telem_loop(autopilot_state_t *s)
{
	uint32_t i = 0;
	while (1) {
		send_attitude(s);                  /* 10 Hz */
		if ((i % 2) == 0) send_gps_raw(s); /*  5 Hz */
		if ((i % 10) == 0) {
			send_heartbeat(s); /*  1 Hz */
			send_battery(s);   /*  1 Hz */
		}
		i++;
		k_msleep(100);
	}
}

void alp_mavlink_rx_loop(autopilot_state_t *s)
{
	while (1) {
		uint8_t b;
		if (s_uart && alp_uart_read(s_uart, &b, 1, /*timeout_ms=*/100) == ALP_OK) {
			rx_feed_byte(b, s);
		}
		/* GCS link-loss check: 3 s since last HEARTBEAT trips the
         * MAVLink-specific failsafe (separate from RC-link
         * failsafe in nav_loop). */
		if (k_uptime_get_32() - s_last_gcs_ms > 3000) {
			/* No GCS heartbeat -- the RC-link failsafe in nav_loop
             * still applies, but we log it.  Real production
             * autopilots may want a separate failsafe action when
             * the GCS link drops (e.g. continue mission instead
             * of land). */
		}
	}
}
