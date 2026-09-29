/* tests/host/test_hp_dbg_stable_read.c -- src/ipc/tr_hp_dbg.h's
 * tr_hp_dbg_read_stable(): the double-read-until-stable retry for a
 * monotonic 64-bit cross-core counter with no seqlock (fix round 7,
 * hp_dbg_t.busy_cyc/total_cyc). A mock read callback stands in for the
 * real volatile memory: it returns a SCRIPTED sequence, so a "torn" read
 * (two consecutive DIFFERENT values, simulating the HP updating the field
 * between the reader's two loads) is reproducible on the host, the same
 * technique test_pslot.c uses for its own torn-read simulation.
 *
 * fix round 11: also tr_hp_dbg_read()/tr_hp_dbg_write_seq_odd()/_even() --
 * the WHOLE-struct seqlock added this round for the *_us timing block and
 * loop_hz_x10 (neither had ANY protection before; the silicon finding was
 * "one beacon read per boot was torn, all timings 0"). No mock needed
 * here: a plain hp_dbg_t on the stack plus a no-op barrier is enough to
 * drive the real odd/even protocol directly, the same pattern
 * tests/host/test_pslot.c uses for tr_pslot_read()/tr_pslot_write(). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_hp_dbg.h"

static void noop_barrier(void) { }

/* fix round 12: a barrier that mutates the target's own seq on its FIRST
 * call (used by test 8 below to simulate a write starting mid-read) --
 * file-scope statics, the same pattern g_script/g_i already use above for
 * mock_read, not a nested function (not portable C11). */
static volatile hp_dbg_t *g_race_target;
static int                 g_race_calls;

static void race_barrier(void)
{
	g_race_calls++;
	if (g_race_calls == 1) {
		g_race_target->seq = g_race_target->seq + 1u; /* even -> odd, mid-read */
	}
}

static const uint64_t *g_script;
static int             g_i;

static uint64_t mock_read(void *ctx)
{
	(void)ctx;
	return g_script[g_i++];
}

int main(void)
{
	/* 1. Never torn: every read returns the same value -- one call in,
	 * the very first read already matches the second, done in 2 reads. */
	{
		static const uint64_t stable[] = { 42u, 42u, 42u, 42u };

		g_script = stable;
		g_i      = 0;
		uint64_t v = tr_hp_dbg_read_stable(mock_read, NULL);

		assert(v == 42u);
		assert(g_i == 2); /* exactly two reads for the un-torn case */
	}

	/* 2. Torn once, then stable: the value changed between the writer's
	 * update and the reader's second look (a real tear, or just the
	 * counter genuinely advancing between the two reads) -- must NOT
	 * accept the mismatched pair, must retry until two agree. */
	{
		static const uint64_t torn_once[] = { 100u, 101u, 200u, 200u };

		g_script = torn_once;
		g_i      = 0;
		uint64_t v = tr_hp_dbg_read_stable(mock_read, NULL);

		assert(v == 200u); /* the first STABLE pair, not the torn 100/101 */
		assert(g_i == 4);
	}

	/* 3. Torn twice in a row before settling: proves this is a genuine
	 * retry loop, not just "try twice and give up". */
	{
		static const uint64_t torn_twice[] = { 1u, 2u, 3u, 4u, 5u, 5u };

		g_script = torn_twice;
		g_i      = 0;
		uint64_t v = tr_hp_dbg_read_stable(mock_read, NULL);

		assert(v == 5u);
		assert(g_i == 6);
	}

	/* 4. A real 64-bit value near the reported silicon numbers (fix round
	 * 7's own finding, 787,127,318 / 790,822,175) round-trips exactly
	 * when stable -- no truncation from the uint64_t callback shape. */
	{
		static const uint64_t big[] = { 790822175ull, 790822175ull };

		g_script = big;
		g_i      = 0;
		assert(tr_hp_dbg_read_stable(mock_read, NULL) == 790822175ull);
	}

	/* 5. tr_hp_dbg_read(): a clean, even seq -- one read, accepted, exact
	 * field values round-trip. */
	{
		volatile hp_dbg_t s;
		hp_dbg_t           out;

		memset((void *)&s, 0, sizeof(s));
		s.magic       = TR_HP_DBG_MAGIC;
		s.capture_us  = 111u;
		s.loop_hz_x10 = 251u;
		s.seq         = 8u; /* even: no write in progress */
		assert(tr_hp_dbg_read(&s, &out, noop_barrier));
		assert(out.magic == TR_HP_DBG_MAGIC && out.capture_us == 111u && out.loop_hz_x10 == 251u);
	}

	/* 6. Odd seq (a write left mid-publish, or genuinely in progress):
	 * rejected on sight, `out` untouched by the read itself (still whatever
	 * the caller had before the call -- not part of the contract, but the
	 * rejection itself is). */
	{
		volatile hp_dbg_t s;
		hp_dbg_t           out;

		memset((void *)&s, 0, sizeof(s));
		s.seq = 7u; /* odd */
		assert(!tr_hp_dbg_read(&s, &out, noop_barrier));
	}

	/* 7. The writer's own odd/even bump, driven directly: seq starts even,
	 * goes odd across the "write", ends even again -- and a read taken
	 * AFTER the full odd/even cycle sees the new values, matching
	 * tr_pslot_write()'s own protocol. */
	{
		volatile hp_dbg_t s;
		hp_dbg_t           out;

		memset((void *)&s, 0, sizeof(s));
		s.seq = 4u;
		tr_hp_dbg_write_seq_odd(&s, noop_barrier);
		assert(s.seq == 5u);
		s.loop_hz_x10 = 266u;
		s.capture_us  = 222u;
		tr_hp_dbg_write_seq_even(&s, noop_barrier);
		assert(s.seq == 6u);
		assert(tr_hp_dbg_read(&s, &out, noop_barrier));
		assert(out.loop_hz_x10 == 266u && out.capture_us == 222u);
	}

	/* 8. fix round 12 (review: a mutant that turns tr_hp_dbg_read()'s own
	 * final `return s->seq == s0;` into `return true;` survived every
	 * test above -- none of them exercise the SECOND check, only the
	 * odd-at-entry short-circuit). Genuine mid-copy torn read: a barrier
	 * callback that, on its FIRST call (the one BETWEEN reading s0 and
	 * the struct copy), bumps seq itself -- simulating a write starting
	 * right as the read begins. The struct copy then reads a mix of old
	 * and in-flight bytes (irrelevant here, out's own field values are
	 * not asserted), but the exact bug this proves the REAL check catches:
	 * s->seq (now odd, changed) no longer equals s0 (the even value read
	 * at entry) -- a `return true;` mutant would accept this read; the
	 * real function must reject it. */
	{
		volatile hp_dbg_t s;
		hp_dbg_t           out;

		memset((void *)&s, 0, sizeof(s));
		s.seq          = 8u; /* even */
		s.loop_hz_x10  = 251u;
		g_race_target  = &s;
		g_race_calls   = 0;

		assert(!tr_hp_dbg_read(&s, &out, race_barrier));
		assert(g_race_calls == 2); /* both barriers ran -- the copy itself was not skipped */
	}

	printf("PASS: tests/host/test_hp_dbg_stable_read.c\n");
	return 0;
}
