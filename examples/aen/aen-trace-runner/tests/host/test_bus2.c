/* tests/host/test_bus2.c -- the I2C2 + GPIO5 lease between the two M55 cores (src/ipc/tr_bus2.c),
 * both cores simulated in one thread, one step at a time:
 *   - the two-way handoff: offer -> claim -> return -> the HE owns the bus again, and a second
 *     lease later (a runtime amp write) with a fresh token;
 *   - the bus is never driven by both cores at once (a model of the two NVIC lines);
 *   - a stale offer left by a dead HE (SRAM0 survives warm resets) is never accepted;
 *   - a stale RETURN (a late write of a previous session) never gives the bus back;
 *   - an HP that restarted while holding the lease: the HE takes the bus back by itself;
 *   - the HE restarting inside a bring-up step: the HP aborts before its next access and the HE
 *     waits (bounded) for a step already running; the Dekker order is asserted;
 *   - an unclaimed offer is withdrawn after a timeout, and a claim that races the withdrawal wins;
 *   - cold SRAM0 garbage is "none", never a state;
 *   - the HP's I2C2 IRQ is on only inside a bus step: an HE-only reset while the HP holds the lease
 *     between steps cannot meet an armed HP ISR;
 *   - an HP that dies holding the lease (between steps, or inside a bus step) is reclaimed after
 *     TR_BUS2_HP_DEAD_MS without a beat -- and never while it beats;
 *   - a stale BUS left by a dead HP across a warm HE reset is a warning, not a failure;
 *   - a state word carries its token: one load decides, and a wrong token never claims. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_bus2.h"

#define TAG_OWNS tr_bus2_word(TR_BUS2_HE_OWNS, 0u)

static volatile tr_bus2_t rec;
static int64_t            clk;
static int                he_irq, hp_irq; /* each core's I2C2 NVIC line */
static unsigned           gives, takes, reclaims, sleeps;
static void (*barrier_hook)(void);
static unsigned barrier_skip; /* barrier_hook fires on the barrier after this many others */
static void (*sleep_hook)(void);
static bool sleep_beats; /* a live HP: hp_beat moves on every HE wait */

static tr_bus2_he_t he;
static tr_bus2_hp_t hp;
static uint32_t     nonce_seq = 0x1000u;

static void check_exclusive(void)
{
	assert(!(he_irq && hp_irq)); /* the shared controller has one holder at a time */
}

static void b_barrier(void)
{
	if (barrier_hook) {
		if (barrier_skip != 0u) {
			barrier_skip--;
			return;
		}
		void (*h)(void) = barrier_hook;

		barrier_hook = NULL;
		h();
	}
}

static void b_give(void *c)
{
	(void)c;
	he_irq = 0;
	gives++;
}

static void b_take(void *c)
{
	(void)c;
	check_exclusive();
	he_irq = 1;
	takes++;
	check_exclusive();
}

static void b_reclaim(void *c)
{
	(void)c;
	he_irq = 0; /* stopped, bus cleared */
	reclaims++;
}

static int64_t b_now(void *c)
{
	(void)c;
	return clk;
}

static void b_sleep(void *c, uint32_t ms)
{
	(void)c;
	clk += ms;
	sleeps++;
	if (sleep_beats) {
		rec.hp_beat++;
	}
	if (sleep_hook) {
		void (*h)(void) = sleep_hook;

		sleep_hook = NULL;
		h();
	}
}

static const tr_bus2_ops_t ops = { b_barrier, b_give, b_take, b_reclaim, b_now, b_sleep, NULL };

static void he_tick(void)
{
	clk += 33;
	tr_bus2_he_tick(&he, &rec, &ops, nonce_seq += 0x10u);
	check_exclusive();
}

static void hp_arm(void) /* the HP's i2c2: device_init / i2c_configure + irq_enable */
{
	hp_irq = 1;
	check_exclusive();
}

static void hp_disarm(void)
{
	hp_irq = 0;
}

/* sound/src/main.c tr_snd_bus_enter() / _leave(): the IRQ line is on only after the entry passed. */
static bool hp_step_enter(void)
{
	if (!tr_bus2_hp_enter(&hp, &rec, b_barrier)) {
		return false;
	}
	hp_arm();
	return true;
}

