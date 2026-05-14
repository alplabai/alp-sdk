/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SBUS receiver decoder.  SBUS is Futaba's 16-channel digital RC
 * protocol.  Each frame is 25 bytes long, sent every 7-14 ms at
 * 100000 baud / 8E2 / inverted.  This decoder runs on the host
 * SoC + assumes the carrier provides the polarity inverter.
 */

#ifndef DRONE_AUTOPILOT_SBUS_H
#define DRONE_AUTOPILOT_SBUS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SBUS_FRAME_LEN 25
#define SBUS_CHANNELS  16

typedef struct {
    uint16_t channel[SBUS_CHANNELS];  /**< 0..2047 raw counts. */
    bool     frame_lost;
    bool     failsafe;
} sbus_frame_t;

/** Decode one SBUS frame from a 25-byte buffer.
 *  @return true if the start/end bytes match SBUS magic. */
bool sbus_decode(const uint8_t buf[SBUS_FRAME_LEN], sbus_frame_t *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_AUTOPILOT_SBUS_H */
