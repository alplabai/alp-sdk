/* src/audio/tr_amp_ready.c -- see tr_amp_ready.h. Pure C, host + HP. */
#include "tr_amp_ready.h"

bool tr_amp_wait_ack(const tr_amp_io_t *io,
                     uint8_t            addr,
                     uint32_t           poll_us,
                     uint32_t           poll_max_us,
                     uint32_t          *tries,
                     uint32_t          *waited_us)
{
	uint64_t t0 = io->now_us();

	*tries = 0u;
	for (;;) {
		(*tries)++;
		bool     ok = io->ack(io->ctx, addr);
		uint64_t dt = io->now_us() - t0;

		*waited_us = dt > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)dt;
		if (ok) {
			return true;
		}
		if (dt >= poll_max_us) {
			return false;
		}
		io->sleep_us(poll_us);
	}
}