static void hp_step_leave(void)
{
	hp_disarm();
	tr_bus2_hp_leave(&hp, &rec, b_barrier);
}

/* Both cores just booted: the HE first (its drivers up: line on), then the HP. */
static void cold_start(void)
{
	memset((void *)&rec, 0, sizeof(rec));
	he_irq = 1;
	hp_irq = 0;
	gives = takes = reclaims = sleeps = 0;
	clk                               = 100000;
	barrier_hook                      = NULL;
	barrier_skip                      = 0u;
	sleep_hook                        = NULL;
	sleep_beats                       = false;
	memset(&he, 0, sizeof(he));
	memset(&hp, 0, sizeof(hp));
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS));
	tr_bus2_hp_boot(&rec, b_barrier);
}

/* The HP asks, then polls (every 20 ms) while the HE ticks (every ~33 ms), until it holds an
 * offer. Returns the polls used, 0 if none came within `max`. */
static int hp_get_offer(int max)
{
	tr_bus2_hp_want(&hp, &rec, b_barrier);
	for (int i = 1; i <= max; i++) {
		clk += TR_BUS2_HP_POLL_MS;
		if (tr_bus2_hp_poll(&hp, &rec)) {
			return i;
		}
		if (i % 2 == 0) {
			he_tick();
		}
	}
	return 0;
}

/* One full lease by the HP: enter (bus step), a no-bus step, a bus step, return. */
static void hp_full_lease(void)
{
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	hp_step_leave();
	he_tick(); /* the HE sees the claim and stays off the bus */
	assert(!tr_bus2_he_owns_bus(&he) && he.st == TR_BUS2_ST_CLAIMED && !he_irq && !hp_irq);
	assert(hp_step_enter());
	hp_disarm();
	tr_bus2_hp_return(&hp, &rec, b_barrier);
	check_exclusive();
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
}

static void test_two_way(void)
{
	cold_start();
	for (int i = 0; i < 20; i++) { /* nobody wants it: the HE keeps the bus, no offer ever */
		he_tick();
		assert(tr_bus2_he_owns_bus(&he) && he_irq && gives == 0);
	}
	hp_full_lease();
	assert(gives == 1 && takes == 1 && rec.he_regains == 1 && rec.hp_acq == 1 &&
	       rec.hp_aborts == 0 && rec.he_reclaims == 0);
	for (int i = 0; i < 20; i++) { /* returned: no new offer to an HP that no longer asks */
		he_tick();
		assert(tr_bus2_he_owns_bus(&he) && gives == 1);
	}
	/* a runtime amp write later goes through the same handshake, with a fresh token */
	uint32_t t1 = rec.he_token;

	hp_full_lease();
	assert(gives == 2 && takes == 2 && rec.hp_acq == 2 && rec.he_token != t1 && rec.he_token != 0u);
}

static void test_offer_waits_for_a_live_he(void)
{
	cold_start();
	tr_bus2_hp_want(&hp, &rec, b_barrier);
	/* an offer with an unmoved he_beat = a dead HE's leftover */
	rec.he_state = tr_bus2_word(TR_BUS2_HE_OFFER, 0xDEAD0001u);
	rec.he_token = 0xDEAD0001u;
	for (int i = 0; i < 500; i++) {
		assert(!tr_bus2_hp_poll(&hp, &rec));
	}
	rec.he_beat++; /* an HE that is alive now */
	assert(tr_bus2_hp_poll(&hp, &rec) && hp.token == 0xDEAD0001u);
	/* a zero token is never an offer */
	rec.he_token = 0u;
	assert(!tr_bus2_hp_poll(&hp, &rec));
	/* nor is a token word that is not the one the state word was published with */
	rec.he_token = 0xDEAD0003u;
	rec.he_state = tr_bus2_word(TR_BUS2_HE_OFFER, 0xDEAD0001u);
	assert(!tr_bus2_hp_poll(&hp, &rec));
}

