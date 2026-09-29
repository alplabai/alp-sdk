/* tests/host/test_wd.c -- the HE's A32 watchdog (src/ipc/tr_wd.c) against a
 * model stub + renderer: slow renderer init, a stub that is late or never
 * alive, a genuine mid-game stall, a renderer that never comes up, the
 * boot race that leaves a stale LAUNCH under a self-LAUNCHed renderer, a
 * foreign renderer at boot, a HALT left pending by a previous HE boot, and a
 * renderer fault mid-game. The stub parks PARK_GAP after the renderer
 * consumes a HALT -- longer than one poll, as on silicon. The
 * HE side mirrors src/platform/a32.c: a boot loop polling every 1 ms (bounded,
 * ends on a command or an adopted RUNNING renderer), then frames -- a landed
 * frame every refresh, a miss every TR_OUT_TIMEOUT_MS followed by a poll. */
#include <assert.h>
#include <stdio.h>

#include "../../src/ipc/tr_wd.h"

#define MS             1000u
#define OUT_TIMEOUT_US (100u * MS) /* a32.c TR_OUT_TIMEOUT_MS */
#define FRAME_US       (33u * MS)
#define BOOT_WAIT_US   (3000u * MS) /* a32.c TR_STUB_BOOT_WAIT_MS */
#define NEVER          UINT64_MAX
#define PARK_GAP       (150u * MS) /* HALT consumed -> PARKED (core 1 wait + set/way pass) */

typedef struct {
	/* scenario */
	uint64_t alive_at;    /* the stub initialises the mailbox (PARKED) */
	int      release;     /* self-LAUNCH self_after_us after that (MRAM copy) */
	uint64_t self_after_us;
	uint64_t init_us;     /* LAUNCH -> first frame; HALT is honoured only after it */
	uint64_t stall_at;    /* the renderer stops publishing (still honours HALT) */
	uint64_t fault_until; /* every LAUNCH before this faults (FAULT, like a refused CRC) */
	uint64_t recover_at;  /* a LAUNCH at or after this clears stall_at */
	uint64_t fault_at;    /* the running renderer faults (FAULT, ctrl_cmd cleared) */
	int      ours;        /* ctrl_entry/len/crc are the HE's image (our LAUNCH makes them so) */
	/* state */
	int      alive, self_pending;
	uint32_t state, ctrl;
	uint64_t parked_at, launched_at, halt_consumed_at;
	/* HE side */
	tr_wd_t  wd;
	uint64_t t;
	uint32_t halts, launches, frames, stale_launch_overwritten;
	uint32_t last_cmd;
	tr_wd_why_t last_halt_why, last_launch_why;
	uint64_t last_launch_at, max_launch_gap;
	uint64_t parked_by_halt_at; /* our HALT parked it here; NEVER when not parked by us */
	uint64_t max_halted_us;     /* longest a HALT-parked renderer waited for its LAUNCH */
} sim_t;

static void do_launch(sim_t *s)
{
	if (s->t < s->fault_until) {
		s->state = TR_STUB_FAULT;
		return;
	}
	s->state       = TR_STUB_RUNNING;
	s->launched_at = s->t;
	if (s->fault_at <= s->t)
		s->fault_at = NEVER; /* one fault */
	if (s->recover_at != 0 && s->t >= s->recover_at && s->t >= s->stall_at)
		s->stall_at = NEVER;
}

