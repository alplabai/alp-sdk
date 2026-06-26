/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * SBUS frame decoder for the drone-autopilot example.  SBUS is Futaba's
 * 16-channel digital RC link: the receiver emits one 25-byte frame every
 * 7-14 ms at 100000 baud, 8E2, electrically INVERTED.  That inversion is a
 * hardware quirk -- a stock UART sees idle-low garbage unless the board adds a
 * polarity inverter (or the SoC's UART supports RX invert).  This module is the
 * pure, testable half: it assumes the bytes have already arrived and been
 * de-inverted into a 25-byte buffer, and turns that buffer into channel counts.
 * Keeping decode free of any peripheral call is why it unit-tests on the host.
 *
 * The 25-byte frame layout is:
 *   byte 0:       0x0F  (start)
 *   bytes 1..22:  16 × 11-bit channels packed LSB-first
 *   byte 23:      flags (frame_lost / failsafe / digital channels)
 *   byte 24:      0x00 (end)
 */

#include "sbus.h"

#define SBUS_START 0x0F
#define SBUS_END   0x00

bool sbus_decode(const uint8_t buf[SBUS_FRAME_LEN], sbus_frame_t *out)
{
	/* Reject anything whose start/end markers don't match: cheap framing
	 * sanity-check that catches a mid-frame UART resync before we trust the
	 * payload.  A real receiver task would then realign on the next 0x0F. */
	if (buf[0] != SBUS_START) return false;
	if (buf[24] != SBUS_END) return false;

	/* Unpack 16 × 11-bit channels.  The bit-stream starts at buf[1] and runs
	 * 16*11 = 176 bits = 22 bytes long.  SBUS packs channels LSB-first across
	 * byte boundaries, so a fixed byte->channel map won't work.  Instead we
	 * shift each incoming byte into a little accumulator (`bits`) and drain a
	 * channel whenever >= 11 bits are buffered -- the same sliding-window trick
	 * any LSB-first bit-stream needs.  Mask 0x7FF keeps the low 11 bits. */
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

	/* Flag byte: bit2 = frame_lost (this frame was dropped on the air, channels
	 * are stale), bit3 = failsafe (receiver lost the transmitter entirely).
	 * Autopilot code must treat failsafe as "cut to safe defaults", not "hold
	 * last stick" -- so surface both rather than silently passing channels on. */
	out->frame_lost = (buf[23] & 0x04) != 0;
	out->failsafe   = (buf[23] & 0x08) != 0;
	return true;
}