static void test_stale_offer_of_a_dead_he(void)
{
	/* The previous session left an offer; this HE boots (void) and nobody wants the bus. */
	memset((void *)&rec, 0, sizeof(rec));
	rec.he_state = tr_bus2_word(TR_BUS2_HE_OFFER, 0xDEAD0002u);
	rec.he_token = 0xDEAD0002u;
	rec.he_beat  = 77u;
	tr_bus2_hp_boot(&rec, b_barrier);
	tr_bus2_hp_want(&hp, &rec, b_barrier);
	for (int i = 0; i < 100; i++) {
		assert(!tr_bus2_hp_poll(&hp, &rec)); /* no HE ticking: the offer is never taken */
	}
	memset(&he, 0, sizeof(he));
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS));
	assert(rec.he_state == TAG_OWNS && rec.he_token == 0u);
	rec.he_beat++;
	assert(!tr_bus2_hp_poll(&hp, &rec)); /* the HE's boot voided the stale offer */
}

static void test_fail_safe_no_hp(void)
{
	/* a stale WANT whose HP is dead: hp_beat frozen -> never offered */
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	for (int i = 0; i < 200; i++) {
		he_tick();
	}
	assert(gives == 0 && tr_bus2_he_owns_bus(&he) && he_irq);

	/* an HP that is asking and beating: offered */
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	he_tick();
	assert(gives == 1 && he.st == TR_BUS2_ST_OFFERED && !he_irq);

	/* an HP that beat once and then stopped: the beat goes stale after TR_BUS2_HP_FRESH_MS */
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	he.seen_hp_beat = rec.hp_beat; /* the HE saw that beat ... */
	he.hp_moved     = true;
	he.hp_moved_ms  = clk;
	clk += TR_BUS2_HP_FRESH_MS + 100; /* ... a long time ago */
	he_tick();
	assert(gives == 0 && he_irq);

	/* the same frozen WANT, early after the HE's boot (uptime below the freshness window): the
	 * beat must have MOVED, not merely be recent */
	cold_start();
	clk          = 50;
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	for (int i = 0; i < 3; i++) {
		tr_bus2_he_tick(&he, &rec, &ops, 0x77u);
		clk += 1;
	}
	assert(gives == 0 && he_irq);
}

static void test_enter_needs_the_offer_it_accepted(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	rec.he_token += 2u; /* the HE withdrew and offered again: a different lease */
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
}

static void test_cold_garbage_is_none(void)
{
	memset((void *)&rec, 0xA5, sizeof(rec));
	he_irq = 1;
	hp_irq = 0;
	clk    = 5;
	sleeps = 0;
	memset(&he, 0, sizeof(he));
	/* hp_state = 0xA5A5A5A5 is not tagged: no HP bus step to wait for */
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && sleeps == 0u);
	for (int i = 0; i < 20; i++) {
		he_tick();
	}
	assert(he.st == TR_BUS2_ST_OWNS); /* a garbage hp_state is not a WANT */

	memset((void *)&rec, 0xA5, sizeof(rec));
	tr_bus2_hp_want(&hp, &rec, b_barrier);
	rec.he_state = 0xA5A5A5A5u;
	rec.he_beat++;
	assert(!tr_bus2_hp_poll(&hp, &rec)); /* a garbage he_state is not an offer */
}

static void test_stale_return(void)
{
	cold_start();
	hp_full_lease();
	uint32_t t1 = rec.he_token;

	/* a second lease; while it is held, a late write of the FIRST lease's return lands */
	assert(hp_get_offer(50) > 0);
	assert(hp.token != t1);
	assert(hp_step_enter());
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	rec.hp_token = t1;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_RETURN, t1);
	for (int i = 0; i < 10; i++) {
		he_tick();
		assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq); /* the HE does not take the bus */
	}
	/* the real return */
	hp_disarm();
	rec.hp_token = hp.token;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_RETURN, hp.token);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
}

