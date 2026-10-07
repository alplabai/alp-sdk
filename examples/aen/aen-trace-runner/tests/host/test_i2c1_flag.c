/* tests/host/test_i2c1_flag.c -- src/ipc/tr_i2c1_flag.h: the HP's wait for
 * the HE's "I2C1 free" word: returns as soon as the magic is there, gives up
 * (false) at the limit without it, and a stale/garbage word is not the magic. */
#include <assert.h>
#include <stdio.h>

#include "../../src/ipc/tr_i2c1_flag.h"

typedef struct {
	int64_t  now;
	uint32_t word;
	int64_t  set_at; /* the word becomes the magic at this time; < 0: never */
	int      naps;
} sim_t;

static uint32_t rd(void *c)
{
	sim_t *s = c;

	return s->set_at >= 0 && s->now >= s->set_at ? TR_I2C1_FREE_MAGIC : s->word;
}
static int64_t now_ms(void *c)
{
	return ((sim_t *)c)->now;
}
static void nap(void *c, uint32_t ms)
{
	sim_t *s = c;

	s->now += ms;
	s->naps++;
}

int main(void)
{
	/* Already set: no wait at all. */
	sim_t a = { 0, TR_I2C1_FREE_MAGIC, -1, 0 };
	assert(tr_i2c1_wait_free(rd, now_ms, nap, &a, 5, 1000) && a.naps == 0);

	/* Set 300 ms in: seen within one step of it. */
	sim_t b = { 0, 0, 300, 0 };
	assert(tr_i2c1_wait_free(rd, now_ms, nap, &b, 5, 1000));
	assert(b.now == 300 && b.naps == 60);

	/* Never set (a stale word is not the magic): false at the limit, and the
	 * caller can wait again. */
	sim_t c = { 0, 0x31433248u, -1, 0 };
	assert(!tr_i2c1_wait_free(rd, now_ms, nap, &c, 5, 1000));
	assert(c.now == 1000);
	c.set_at = 1500;
	assert(tr_i2c1_wait_free(rd, now_ms, nap, &c, 5, 1000) && c.now == 1500);

	puts("i2c1 flag ok");
	return 0;
}
