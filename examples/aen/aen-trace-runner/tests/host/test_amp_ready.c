/* tests/host/test_amp_ready.c -- the amp settle + ACK poll (src/audio/tr_amp_ready.c), against a
 * model of the bench carrier: after SD_N rises U27 (0x4D) NACKs at 313 us and first ACKs at
 * 1142 us, U28 (0x4E) ACKs at 390 us (2026-09-30). The SDK's own 200 us settle hits the NACK
 * window (tas2563_init -5); the 2 ms settle + a bounded poll does not. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/audio/tr_amp_ready.h"

static struct {
	uint64_t t;           /* us since SD_N rose */
	uint64_t ack_from[2]; /* per amp: first ACK time; ~0 = never */
	uint32_t reads, nacks;
} a;

static bool a_ack(void *ctx, uint8_t addr)
{
	(void)ctx;
	unsigned i  = addr == 0x4Du ? 0u : 1u;
	bool     ok = a.t >= a.ack_from[i];

	a.t += 100u; /* one 1-byte read at 100 kHz */
	a.reads++;
	a.nacks += !ok;
	return ok;
}

static void a_sleep(uint32_t us)
{
	a.t += us;
}

static uint64_t a_now(void)
{
	return a.t;
}

static const tr_amp_io_t io = { a_ack, a_sleep, a_now, NULL };

/* the bring-up's order: SD_N high, the settle, then the poll per amp */
static bool a_bringup(uint32_t *tries0, uint32_t *tries1)
{
	uint32_t w;

	a_sleep(TR_SND_AMP_SETTLE_US);
	bool ok0 = tr_amp_wait_ack(&io, 0x4Du, TR_SND_AMP_POLL_US, TR_SND_AMP_POLL_MAX_US, tries0, &w);
	bool ok1 =
	    ok0 && tr_amp_wait_ack(&io, 0x4Eu, TR_SND_AMP_POLL_US, TR_SND_AMP_POLL_MAX_US, tries1, &w);

	return ok0 && ok1;
}

int main(void)
{
	uint32_t t0 = 0, t1 = 0, w = 0;

	/* the bench amps as measured: with the settle, both answer the FIRST read -- no NACK window */
	memset(&a, 0, sizeof(a));
	a.ack_from[0] = 1142u, a.ack_from[1] = 390u;
	assert(a_bringup(&t0, &t1) && t0 == 1u && t1 == 1u && a.nacks == 0u);
	assert(TR_SND_AMP_SETTLE_US >= 2000u && TR_SND_AMP_POLL_US == 1000u &&
	       TR_SND_AMP_POLL_MAX_US == 50000u);

	/* an amp slower than measured (3.5 ms): the poll still finds it, a few reads later */
	memset(&a, 0, sizeof(a));
	a.ack_from[0] = 3500u, a.ack_from[1] = 390u;
	assert(a_bringup(&t0, &t1) && t0 >= 2u && t0 <= 3u && t1 == 1u);

	/* an amp that never answers: false after the 50 ms bound, not forever */
	memset(&a, 0, sizeof(a));
	a.ack_from[0] = ~0ull, a.ack_from[1] = 390u;
	a_sleep(TR_SND_AMP_SETTLE_US);
	assert(!tr_amp_wait_ack(&io, 0x4Du, TR_SND_AMP_POLL_US, TR_SND_AMP_POLL_MAX_US, &t0, &w));
	assert(w >= TR_SND_AMP_POLL_MAX_US && w < TR_SND_AMP_POLL_MAX_US + 1200u && t0 >= 40u &&
	       t0 <= 51u);

	/* the old SDK settle (200 us) against the measured U27: the first read NACKs (the bench's -5) */
	memset(&a, 0, sizeof(a));
	a.ack_from[0] = 1142u, a.ack_from[1] = 390u;
	a_sleep(200u);
	assert(!a_ack(NULL, 0x4Du));

	printf("test_amp_ready: ok\n");
	return 0;
}