static void test_token_never_collides_with_the_last_hp_token(void)
{
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	rec.hp_token = 0x2011u; /* what the next nonce would produce: (0x1010 + 0x1001) | 1 ... */
	nonce_seq    = 0x2010u;
	he_tick(); /* nonce 0x2020 | 1 = 0x2021 */
	assert(rec.he_token != 0u && (rec.he_token & 1u) == 1u && rec.he_token != rec.hp_token);
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	nonce_seq    = 0x3000u - 0x10u;
	rec.hp_token = 0x3001u; /* exactly the token the HE is about to make */
	he_tick();
	assert(rec.he_token == 0x3003u);
	/* a token whose low 12 bits (the part a state word carries) equal the last HP token's or the
	 * HE's previous token's is skipped too: 0x5001 and 0x3001 share the low 12 bits */
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	nonce_seq    = 0x5000u - 0x10u;
	rec.hp_token = 0x3001u;
	he_tick();
	assert(rec.he_token == 0x5003u);
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	nonce_seq = 0x7000u - 0x10u;
	he.token  = 0x9001u; /* this HE's previous offer */
	he_tick();
	assert(rec.he_token == 0x7003u);
}

static void test_return_before_the_he_noticed_the_claim(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	/* the whole lease passes between two HE ticks (a long HE frame) */
	assert(hp_step_enter());
	hp_disarm();
	tr_bus2_hp_return(&hp, &rec, b_barrier);
	assert(he.st == TR_BUS2_ST_OFFERED);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
}

static void test_hp_restart_while_holding(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	uint32_t t1 = rec.he_token;

	/* the HP resets (its NVIC is fresh: line off); its PRE_KERNEL_1 forgets the old session */
	hp_disarm();
	tr_bus2_hp_boot(&rec, b_barrier);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq && rec.he_regains == 1u && reclaims == 0u);
	/* the new HP session asks again and gets a NEW token */
	hp_full_lease();
	assert(rec.he_token != t1 && rec.hp_acq == 1u);
}

static void test_hp_abort_returns_the_bus(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	/* the HP gave up the lease without a RETURN (abort: back to WANT) */
	hp_disarm();
	tr_bus2_hp_abort(&hp, &rec, b_barrier);
	assert(rec.hp_aborts == 1u);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
	/* it still wants the bus and beats: a new offer follows */
	hp_full_lease();
	assert(rec.hp_acq == 1u);
}

/* ---- the HE restarts inside a bring-up step ---- */
static int hp_step_ended;

static void hp_step_ends(void) /* runs on the HE's first 1 ms wait */
{
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier)); /* the next entry sees the void ... */
	hp_disarm();                                     /* ... aborts with no access ... */
	tr_bus2_hp_abort(&hp, &rec, b_barrier);          /* ... and asks again */
	hp_step_ended = 1;
}

static void test_he_restart_inside_a_bus_step(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter()); /* a bus step is running */
	/* the HE resets: its drivers are not up yet (line off) */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	hp_step_ended = 0;
	sleep_hook    = hp_step_ends;
	int64_t t0    = clk;

	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS));
	assert(hp_step_ended && rec.hp_aborts == 1u && clk - t0 < (int64_t)TR_BUS2_HE_BOOT_WAIT_MS);
	assert(rec.he_state == TAG_OWNS && !he.boot_stale);
	he_irq = 1; /* its i2c_dw driver comes up */
	check_exclusive();
	/* the HP asks again; the new HE offers a fresh token; the lease completes */
	hp_full_lease();
	assert(rec.hp_acq == 1u);
}

static void test_he_restart_waits_a_bounded_time(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter()); /* a bus step that never ends ... */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	sleep_beats = true; /* ... on an HP that is alive (hp_beat moves) */
	int64_t t0  = clk;

	assert(tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS)); /* timed out, live */
	assert(clk - t0 >= (int64_t)TR_BUS2_HE_BOOT_WAIT_MS &&
	       clk - t0 < (int64_t)TR_BUS2_HE_BOOT_WAIT_MS + 5);
	assert(rec.he_state == TAG_OWNS && !he.boot_stale);
	/* whatever the HP does next, it cannot enter another step */
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
}

static void test_he_restart_between_bus_steps_does_not_wait(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	hp_step_leave(); /* HELD: SPI1 / LP-GPIO / I2S3 only -- the IRQ line is off */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	sleeps = 0;
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && sleeps == 0u);
	he_irq = 1; /* its i2c_dw comes up while the HP holds the lease between steps */
	check_exclusive();
	assert(!hp_step_enter() && !hp_irq); /* the next bus step is refused: the line stays off */
}

