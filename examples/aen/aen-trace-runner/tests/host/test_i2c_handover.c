/* tests/host/test_i2c_handover.c -- the flag-word protocol of the SDK's
 * alp,i2c-handover glue (zephyr/soc-bridge/alif/i2c_handover.h): a release is
 * taken exactly once, an already-taken release never satisfies a wait, the
 * documented residual window (a published but untaken release survives a warm
 * reset) behaves as documented, a DIRTY release is reported as such, and the
 * release side's per-boot nonce can never collide with the one the acquiring
 * core already consumed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../../../../zephyr/soc-bridge/alif/i2c_handover.h"

static alp_i2c_handover_t w;

/* A fake clock and liveness word: the heartbeat moves every `hb_every_us` of delay (0: never). */
static uint32_t           alive, now_us, hb_every_us, last_tick_us, delayed_us;

static void fake_delay(uint32_t us)
{
	now_us += us;
	delayed_us += us;
	if (hb_every_us != 0u && now_us - last_tick_us >= hb_every_us) {
		alive++;
		last_tick_us = now_us;
	}
}

static bool sample(bool taken)
{
	now_us = last_tick_us = delayed_us = 0u;
	return alp_i2c_handover_sample_warm(&w,
	                                    taken,
	                                    &alive,
	                                    fake_delay,
	                                    ALP_I2C_HANDOVER_ALIVE_STEP_US,
	                                    ALP_I2C_HANDOVER_ALIVE_WINDOW_US);
}

static void cold(uint32_t s, uint32_t n, uint32_t c)
{
	w.state    = s;
	w.nonce    = n;
	w.consumed = c;
}