static void stub_step(sim_t *s)
{
	if (!s->alive) {
		if (s->t < s->alive_at)
			return;
		s->alive = 1;
		s->state = TR_STUB_PARKED;
		s->ctrl  = TR_CTRL_NONE; /* stub_main drops a leftover command */
		s->parked_at    = s->t;
		s->self_pending = s->release;
	}
	if (s->self_pending) {
		if (s->t < s->parked_at + s->self_after_us)
			return; /* copying: ctrl_cmd not looked at */
		s->self_pending = 0;
		do_launch(s);
		return;
	}
	if (s->state != TR_STUB_RUNNING && s->ctrl != TR_CTRL_NONE) { /* park0 */
		uint32_t c = s->ctrl;

		s->ctrl = TR_CTRL_NONE;
		if (c == TR_CTRL_LAUNCH)
			do_launch(s);
		return;
	}
	if (s->state == TR_STUB_RUNNING && s->t >= s->fault_at) {
		s->state    = TR_STUB_FAULT; /* stub_fault: HALT to the sibling, which the re-entry clears */
		s->ctrl     = TR_CTRL_NONE;
		s->fault_at = NEVER;
		return;
	}
	if (s->halt_consumed_at != NEVER) {
		if (s->t >= s->halt_consumed_at + PARK_GAP) {
			s->state            = TR_STUB_PARKED;
			s->halt_consumed_at = NEVER;
		}
		return;
	}
	if (s->state == TR_STUB_RUNNING && s->ctrl == TR_CTRL_HALT && s->t >= s->launched_at + s->init_us) {
		s->ctrl             = TR_CTRL_NONE; /* the renderer's main loop consumes HALT, returns */
		s->halt_consumed_at = s->t;
	}
}

static int frame_ready(const sim_t *s)
{
	return s->state == TR_STUB_RUNNING && s->halt_consumed_at == NEVER && s->t >= s->launched_at + s->init_us &&
	       s->t < s->stall_at && s->t < s->fault_at;
}

static void he_poll(sim_t *s, int missed)
{
	tr_wd_why_t why;
	uint32_t    cmd = tr_wd_poll(&s->wd, s->alive, s->alive ? s->state : 0xE2B8D896u, s->alive ? s->ctrl : 0x1234u,
				     s->ours, missed, s->t, &why);

	assert(why < TR_WD_WHY_N && tr_wd_why_str(why)[0] != '\0');
	if (cmd == TR_CTRL_NONE)
		return;
	assert(s->alive); /* never poke a stub that is not there */
	if (s->ctrl == TR_CTRL_LAUNCH && cmd == TR_CTRL_HALT)
		s->stale_launch_overwritten++;
	else
		assert(s->ctrl == TR_CTRL_NONE); /* never stacked */
	s->ctrl     = cmd;
	s->last_cmd = cmd;
	if (cmd == TR_CTRL_HALT) {
		assert(why == TR_WD_HALT_FIRST || why == TR_WD_HALT_STALL || why == TR_WD_HALT_FOREIGN);
		s->halts++;
		s->last_halt_why = why;
	} else {
		assert(cmd == TR_CTRL_LAUNCH);
		assert(why >= TR_WD_LAUNCH_BOOT && why <= TR_WD_LAUNCH_PARKED);
		if (s->launches != 0 && s->t - s->last_launch_at > s->max_launch_gap)
			s->max_launch_gap = s->t - s->last_launch_at;
		s->last_launch_at = s->t;
		s->launches++;
		s->last_launch_why = why;
		s->ours            = 1; /* a32.c writes tr_a32_autolaunch_id with every LAUNCH */
	}
}

static void he_boot(sim_t *s)
{
	tr_wd_init(&s->wd, s->t);
	for (uint64_t end = s->t + BOOT_WAIT_US; s->t < end; s->t += MS) {
		stub_step(s);
		uint32_t before = s->launches + s->halts;

		he_poll(s, 0);
		if (s->launches + s->halts != before || (s->alive && s->state == TR_STUB_RUNNING))
			break;
	}
}

/* Run the game loop until `until`. */
static void he_run(sim_t *s, uint64_t until)
{
	while (s->t < until) {
		stub_step(s);
		/* "never left HALTed": time from our HALT parking it to the next LAUNCH */
		if (s->last_cmd == TR_CTRL_HALT && s->state == TR_STUB_PARKED && s->ctrl == TR_CTRL_NONE) {
			if (s->parked_by_halt_at == NEVER)
				s->parked_by_halt_at = s->t;
			if (s->t - s->parked_by_halt_at > s->max_halted_us)
				s->max_halted_us = s->t - s->parked_by_halt_at;
		} else {
			s->parked_by_halt_at = NEVER;
		}
		if (frame_ready(s)) {
			s->t += FRAME_US;
			(void)tr_wd_landed(&s->wd, s->t);
			s->frames++;
		} else {
			s->t += OUT_TIMEOUT_US;
			stub_step(s);
			he_poll(s, 1);
		}
	}
}