/* The HP's IRQ line is armed only between an accepted entry and the leave: through a HELD stretch
 * (CC3501E reset, I2S3 bring-up) it is off, so an HE-only reset there never meets the HP's
 * i2c_dw_isr. This models every stretch of the bring-up as the sound code does. */
static void test_hp_line_is_off_while_held(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter() && hp_irq); /* steps 4-7 */
	hp_step_leave();                   /* steps 8-10 */
	assert(!hp_irq);
	for (int i = 0; i < 20; i++) { /* a long HELD stretch: ticks, the line stays off */
		he_tick();
		assert(!hp_irq && he.st == TR_BUS2_ST_CLAIMED);
	}
	/* the HE alone resets in that window: its line comes up, the HP's is off, nothing waited */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	sleeps = 0;
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && sleeps == 0u);
	he_irq = 1;
	check_exclusive();
	/* step 11: the entry is refused, so the line is never armed */
	assert(!hp_step_enter() && !hp_irq);
	tr_bus2_hp_abort(&hp, &rec, b_barrier);
	assert(hp_get_offer(100) > 0); /* a NEW lease from the new HE, and the HP can use it */
	assert(hp_step_enter() && hp_irq);
	hp_disarm();
	tr_bus2_hp_return(&hp, &rec, b_barrier);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq && !hp_irq);
}

/* ---- the HP dies holding the lease ---- */
static void ticks_for(int64_t ms)
{
	int64_t end = clk + ms;

	while (clk < end) {
		he_tick();
	}
}

static void test_hp_dies_held(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	hp_step_leave(); /* HELD, then the HP stops beating for good */
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	ticks_for(TR_BUS2_HP_DEAD_MS - 100); /* not yet dead */
	assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq && reclaims == 0u && rec.he_reclaims == 0u);
	ticks_for(200); /* past TR_BUS2_HP_DEAD_MS without a beat */
	assert(tr_bus2_he_owns_bus(&he) && he_irq && reclaims == 1u && rec.he_reclaims == 1u &&
	       rec.he_regains == 1u && takes == 1u && rec.he_state == TAG_OWNS);
	for (int i = 0; i < 100; i++) { /* the dead HP's leftover HELD is never a new claim */
		he_tick();
		assert(tr_bus2_he_owns_bus(&he) && gives == 1u && reclaims == 1u);
	}
	/* an HP that was only slow and comes back: its entry is refused, it asks again */
	assert(!hp_step_enter() && !hp_irq);
	tr_bus2_hp_abort(&hp, &rec, b_barrier);
	hp_full_lease();
}

static void test_hp_dies_in_a_bus_step(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter()); /* BUS, and the core dies mid-transfer */
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	hp_disarm(); /* a dead core's NVIC line is gone; the controller is wedged */
	ticks_for(TR_BUS2_HP_DEAD_MS - 100);
	assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq && reclaims == 0u);
	ticks_for(200);
	assert(tr_bus2_he_owns_bus(&he) && he_irq && reclaims == 1u && rec.he_reclaims == 1u);
	/* the HP restarts: PRE_KERNEL_1 -> NONE -> a fresh lease */
	tr_bus2_hp_boot(&rec, b_barrier);
	hp_full_lease();
	assert(rec.hp_acq == 1u && reclaims == 1u);
}

static void test_beating_hp_is_never_reclaimed(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(hp_step_enter());
	/* 20 s in a lease: a beat at least every ~100 ms (enter / leave / the step's own beat) */
	for (int i = 0; i < 200; i++) {
		clk += 100;
		if (i % 3 == 0) {
			hp_step_leave();
		} else if (i % 3 == 1) {
			assert(hp_step_enter());
		} else {
			rec.hp_beat++;
		}
		he_tick();
		assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq);
	}
	assert(reclaims == 0u && rec.he_reclaims == 0u);
	/* ... and the beat is the ONLY thing that matters: the same lease with a frozen beat dies */
	hp_disarm(); /* (a dead core's NVIC line is gone) */
	ticks_for(TR_BUS2_HP_DEAD_MS + 200);
	assert(reclaims == 1u && tr_bus2_he_owns_bus(&he));
}

