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

	puts("i2c handover protocol ok");
	return 0;
}