static sim_t sim(uint64_t alive_at, int release, uint64_t init_us)
{
	return (sim_t){ .alive_at = alive_at, .release = release, .self_after_us = 20u * MS, .init_us = init_us,
			.stall_at = NEVER, .fault_at = NEVER, .ours = 1, .halt_consumed_at = NEVER,
			.parked_by_halt_at = NEVER };
}

int main(void)
{
	/* Pure rules. */
	{
		tr_wd_t     w;
		tr_wd_why_t why;

		tr_wd_init(&w, 0);
		/* Garbage stub_state (cold SRAM1): nothing, whatever the time. */
		assert(tr_wd_poll(&w, true, 0xE2B8D896u, 0, true, true, 10000000u, &why) == TR_CTRL_NONE && why == TR_WD_NOT_ALIVE);
		assert(tr_wd_poll(&w, false, TR_STUB_PARKED, 0, true, true, 10000000u, &why) == TR_CTRL_NONE && why == TR_WD_NOT_ALIVE);
		/* First seen alive at 10 s: the settle and first-frame clocks start there. */
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, 0, true, false, 10000000u, &why) == TR_CTRL_NONE && why == TR_WD_SETTLING);
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, 0, true, true, 10000000u + TR_WD_SETTLE_US - 1u, &why) == TR_CTRL_NONE);
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, 0, true, true, 10000000u + TR_WD_SETTLE_US, &why) == TR_CTRL_LAUNCH &&
		       why == TR_WD_LAUNCH_BOOT);
		/* Unconsumed LAUNCH under a parked stub: not stacked. */
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, TR_CTRL_LAUNCH, true, true, 11000000u, &why) == TR_CTRL_NONE &&
		       why == TR_WD_PENDING);
		/* Running, first frame not due: a 100 ms miss is not a stall. */
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, true, 11000000u, &why) == TR_CTRL_NONE &&
		       why == TR_WD_INITIALISING);
		uint64_t dl = tr_wd_deadline_us(&w);

		assert(dl == 10000000u + TR_WD_SETTLE_US + TR_WD_FIRST_FRAME_US);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, true, dl, &why) == TR_CTRL_HALT && why == TR_WD_HALT_FIRST);
		/* Our HALT parked it: LAUNCH at once (a recovery never ends HALTed). */
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, 0, true, true, dl + 1u, &why) == TR_CTRL_LAUNCH &&
		       why == TR_WD_LAUNCH_HALTED);
		/* Second failed LAUNCH: the first-frame timeout doubled. */
		assert(tr_wd_deadline_us(&w) == dl + 1u + 2u * TR_WD_FIRST_FRAME_US);
		/* A frame lands: first-frame phase over, then a miss is a stall. */
		assert(tr_wd_landed(&w, dl + 100000u) && !tr_wd_landed(&w, dl + 133000u));
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, false, dl + 200000u, &why) == TR_CTRL_NONE && why == TR_WD_OK);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, true, dl + 233000u, &why) == TR_CTRL_HALT &&
		       why == TR_WD_HALT_STALL);
		/* A stale LAUNCH under a RUNNING renderer does not block the HALT. */
		tr_wd_init(&w, 0);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, TR_CTRL_LAUNCH, true, false, 0, &why) == TR_CTRL_NONE);
		(void)tr_wd_landed(&w, 50000u);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, TR_CTRL_LAUNCH, true, true, 150000u, &why) == TR_CTRL_HALT);
		/* An unconsumed HALT is not re-sent. */
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, TR_CTRL_HALT, true, true, 250000u, &why) == TR_CTRL_NONE &&
		       why == TR_WD_PENDING);
	}

	/* 1. The silicon cold boot of 2026-09-24: the HE up ~150 ms before the
	 * stub, a release self-LAUNCH, a renderer whose LAUNCH -> first frame is
	 * 350 ms (> the 100 ms stall watchdog). No command at all; it runs. */
	{
		sim_t s = sim(150u * MS, 1, 350u * MS);

		he_boot(&s);
		he_run(&s, 10000000u);
		assert(s.halts == 0 && s.launches == 0);
		assert(s.frames > 250u && s.state == TR_STUB_RUNNING);
	}

	/* 2. Stub alive only after the boot wait (5 s), dev stub (no self-LAUNCH):
	 * nothing while it is absent, then one LAUNCH after the settle. */
	{
		sim_t s = sim(5000u * MS, 0, 120u * MS);

		he_boot(&s);
		assert(s.launches == 0);
		he_run(&s, 15000000u);
		assert(s.launches == 1 && s.halts == 0 && s.frames > 200u);
		/* A stub that never comes up: never a command. */
		sim_t n = sim(NEVER, 1, 120u * MS);

		he_boot(&n);
		he_run(&n, 120000000u);
		assert(n.launches == 0 && n.halts == 0 && n.frames == 0);
	}

	/* 3. Genuine stall mid-game at 20 s: one HALT (stall), one LAUNCH, back. */
	{
		sim_t s = sim(0, 1, 350u * MS);

		s.stall_at   = 20000000u;
		s.recover_at = 1; /* the relaunched renderer is healthy */
		he_boot(&s);
		he_run(&s, 30000000u);
		assert(s.halts == 1 && s.launches == 1);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s));
		assert(s.max_halted_us <= OUT_TIMEOUT_US);
	}

	/* 4. A renderer that never produces a frame (but honours HALT) for 10 min,
	 * then a LAUNCH that works: unlimited attempts (well past the old 8),
	 * spaced by the capped backoff, and never left HALTed. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		s.stall_at   = 0; /* no frame at all */
		s.recover_at = 600000000u;
		he_boot(&s);
		he_run(&s, 700000000u);
		assert(s.halts > 8u && s.launches > 8u);
		assert(s.max_launch_gap <= (TR_WD_FIRST_FRAME_US << TR_WD_BACKOFF_MAX_SHIFT) + 2u * OUT_TIMEOUT_US);
		assert(s.max_launch_gap >= (TR_WD_FIRST_FRAME_US << TR_WD_BACKOFF_MAX_SHIFT));
		assert(s.max_halted_us <= OUT_TIMEOUT_US);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s) && s.frames > 1000u);
	}

	/* 5. LAUNCHes refused (FAULT) for 30 s: relaunched with backoff, then runs. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		s.fault_until = 30000000u;
		he_boot(&s);
		he_run(&s, 90000000u);
		assert(s.launches >= 3u && s.launches < 10u && s.halts == 0);
		assert(s.state == TR_STUB_RUNNING && s.frames > 1000u);
	}

	/* 6. Boot race: the stub is PARKED for 800 ms before its self-LAUNCH
	 * (longer than the settle): the HE's LAUNCH lands during the copy and is
	 * left under the RUNNING renderer. A later stall still HALTs it. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		s.self_after_us = 800u * MS;
		s.stall_at      = 20000000u;
		s.recover_at    = 1;
		he_boot(&s);
		assert(s.launches == 1 && s.ctrl == TR_CTRL_LAUNCH);
		he_run(&s, 30000000u);
		assert(s.stale_launch_overwritten == 1 && s.halts == 1);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s));
	}

	/* 7. Backoff reset: failed LAUNCHes grow the first-frame timeout; 60 s
	 * of landed frames bring it back to TR_WD_FIRST_FRAME_US. */
	{
		tr_wd_t     w;
		tr_wd_why_t why;
		uint64_t    t = 0;

		tr_wd_init(&w, 0);
		for (int i = 0; i < 3; i++) { /* three LAUNCHes, no frame */
			t += 10000000u;
			assert(tr_wd_poll(&w, true, TR_STUB_FAULT, 0, true, true, t, &why) == TR_CTRL_LAUNCH);
		}
		assert(tr_wd_deadline_us(&w) == t + 4u * TR_WD_FIRST_FRAME_US);
		for (uint64_t f = t + 100000u; f <= t + 100000u + TR_WD_WINDOW_US - 33000u; f += 33000u)
			(void)tr_wd_landed(&w, f);
		assert(w.tries == 3u); /* not a full window yet */
		(void)tr_wd_landed(&w, t + 100000u + TR_WD_WINDOW_US);
		assert(w.tries == 0u);
		t += 100000u + TR_WD_WINDOW_US + 1000000u;
		assert(tr_wd_poll(&w, true, TR_STUB_FAULT, 0, true, true, t, &why) == TR_CTRL_LAUNCH);
		assert(tr_wd_deadline_us(&w) == t + TR_WD_FIRST_FRAME_US);
	}

	/* 8. FAULT when the stub is first seen (a refused self-LAUNCH): LAUNCH
	 * at once, no settle. */
	{
		tr_wd_t     w;
		tr_wd_why_t why;

		tr_wd_init(&w, 0);
		assert(tr_wd_poll(&w, true, TR_STUB_FAULT, 0, true, false, 700000u, &why) == TR_CTRL_LAUNCH &&
		       why == TR_WD_LAUNCH_FAULT);
	}

	/* 9. The renderer faults mid-game (20 s): relaunched at the next poll,
	 * no HALT (it is already parked), and it runs again. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		s.fault_at = 20000000u;
		he_boot(&s);
		he_run(&s, 30000000u);
		assert(s.halts == 0 && s.launches == 1 && s.last_launch_why == TR_WD_LAUNCH_FAULT);
		assert(s.last_launch_at <= 20000000u + 2u * OUT_TIMEOUT_US);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s));
	}

	/* 10. A HALT left pending by the previous HE boot (the HE reset between
	 * writing it and the renderer consuming it): not stacked on, waited out,
	 * then the parked stub is LAUNCHed. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		/* LAUNCHed just before the HE reset: it reads ctrl_cmd only from its
		 * main loop, 2.3 s after this HE boots -- past the first-frame
		 * timeout, so the pending HALT must not be stacked on then either. */
		s.t     = 5000000u;
		s.alive = 1, s.state = TR_STUB_RUNNING, s.ctrl = TR_CTRL_HALT;
		s.launched_at = s.t + 2300000u - s.init_us;
		he_boot(&s);
		he_run(&s, 15000000u);
		assert(s.halts == 0 && s.launches == 1);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s) && s.frames > 200u);
	}

	/* 11. A foreign renderer RUNNING at boot (ctrl_entry/len/crc not ours):
	 * HALT, then our LAUNCH -- not adopted. */
	{
		sim_t s = sim(0, 1, 200u * MS);

		s.ours = 0;
		he_boot(&s);
		assert(s.halts == 1 && s.last_halt_why == TR_WD_HALT_FOREIGN);
		he_run(&s, 10000000u);
		assert(s.halts == 1 && s.launches == 1 && s.last_launch_why == TR_WD_LAUNCH_HALTED);
		assert(s.state == TR_STUB_RUNNING && frame_ready(&s));
	}

	/* 12. The HALT -> PARKED gap and the pending clock: after our HALT is
	 * consumed the stub is still RUNNING -- "waiting to park", no second
	 * HALT; a HALT never consumed is timed. */
	{
		tr_wd_t     w;
		tr_wd_why_t why;

		tr_wd_init(&w, 0);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, false, 0, &why) == TR_CTRL_NONE);
		(void)tr_wd_landed(&w, 50000u);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, true, 150000u, &why) == TR_CTRL_HALT);
		assert(tr_wd_pending_us(&w, 150000u) == 0u);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, TR_CTRL_HALT, true, true, 250000u, &why) == TR_CTRL_NONE &&
		       why == TR_WD_PENDING);
		assert(tr_wd_poll(&w, true, TR_STUB_RUNNING, 0, true, true, 350000u, &why) == TR_CTRL_NONE &&
		       why == TR_WD_PARKING);
		assert(tr_wd_pending_us(&w, 1250000u) == 1000000u); /* one clock from the first pending poll */
		assert(tr_wd_poll(&w, true, TR_STUB_PARKED, 0, true, true, 450000u, &why) == TR_CTRL_LAUNCH);
		assert(tr_wd_pending_us(&w, 450000u) == 0u);
	}

	printf("test_wd: ok\n");
	return 0;
}
