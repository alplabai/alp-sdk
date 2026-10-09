/* tests/host/test_reset_guard.c -- the loop guard of the HE's last-resort SoC reset
 * (src/ipc/tr_reset_guard.h): two quick fatal errors reset, the third in a row halts; a fatal error
 * after a healthy boot starts over; cold-SRAM garbage never halts the first fault. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_reset_guard.h"

static tr_reset_guard_t g;

int main(void)
{
	/* Cold SRAM: nothing recorded, garbage of every flavour -> the first fault resets. */
	static const uint8_t fill[] = { 0x00, 0xFF, 0xA5, 0x5A };

	for (unsigned i = 0; i < sizeof(fill); i++) {
		memset((void *)&g, fill[i], sizeof(g));
		assert(tr_reset_guard_allow(&g, 5000u));
		assert(g.count == 1u);
	}
	/* The magic is ours but the count is garbage: treated as no streak. */
	g.magic = TR_RESET_GUARD_MAGIC;
	g.count = 0xFFFFFFFFu;
	assert(tr_reset_guard_allow(&g, 5000u) && g.count == 1u);

	/* A fault that repeats right after every boot: reset, reset, then halt (and keep halting). */
	memset((void *)&g, 0, sizeof(g));
	assert(tr_reset_guard_allow(&g, 4000u));  /* 1st */
	assert(tr_reset_guard_allow(&g, 4000u));  /* 2nd */
	assert(!tr_reset_guard_allow(&g, 4000u)); /* 3rd: halt */
	assert(!tr_reset_guard_allow(&g, 4000u)); /* never resets again while it keeps happening */

	/* A fatal error that comes after the window ends the streak, even a halted one. */
	assert(tr_reset_guard_allow(&g, TR_RESET_GUARD_WINDOW_MS) && g.count == 1u);
	assert(tr_reset_guard_allow(&g, TR_RESET_GUARD_WINDOW_MS - 1u) && g.count == 2u);
	assert(!tr_reset_guard_allow(&g, 1u) && g.count == 3u);

	/* Realistic cold patterns. A count without our magic is not a streak (the magic check): */
	g.magic = 0u;
	g.count = 2u;
	assert(tr_reset_guard_allow(&g, 5000u) && g.count == 1u && g.magic == TR_RESET_GUARD_MAGIC);
	g.magic = 0u;
	g.count = 3u;
	assert(tr_reset_guard_allow(&g, 5000u) && g.count == 1u);
	/* power-up SRAM that is all-ones / 0xA5 / a stray magic-like value with a wild count: */
	g.magic = 0xFFFFFFFFu;
	g.count = 0xFFFFFFFFu;
	assert(tr_reset_guard_allow(&g, 5000u) && g.count == 1u);
	g.magic = TR_RESET_GUARD_MAGIC ^ 1u;
	g.count = 2u;
	assert(tr_reset_guard_allow(&g, 5000u) && g.count == 1u);
	/* our magic with a streak of 2 and a quick fault: the third halts; a streak of 3 stays halted */
	g.magic = TR_RESET_GUARD_MAGIC;
	g.count = 2u;
	assert(!tr_reset_guard_allow(&g, 100u) && g.count == 3u);
	g.count = TR_RESET_GUARD_MAX;
	assert(!tr_reset_guard_allow(&g, 100u) && g.count == TR_RESET_GUARD_MAX);
	/* one count past the limit is garbage, not a halted streak */
	g.count = TR_RESET_GUARD_MAX + 1u;
	assert(tr_reset_guard_allow(&g, 100u) && g.count == 1u);

	puts("test_reset_guard: OK");
	return 0;
}
