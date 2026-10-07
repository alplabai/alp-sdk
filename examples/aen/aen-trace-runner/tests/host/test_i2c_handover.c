/* tests/host/test_i2c_handover.c -- the flag-word protocol of the SDK's
 * alp,i2c-handover glue (zephyr/soc-bridge/alif/i2c_handover.h): a release is
 * taken exactly once, a stale or already-taken magic never satisfies a wait,
 * a DIRTY release is reported as such, and the release side's per-boot nonce
 * can never collide with the one the acquiring core already consumed. */
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

	/* A stale magic whose release was already taken (same nonce as consumed) is
	 * ignored -- SRAM0 survives a warm reset, a leftover must not count. */
	cold(ALP_I2C_HANDOVER_CLEAN, 0x3333u, 0x3333u);
	assert(alp_i2c_handover_try_acquire(&w) == ALP_I2C_HANDOVER_NOT_YET);
	assert(w.state == ALP_I2C_HANDOVER_CLEAN); /* untouched */

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