static void test_enter_and_leave_beat(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	uint32_t b = rec.hp_beat;

	assert(hp_step_enter());
	assert(rec.hp_beat != b);
	b = rec.hp_beat;
	hp_step_leave();
	assert(rec.hp_beat != b);
}

/* ---- a stale BUS across a warm HE reset ---- */
static void hp_leaves_bus(void) /* the HP finishes its step on the HE's first wait */
{
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_HELD, 0x4321u);
}

static void hp_reenters_bus(void) /* the HP starts another step: BUS again, a new token */
{
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, 0x4322u);
}

static void test_stale_bus_is_a_warning(void)
{
	cold_start();
	/* a dead HP's BUS, still in SRAM0 after the HE's warm reset: nothing ever moves */
	rec.hp_token = 0x4321u;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, 0x4321u);
	memset(&he, 0, sizeof(he));
	int64_t t0 = clk;

	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS)); /* not a FAIL ... */
	assert(he.boot_stale);                                              /* ... a WARN */
	assert(clk - t0 >= (int64_t)TR_BUS2_HE_BOOT_WAIT_MS);               /* it did wait the bound */

	/* a LIVE HP still in the step at the timeout: the FAIL */
	cold_start();
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, 0x4321u);
	memset(&he, 0, sizeof(he));
	sleep_beats = true;
	assert(tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && !he.boot_stale);

	/* a state that moves (the HP leaves BUS mid-wait) ends the wait early and is not stale */
	cold_start();
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, 0x4321u);
	memset(&he, 0, sizeof(he));
	sleep_hook = hp_leaves_bus;
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && !he.boot_stale);

	/* a new BUS entry (same state, new token word) is movement too */
	cold_start();
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, 0x4321u);
	memset(&he, 0, sizeof(he));
	sleep_hook = hp_reenters_bus;
	assert(tr_bus2_he_boot(&he, &rec, &ops, 20u) && !he.boot_stale);
}

/* ---- a state word carries its token ---- */
static void test_state_word_carries_the_token(void)
{
	/* the HE reads ONE word: a BUS for another token (even with the right hp_token word) is no
	 * claim, so the offer is not taken as claimed, and later withdrawn */
	cold_start();
	assert(hp_get_offer(50) > 0);
	rec.hp_token = hp.token;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, hp.token ^ 1u);
	he_tick();
	assert(he.st == TR_BUS2_ST_OFFERED);

	/* the HP's entry: the HE's state word must be the OFFER of THIS token ... */
	cold_start();
	assert(hp_get_offer(50) > 0);
	rec.he_state = tr_bus2_word(TR_BUS2_HE_OFFER, hp.token ^ 1u); /* ... another token's word */
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
	/* ... and a word that is the OFFER of this token but with a different full he_token fails */
	cold_start();
	assert(hp_get_offer(50) > 0);
	rec.he_token = hp.token ^ 0x10000u; /* same low 12 bits, other token */
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
}

/* Dekker: each side writes its word, fences, THEN reads the other's. */
static uint32_t seen_at_fence;

static void capture_he_state(void)
{
	seen_at_fence = rec.he_state;
}

static void capture_hp_state(void)
{
	seen_at_fence = rec.hp_state;
}

static void void_he_state(void)
{
	rec.he_state = TAG_OWNS;
}

static void test_dekker_order(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	/* fence 1: the token is out, the state is not yet "bus step" */
	seen_at_fence = 0u;
	barrier_hook  = capture_hp_state;
	barrier_skip  = 0u;
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	assert(seen_at_fence != tr_bus2_word(TR_BUS2_HP_BUS, hp.token) && rec.hp_token == hp.token);
	/* fence 2: "bus step" (with its token) was already written when the fence ran ... */
	seen_at_fence = 0u;
	barrier_hook  = capture_hp_state;
	barrier_skip  = 1u;
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	assert(seen_at_fence == tr_bus2_word(TR_BUS2_HP_BUS, hp.token));
	/* ... and the HE's word is loaded AFTER it: an HE void landing at that fence is seen */
	barrier_hook = void_he_state;
	barrier_skip = 1u;
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
	rec.he_state = tr_bus2_word(TR_BUS2_HE_OFFER, hp.token); /* the offer stands again */
	hp_arm();
	memset(&he, 0, sizeof(he));
	seen_at_fence = 0u;
	barrier_hook  = capture_he_state;
	barrier_skip  = 0u;
	(void)tr_bus2_he_boot(&he, &rec, &ops, 5u);
	assert(seen_at_fence == TAG_OWNS); /* the void was already written when the fence ran */
}

