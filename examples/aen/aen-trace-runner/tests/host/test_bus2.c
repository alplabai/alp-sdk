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
 *   - cold SRAM0 garbage is "none", never a state. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_bus2.h"

#define TAG_OWNS  (TR_BUS2_TAG | TR_BUS2_HE_OWNS)
#define TAG_OFFER (TR_BUS2_TAG | TR_BUS2_HE_OFFER)
#define TAG_BUS   (TR_BUS2_TAG | TR_BUS2_HP_BUS)

static volatile tr_bus2_t rec;
static int64_t            clk;
static int                he_irq, hp_irq; /* each core's I2C2 NVIC line */
static unsigned           gives, takes, sleeps;
static void (*barrier_hook)(void);
static void (*sleep_hook)(void);

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
	if (sleep_hook) {
		void (*h)(void) = sleep_hook;

		sleep_hook = NULL;
		h();
	}
}

static const tr_bus2_ops_t ops = { b_barrier, b_give, b_take, b_now, b_sleep, NULL };

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

/* Both cores just booted: the HE first (its drivers up: line on), then the HP. */
static void cold_start(void)
{
	memset((void *)&rec, 0, sizeof(rec));
	he_irq = 1;
	hp_irq = 0;
	gives = takes = sleeps = 0;
	clk                    = 100000;
	barrier_hook           = NULL;
	sleep_hook             = NULL;
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
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
	tr_bus2_hp_leave(&hp, &rec, b_barrier);
	he_tick(); /* the HE sees the claim and stays off the bus */
	assert(!tr_bus2_he_owns_bus(&he) && he.st == TR_BUS2_ST_CLAIMED && !he_irq);
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
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
	       rec.hp_aborts == 0);
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
	rec.he_state = TAG_OFFER;
	rec.he_token = 0xDEAD0001u;
	for (int i = 0; i < 500; i++) {
		assert(!tr_bus2_hp_poll(&hp, &rec));
	}
	rec.he_beat++; /* an HE that is alive now */
	assert(tr_bus2_hp_poll(&hp, &rec) && hp.token == 0xDEAD0001u);
	/* a zero token is never an offer */
	rec.he_token = 0u;
	assert(!tr_bus2_hp_poll(&hp, &rec));
}

static void test_stale_offer_of_a_dead_he(void)
{
	/* The previous session left an offer; this HE boots (void) and nobody wants the bus. */
	memset((void *)&rec, 0, sizeof(rec));
	rec.he_state = TAG_OFFER;
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
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	rec.hp_token = t1;
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_RETURN;
	for (int i = 0; i < 10; i++) {
		he_tick();
		assert(he.st == TR_BUS2_ST_CLAIMED && !he_irq); /* the HE does not take the bus */
	}
	/* the real return */
	hp_disarm();
	rec.hp_token = hp.token;
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_RETURN;
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
}

static void test_return_before_the_he_noticed_the_claim(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	/* the whole lease passes between two HE ticks (a long HE frame) */
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
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
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
	he_tick();
	assert(he.st == TR_BUS2_ST_CLAIMED);
	uint32_t t1 = rec.he_token;

	/* the HP resets (its NVIC is fresh: line off); its PRE_KERNEL_1 forgets the old session */
	hp_disarm();
	tr_bus2_hp_boot(&rec, b_barrier);
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq && rec.he_regains == 1u);
	/* the new HP session asks again and gets a NEW token */
	hp_full_lease();
	assert(rec.he_token != t1 && rec.hp_acq == 1u);
}

static void test_hp_abort_returns_the_bus(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
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
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier)); /* a bus step is running */
	hp_arm();
	/* the HE resets: its drivers are not up yet (line off) */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	hp_step_ended = 0;
	sleep_hook    = hp_step_ends;
	int64_t t0    = clk;

	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS));
	assert(hp_step_ended && rec.hp_aborts == 1u && clk - t0 < (int64_t)TR_BUS2_HE_BOOT_WAIT_MS);
	assert(rec.he_state == TAG_OWNS);
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
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier)); /* a bus step that never ends */
	hp_arm();
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	int64_t t0 = clk;

	assert(tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS)); /* timed out */
	assert(clk - t0 >= (int64_t)TR_BUS2_HE_BOOT_WAIT_MS &&
	       clk - t0 < (int64_t)TR_BUS2_HE_BOOT_WAIT_MS + 5);
	assert(rec.he_state == TAG_OWNS);
	/* whatever the HP does next, it cannot enter another step */
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier));
}

static void test_he_restart_between_bus_steps_does_not_wait(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	hp_arm();
	tr_bus2_hp_leave(&hp, &rec, b_barrier); /* HELD: SPI1 / LP-GPIO / I2S3 only */
	he_irq = 0;
	memset(&he, 0, sizeof(he));
	sleeps = 0;
	assert(!tr_bus2_he_boot(&he, &rec, &ops, TR_BUS2_HE_BOOT_WAIT_MS) && sleeps == 0u);
	assert(!tr_bus2_hp_enter(&hp, &rec, b_barrier)); /* the next bus step is refused */
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

static void test_dekker_order(void)
{
	cold_start();
	assert(hp_get_offer(50) > 0);
	seen_at_fence = 0u;
	barrier_hook  = capture_hp_state;
	assert(tr_bus2_hp_enter(&hp, &rec, b_barrier));
	assert(seen_at_fence == TAG_BUS); /* "bus step" was already written when the fence ran */
	hp_arm();
	memset(&he, 0, sizeof(he));
	seen_at_fence = 0u;
	barrier_hook  = capture_he_state;
	(void)tr_bus2_he_boot(&he, &rec, &ops, 5u);
	assert(seen_at_fence == TAG_OWNS); /* the void was already written when the fence ran */
}

/* ---- an unclaimed offer ---- */
static void hp_slips_in(void) /* an HP claim lands during the HE's withdrawal */
{
	rec.hp_token = hp.token;
	rec.hp_state = TAG_BUS;
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
	assert(rec.he_state == TAG_OFFER); /* the offer is back: the HP's lease is honoured */
	hp_arm();
	hp_disarm();
	rec.hp_state = TR_BUS2_TAG | TR_BUS2_HP_RETURN;
	he_tick();
	assert(tr_bus2_he_owns_bus(&he) && he_irq);
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
	test_dekker_order();
	test_unclaimed_offer_is_withdrawn();
	test_claim_races_the_withdrawal();
	printf("test_bus2: ok\n");
	return 0;
}
