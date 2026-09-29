/* tests/host/test_panel_retry.c -- src/platform/panel_retry.h against a stub
 * panel: the intermittent cold-boot init failure (2026W36-0009, 1 in 2-14 boots
 * left the backlight off because hx8394_init() returned -EIO before it set
 * bl-gpios) must be retried, and a panel that never comes up must be
 * reported with its attempt count. */
#include <assert.h>
#include <stdio.h>

#include "../../src/platform/panel_retry.h"

typedef struct {
	unsigned fail_first;   /* init fails this many times, then succeeds */
	bool     init_sets_bl; /* the driver sets the backlight on success */
	bool     bl_force_works;
	unsigned calls, resets;
	bool     bl;
} stub_t;

static int stub_init(void *ctx, unsigned attempt)
{
	stub_t *s = ctx;

	assert(attempt == s->calls);
	s->resets += attempt > 0;
	if (s->calls++ < s->fail_first) {
		return -5; /* -EIO, as hx8394_init() on a failed DCS write */
	}
	s->bl = s->bl || s->init_sets_bl;
	return 0;
}

static bool stub_bl_on(void *ctx)
{
	return ((stub_t *)ctx)->bl;
}

static int stub_bl_force(void *ctx)
{
	stub_t *s = ctx;

	s->bl = s->bl_force_works;
	return 0;
}

static uint32_t run(stub_t *s, unsigned max)
{
	tr_panel_ops_t ops = { s, stub_init, stub_bl_on, stub_bl_force };

	return tr_panel_bringup(&ops, max);
}

int main(void)
{
	/* Clean first boot. */
	stub_t   s = { 0, true, true, 0, 0, false };
	uint32_t w = run(&s, 4);
	assert(w == (1u | TR_PANEL_OK) && s.bl && s.resets == 0);

	/* Fails twice, then comes up: backlight on, three attempts. */
	s = (stub_t){ 2, true, true, 0, 0, false };
	w = run(&s, 4);
	assert(TR_PANEL_TRIES(w) == 3 && (w & TR_PANEL_OK) && !(w & TR_PANEL_BL_FORCED));
	assert(s.bl && s.resets == 2);

	/* Never comes up: failure, the count, the errno; backlight left off. */
	s = (stub_t){ 99, true, true, 0, 0, false };
	w = run(&s, 4);
	assert(TR_PANEL_TRIES(w) == 4 && !(w & TR_PANEL_OK) && TR_PANEL_ERR(w) == 5);
	assert(!s.bl && s.calls == 4);

	/* Init reports success but the pin is off: forced on. */
	s = (stub_t){ 0, false, true, 0, 0, false };
	w = run(&s, 4);
	assert(w == (1u | TR_PANEL_OK | TR_PANEL_BL_FORCED) && s.bl);

	/* ... and a force that does not take is a failure, not a silent OK. */
	s = (stub_t){ 0, false, false, 0, 0, false };
	w = run(&s, 4);
	assert(!(w & TR_PANEL_OK) && (w & TR_PANEL_BL_FORCED) &&
	       TR_PANEL_ERR(w) == TR_PANEL_ERR_BL_STUCK);

	printf("test_panel_retry: ok\n");
	return 0;
}
