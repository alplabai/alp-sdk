/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SBUS decoder.  The 25-byte frame layout is:
 *   byte 0:       0x0F  (start)
 *   bytes 1..22:  16 × 11-bit channels packed LSB-first
 *   byte 23:      flags (frame_lost / failsafe / digital channels)
 *   byte 24:      0x00 (end)
 */

#include "sbus.h"

#define SBUS_START 0x0F
#define SBUS_END 0x00

bool sbus_decode(const uint8_t buf[SBUS_FRAME_LEN], sbus_frame_t *out)
{
	if (buf[0] != SBUS_START) return false;
	if (buf[24] != SBUS_END) return false;

	/* Unpack 16 × 11-bit channels.  The bit-stream starts at
     * buf[1] and runs 16*11 = 176 bits = 22 bytes long. */
	uint32_t bits   = 0;
	int      n_bits = 0;
	int      ch     = 0;
	for (int i = 1; i <= 22 && ch < SBUS_CHANNELS; i++) {
		bits |= ((uint32_t)buf[i]) << n_bits;
		n_bits += 8;
		while (n_bits >= 11 && ch < SBUS_CHANNELS) {
			out->channel[ch++] = bits & 0x7FF;
			bits >>= 11;
			n_bits -= 11;
		}
	}

	/* Flags. */
	out->frame_lost = (buf[23] & 0x04) != 0;
	out->failsafe   = (buf[23] & 0x08) != 0;
	return true;
}
