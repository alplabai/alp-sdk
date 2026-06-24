/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAVLink v2 ground-station link for the drone-autopilot.
 *
 * This is a deliberately minimal MAVLink stack -- enough to talk
 * to QGroundControl / Mission Planner over a serial telemetry
 * radio.  Customers wanting the full message set (200+ message
 * types) drop the upstream
 *
 *     https://github.com/mavlink/c_library_v2
 *
 * into the project's include path; this file's pack/parse helpers
 * are designed to coexist with that header (different symbol
 * prefix: `alp_mavlink_*` vs `mavlink_*`).
 *
 *
 * ── What this stack carries ────────────────────────────────────
 *
 * In v0.5 the GCS link shares UART0 with the GNSS port at the C
 * level (the only two-UART E1M family is short on dedicated radio
 * ports).  Real builds wire the SiK radio to a dedicated board
 * UART that the TBD board overlay exposes.
 *
 *   Tx (autopilot → ground station):
 *     - HEARTBEAT       (msg-id 0)    -- 1 Hz, always.
 *     - ATTITUDE        (msg-id 30)   -- 10 Hz.
 *     - GLOBAL_POSITION_INT (msg-id 33) -- 5 Hz.
 *     - GPS_RAW_INT     (msg-id 24)   -- 5 Hz.
 *     - BATTERY_STATUS  (msg-id 147)  -- 1 Hz.
 *     - RC_CHANNELS     (msg-id 65)   -- 5 Hz.
 *
 *   Rx (ground station → autopilot):
 *     - COMMAND_LONG    (msg-id 76)   -- arm/disarm, mode change.
 *     - HEARTBEAT       (msg-id 0)    -- keepalive; we use it
 *                                         to detect GCS link loss.
 *
 * ── Wire format ────────────────────────────────────────────────
 *
 * MAVLink v2 frame (11 + payload + 2 bytes CRC):
 *
 *   ┌──────┬──────┬──────┬──────┬──────┬──────┬───────────────┬──────┐
 *   │ 0xFD │ plen │ icmp │ csig │ seq  │ sysid│ msgid (3 B)   │ ...  │
 *   └──────┴──────┴──────┴──────┴──────┴──────┴───────────────┴──────┘
 *
 *   followed by `plen` bytes of payload + 2 bytes CRC-X.25.
 *
 * v0.5 stub: pack/parse the 7 message types above with hand-coded
 * marshalling.  Real customers swap in the auto-generated upstream
 * code when they need the full message catalogue.
 */

#ifndef DRONE_AUTOPILOT_MAVLINK_H
#define DRONE_AUTOPILOT_MAVLINK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "autopilot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MAVLink v2 magic + message IDs we handle. */
#define ALP_MAVLINK_STX_V2              0xFDu
#define ALP_MAVLINK_MSG_HEARTBEAT       0u
#define ALP_MAVLINK_MSG_GPS_RAW_INT     24u
#define ALP_MAVLINK_MSG_ATTITUDE        30u
#define ALP_MAVLINK_MSG_GLOBAL_POSITION 33u
#define ALP_MAVLINK_MSG_RC_CHANNELS     65u
#define ALP_MAVLINK_MSG_COMMAND_LONG    76u
#define ALP_MAVLINK_MSG_BATTERY_STATUS  147u

/* HEARTBEAT mav_type / autopilot enums (subset). */
#define ALP_MAV_TYPE_QUADROTOR    2u
#define ALP_MAV_AUTOPILOT_GENERIC 0u

/* COMMAND_LONG command IDs we honour. */
#define ALP_MAV_CMD_COMPONENT_ARM_DISARM 400u
#define ALP_MAV_CMD_DO_SET_MODE          176u

/** Frame-parser state machine. */
typedef struct {
	enum {
		ALP_MAV_RX_IDLE = 0,
		ALP_MAV_RX_LEN,
		ALP_MAV_RX_INCOMPAT,
		ALP_MAV_RX_COMPAT,
		ALP_MAV_RX_SEQ,
		ALP_MAV_RX_SYSID,
		ALP_MAV_RX_COMPID,
		ALP_MAV_RX_MSGID_LO,
		ALP_MAV_RX_MSGID_MID,
		ALP_MAV_RX_MSGID_HI,
		ALP_MAV_RX_PAYLOAD,
		ALP_MAV_RX_CRC_LO,
		ALP_MAV_RX_CRC_HI,
	} state;
	uint8_t  payload_len;
	uint8_t  seq;
	uint8_t  sysid;
	uint8_t  compid;
	uint32_t msgid;
	uint8_t  payload[256];
	uint16_t payload_idx;
	uint16_t crc;
} alp_mavlink_rx_t;

/** Bring up the MAVLink link.  @return 0 on success. */
int alp_mavlink_init(uint8_t sysid, uint8_t compid);

/** Telemetry loop -- emits HEARTBEAT + ATTITUDE + GPS + BATTERY
 *  at the configured rates from the autopilot state snapshot. */
void alp_mavlink_telem_loop(autopilot_state_t *s);

/** Receive loop -- read frames from the GCS UART, decode
 *  COMMAND_LONG, mutate the autopilot state accordingly
 *  (arm/disarm, mode change). */
void alp_mavlink_rx_loop(autopilot_state_t *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_AUTOPILOT_MAVLINK_H */