int main(void)
{
	/* Nothing published: the wait goes on. */
	cold(0, 0, 0);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);

	/* A clean release is taken once; the take clears state and records the nonce. */
	alp_i2c_handover_reset(&w);
	alp_i2c_handover_publish(&w, 0x1234u, 0);
	assert(w.state == ALP_I2C_HANDOVER_CLEAN && w.nonce == 0x1234u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN);
	assert(w.state == 0u && w.consumed == 0x1234u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET); /* not again */

	/* A DIRTY release (the controller never went idle) is taken and reported. */
	alp_i2c_handover_reset(&w);
	alp_i2c_handover_publish(&w, 0x2222u, 1);
	assert(w.state == ALP_I2C_HANDOVER_DIRTY);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_DIRTIED);
	assert(w.consumed == 0x2222u && w.state == 0u);

	/* An already-taken release never satisfies a later wait: the take cleared state,
	 * and a magic with the nonce already in consumed is ignored too. */
	cold(ALP_I2C_HANDOVER_CLEAN, 0x3333u, 0x3333u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);
	assert(w.state == ALP_I2C_HANDOVER_CLEAN); /* untouched */

	/* The documented residual window: a release published but NOT yet taken survives a
	 * warm reset (always-on SRAM). An acquiring core that gets to its wait before the
	 * releasing core's boot-time reset of the state takes it -- harmless with the bus
	 * disabled, why both cores are reset together and the releasing core starts first. */
	cold(0, 0, 0x5555u);
	alp_i2c_handover_publish(&w, 0x6666u, 0); /* run 1: published, the acquirer is not up yet */
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN); /* run 2's early waiter */
	/* ... whereas the releasing core's reset comes first when it starts first: */
	cold(0, 0, 0x5555u);
	alp_i2c_handover_publish(&w, 0x6666u, 0);
	alp_i2c_handover_reset(&w); /* PRE_KERNEL_1 of the next boot */
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);

	/* The release side resets state at every boot: a magic left from the last run
	 * is gone before anyone can read it. */
	alp_i2c_handover_reset(&w);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);

	/* A fresh release after a taken one (a new HE boot) is a new nonce and is taken. */
	alp_i2c_handover_publish(&w, 0x4444u, 0);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN && w.consumed == 0x4444u);

	/* A nonce that happens to equal the consumed one is bumped: it must not be
	 * mistaken for the release already taken. */
	alp_i2c_handover_reset(&w);
	alp_i2c_handover_publish(&w, 0x4444u, 0);
	assert(w.nonce == 0x4445u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN);

	/* Garbage in state (cold SRAM) is not a release. */
	cold(0xDEADBEEFu, 1u, 2u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);
	cold(ALP_I2C_HANDOVER_CLEAN & ~1u, 1u, 2u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);

	/* WARM boot of the releasing core alone: the record reads "taken" (published, then consumed by
	 * the acquiring core) and the acquiring core's liveness word moves. */
	alp_i2c_handover_reset(&w);
	alp_i2c_handover_publish(&w, 0x7777u, 0);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN);
	assert(alp_i2c_handover_taken(&w));
	assert(alp_i2c_handover_is_warm(&w, 100u, 101u));
	assert(alp_i2c_handover_is_warm(&w, 0xFFFFFFFFu, 0u)); /* a wrapping counter still moved */
	assert(!alp_i2c_handover_is_warm(&w, 100u, 100u));     /* a stopped acquiring core: cold path */

	/* Cold SRAM must never read as warm, whatever it holds. Zero fill: nonce 0 is not a release. */
	cold(0, 0, 0);
	assert(!alp_i2c_handover_taken(&w) && !alp_i2c_handover_is_warm(&w, 1u, 2u));
	/* Any word-fill pattern: either state != 0, or (state 0 patterns) nonce == consumed == fill. */
	static const uint32_t fills[] = { 0xFFFFFFFFu, 0xA5A5A5A5u, 0x5A5A5A5Au, 0xDEADBEEFu,
		                              0xCCCCCCCCu, 0x55555555u, 0xAAAAAAAAu, 0x12345678u };

	for (unsigned i = 0; i < sizeof(fills) / sizeof(fills[0]); i++) {
		cold(fills[i], fills[i], fills[i]);
		assert(!alp_i2c_handover_taken(&w));
		assert(!alp_i2c_handover_is_warm(&w, 1u, 2u));
	}
	/* A release not yet taken is not a taken one (the HP has not run): nonce differs. */
	cold(0, 0x1234u, 0x1111u);
	assert(!alp_i2c_handover_taken(&w));
	/* A pending CLEAN / DIRTY release is never "taken" even with equal nonce words. */
	cold(ALP_I2C_HANDOVER_CLEAN, 5u, 5u);
	assert(!alp_i2c_handover_taken(&w));
	cold(ALP_I2C_HANDOVER_DIRTY, 5u, 5u);
	assert(!alp_i2c_handover_taken(&w));
	/* Garbage that happens to look taken (state 0, nonce == consumed != 0) still needs a moving
	 * liveness word: static SRAM never provides one. */
	cold(0, 0xDEADBEEFu, 0xDEADBEEFu);
	assert(alp_i2c_handover_taken(&w));
	assert(!alp_i2c_handover_is_warm(&w, 0xDEADBEEFu, 0xDEADBEEFu));

	/* STALE CLEAN: the releasing core died before the acquiring core took a release, then the whole
	 * SoC was reset. At PRE_KERNEL_1 the snapshot clears the untaken release, so the acquiring core
	 * (POST_KERNEL 0) cannot take it, and this core cannot mistake the later take for a warm record. */
	cold(ALP_I2C_HANDOVER_CLEAN, 0x8888u, 0x7777u);
	assert(!alp_i2c_handover_boot_snapshot(&w));
	assert(w.state == 0u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);
	cold(ALP_I2C_HANDOVER_DIRTY, 0x8888u, 0x7777u);
	assert(!alp_i2c_handover_boot_snapshot(&w) && w.state == 0u);
	/* ... the same acquire taking it afterwards would have made THIS record look taken: */
	cold(ALP_I2C_HANDOVER_CLEAN, 0x8888u, 0x7777u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_TAKEN);
	assert(alp_i2c_handover_taken(&w)); /* what the snapshot prevents the HE from ever acting on */
	/* A record that is taken is left exactly as it is. */
	cold(0, 0x9999u, 0x9999u);
	assert(alp_i2c_handover_boot_snapshot(&w) && w.state == 0u && w.nonce == 0x9999u);

	/* sample_warm against a fake clock and liveness word. */
	cold(0, 0x9999u, 0x9999u);
	alive       = 1000u;
	hb_every_us = 10000u; /* the HP's 10 ms timer */
	assert(sample(true));
	assert(delayed_us <= 6u * ALP_I2C_HANDOVER_ALIVE_STEP_US); /* returns at the first tick */
	hb_every_us = 0u; /* a stopped / not yet acquired HP */
	assert(!sample(true));
	assert(delayed_us >= ALP_I2C_HANDOVER_ALIVE_WINDOW_US); /* waited the whole window */
	/* Cold boot with a MOVING heartbeat but the record not taken: cold, and no wait at all. */
	hb_every_us = 10000u;
	assert(!sample(false) && delayed_us == 0u);
	cold(ALP_I2C_HANDOVER_CLEAN, 5u, 6u);
	assert(!sample(alp_i2c_handover_taken(&w)) && delayed_us == 0u);
	/* No liveness word on the node: never warm. */
	cold(0, 0x9999u, 0x9999u);
	assert(!alp_i2c_handover_sample_warm(
	    &w, true, NULL, fake_delay, ALP_I2C_HANDOVER_ALIVE_STEP_US, ALP_I2C_HANDOVER_ALIVE_WINDOW_US));
	/* The HP's timer must tick at least twice in the window (i2c_handover.c asserts the same). */
	assert(ALP_I2C_HANDOVER_ALIVE_PERIOD_MS * 1000u * 2u < ALP_I2C_HANDOVER_ALIVE_WINDOW_US);
	/* Fill patterns with state == 0 look taken (nonce == consumed == fill) but need a moving word: */
	{
		static const uint32_t z[] = { 0x00000001u, 0x80000000u, 0x0000FFFFu, 0xA5A5A5A5u };

		for (unsigned i = 0; i < sizeof(z) / sizeof(z[0]); i++) {
			cold(0, z[i], z[i]);
			hb_every_us = 0u;
			assert(alp_i2c_handover_taken(&w) && !sample(true));
		}
	}

	puts("i2c handover protocol ok");
	return 0;
}