/* ---- an unclaimed offer ---- */
static void hp_slips_in(void) /* an HP claim lands during the HE's withdrawal */
{
	rec.hp_token = hp.token;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, hp.token);
}

static void test_unclaimed_offer_is_withdrawn(void)
{
	cold_start();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_WANT;
	rec.hp_beat++;
	he_tick();
	assert(he.st == TR_BUS2_ST_OFFERED && !he_irq);
	clk += TR_BUS2_OFFER_TIMEOUT_MS - 100;
	he.t_offer = clk - (int64_t)TR_BUS2_OFFER_TIMEOUT_MS + 100; /* 100 ms short of the timeout */
	he_tick();
	assert(he.st == TR_BUS2_ST_OFFERED);
	clk += 200;
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq && rec.he_state == TAG_OWNS && takes == 1u);
	/* the HP, polling, sees no offer any more */
	tr_bus2_hp_want(&hp, &rec, b_barrier);
	rec.he_beat++;
	assert(!tr_bus2_hp_poll(&hp, &rec));
}

static void test_claim_races_the_withdrawal(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(he.st == TR_BUS2_ST_OFFERED);
	clk += TR_BUS2_OFFER_TIMEOUT_MS + 10;
	barrier_hook = hp_slips_in; /* fires at the withdrawal's fence, before it looks */
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq && takes == 0u);
	/* the offer is back: the HP's lease is honoured */
	assert(rec.he_state == tr_bus2_word(TR_BUS2_HE_OFFER, rec.he_token));
	hp_arm();
	hp_disarm();
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_RETURN, hp.token);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
}

/* The claim itself is a sign of life: an HE that stalled for seconds (no tick, no beat seen) and
 * then finds the lease claimed does not reclaim it on the very next tick. */
static void test_claim_is_a_sign_of_life(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	clk += 5000; /* a long HE stall; the HP's last beat the HE saw is far back */
	rec.hp_token = hp.token;
	rec.hp_state = tr_bus2_word(TR_BUS2_HP_BUS, hp.token); /* a claim with no beat in this window */
	he.seen_hp_beat = rec.hp_beat;                         /* (the beat was seen long ago) */
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED && reclaims == 0u);
	ticks_for(TR_BUS2_HP_DEAD_MS - 100);
	assert(reclaims == 0u);
	ticks_for(300);
	assert(reclaims == 1u);
}

int main(void)
{
	test_two_way();
	test_offer_waits_for_a_live_he();
	test_stale_offer_of_a_dead_he();
	test_fail_safe_no_hp();
	test_enter_needs_the_offer_it_accepted();
	test_cold_garbage_is_none();
	test_stale_return();
	test_token_never_collides_with_the_last_hp_token();
	test_return_before_the_he_noticed_the_claim();
	test_hp_restart_while_holding();
	test_hp_abort_returns_the_bus();
	test_he_restart_inside_a_bus_step();
	test_he_restart_waits_a_bounded_time();
	test_he_restart_between_bus_steps_does_not_wait();
	test_hp_line_is_off_while_held();
	test_hp_dies_held();
	test_hp_dies_in_a_bus_step();
	test_beating_hp_is_never_reclaimed();
	test_enter_and_leave_beat();
	test_claim_is_a_sign_of_life();
	test_stale_bus_is_a_warning();
	test_state_word_carries_the_token();
	test_dekker_order();
	test_unclaimed_offer_is_withdrawn();
	test_claim_races_the_withdrawal();
	printf("test_bus2: ok\n");
	return 0;
}
