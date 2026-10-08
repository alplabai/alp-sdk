/* src/ipc/tr_bus2.c -- see tr_bus2.h. Pure C, host + HE + HP. */
#include <stddef.h>

#include "tr_bus2.h"

/* A state word's state, or 0 (none) for a word without the tag (cold SRAM0). */
static uint32_t word_state(uint32_t w)
{
	return (w & TR_BUS2_TAG_MASK) == TR_BUS2_TAG ? (w & TR_BUS2_ST_MASK) : 0u;
}

static uint32_t word_token(uint32_t w)
{
	return (w >> TR_BUS2_TOK_SHIFT) & TR_BUS2_TOK_MASK;
}

static uint32_t hp_st(const volatile tr_bus2_t *r)
{
	return word_state(r->hp_state);
}

/* One load decides: the HP is in a bus step or holds the lease, for THIS token. */
static bool hp_holds(const volatile tr_bus2_t *r, uint32_t token)
{
	uint32_t w = r->hp_state;
	uint32_t s = word_state(w);

	return (s == TR_BUS2_HP_BUS || s == TR_BUS2_HP_HELD) &&
	       word_token(w) == (token & TR_BUS2_TOK_MASK);
}

static bool hp_returned(const volatile tr_bus2_t *r, uint32_t token)
{
	uint32_t w = r->hp_state;

	return word_state(w) == TR_BUS2_HP_RETURN && word_token(w) == (token & TR_BUS2_TOK_MASK);
}

/* ---- HE ------------------------------------------------------------------------------------ */

bool tr_bus2_he_boot(tr_bus2_he_t        *he,
                     volatile tr_bus2_t  *r,
                     const tr_bus2_ops_t *o,
                     uint32_t             timeout_ms)
{
	he->st           = TR_BUS2_ST_OWNS;
	he->token        = 0u;
	he->seen_hp_beat = r->hp_beat;
	he->hp_moved     = false;
	he->t_hp_seen    = 0;
	he->boot_stale   = false;
	he->leased       = 0u;
	r->he_regains    = 0u;
	r->he_reclaims   = 0u;
	/* Dekker with tr_bus2_hp_enter(): void first, fence, THEN look at the HP's phase. */
	r->he_state = tr_bus2_word(TR_BUS2_HE_OWNS, 0u);
	r->he_token = 0u;
	o->barrier();
	if (hp_st(r) != TR_BUS2_HP_BUS) {
		return false; /* no HP bus step running */
	}
	int64_t  t0    = o->now_ms(o->ctx);
	uint32_t beat0 = r->hp_beat;
	uint32_t word0 = r->hp_state;
	bool     moved = false;

	while (hp_st(r) == TR_BUS2_HP_BUS) {
		moved = moved || r->hp_beat != beat0 || r->hp_state != word0;
		if (o->now_ms(o->ctx) - t0 >= (int64_t)timeout_ms) {
			he->boot_stale = !moved; /* BUS, but nothing moved: a dead HP's leftover */
			return moved;
		}
		o->sleep_ms(o->ctx, 1u);
	}
	return false;
}

static void he_regain(tr_bus2_he_t *he, volatile tr_bus2_t *r, const tr_bus2_ops_t *o)
{
	/* before the bus is re-armed: no HP entry passes */
	r->he_state = tr_bus2_word(TR_BUS2_HE_OWNS, 0u);
	o->barrier();
	o->take(o->ctx);
	he->st = TR_BUS2_ST_OWNS;
	r->he_regains++;
}

static void he_claimed(tr_bus2_he_t *he, int64_t now)
{
	he->leased++;
	he->st        = TR_BUS2_ST_CLAIMED;
	he->t_hp_seen = now; /* the claim is itself a sign of life */
}

void tr_bus2_he_tick(tr_bus2_he_t        *he,
                     volatile tr_bus2_t  *r,
                     const tr_bus2_ops_t *o,
                     uint32_t             nonce)
{
	int64_t  now = o->now_ms(o->ctx);
	uint32_t hb  = r->hp_beat;

	r->he_beat++; /* "an HE is alive now" for an HP deciding whether an offer is stale */
	if (hb != he->seen_hp_beat) {
		he->seen_hp_beat = hb;
		he->hp_moved_ms  = now;
		he->hp_moved     = true;
		he->t_hp_seen    = now;
	}
	switch (he->st) {
	case TR_BUS2_ST_OWNS:
		if (hp_st(r) == TR_BUS2_HP_WANT && he->hp_moved &&
		    now - he->hp_moved_ms <= (int64_t)TR_BUS2_HP_FRESH_MS) {
			/* Odd (never 0), and its low 12 bits (the part a state word carries) differ from
			 * the HP's last token and from this HE's previous one: a late write of an old
			 * session never matches the new lease. */
			uint32_t tok = nonce | 1u;

			while (((tok ^ r->hp_token) & TR_BUS2_TOK_MASK) == 0u ||
			       ((tok ^ he->token) & TR_BUS2_TOK_MASK) == 0u) {
				tok += 2u;
			}
			o->give(o->ctx); /* the controller is stopped BEFORE anyone is told it is free */
			r->he_token = tok;
			o->barrier();
			r->he_state = tr_bus2_word(TR_BUS2_HE_OFFER, tok);
			o->barrier();
			he->st      = TR_BUS2_ST_OFFERED;
			he->token   = tok;
			he->t_offer = now;
		}
		break;
	case TR_BUS2_ST_OFFERED:
		if (hp_holds(r, he->token)) {
			he_claimed(he, now);
		} else if (hp_returned(r, he->token)) {
			he->leased++; /* the lease passed between two ticks: counted from the RETURN */
			he_regain(he, r, o);
		} else if (now - he->t_offer >= (int64_t)TR_BUS2_OFFER_TIMEOUT_MS) {
			/* Withdraw: void, fence, THEN look (Dekker with the HP's entry). An HP that
			 * slipped in meanwhile holds a lease the HE must honour: put the offer back. */
			r->he_state = tr_bus2_word(TR_BUS2_HE_OWNS, 0u);
			o->barrier();
			if (hp_holds(r, he->token)) {
				r->he_state = tr_bus2_word(TR_BUS2_HE_OFFER, he->token);
				o->barrier();
				he_claimed(he, now);
			} else {
				he_regain(he, r, o);
			}
		}
		break;
	default: /* TR_BUS2_ST_CLAIMED */ {
		uint32_t s = hp_st(r);

		/* Back: the HP returned THIS lease. Or the holder is gone: an HP that restarted
		 * (hp_state NONE) or aborted (WANT) is not using the bus. A RETURN for another token
		 * (a late write of a previous session) changes nothing. */
		if (hp_returned(r, he->token) || s == TR_BUS2_HP_NONE || s == TR_BUS2_HP_WANT) {
			he_regain(he, r, o);
		} else if (now - he->t_hp_seen > (int64_t)TR_BUS2_HP_DEAD_MS) {
			/* The HP holds the lease but has not beaten for TR_BUS2_HP_DEAD_MS (it beats on
			 * every enter / leave and every ~100 ms in a step): it died mid-lease. The bus
			 * may be wedged mid-transfer -- stop the controller, clear the bus, take it. */
			o->reclaim(o->ctx);
			r->he_reclaims++;
			he_regain(he, r, o);
		}
		break;
	}
	}
}

const char *tr_bus2_he_event(const tr_bus2_he_t *he, uint8_t before, uint32_t leased_before)
{
	if (he->st == before) {
		return NULL;
	}
	switch (he->st) {
	case TR_BUS2_ST_OFFERED:
		return "I2C2 offered to the HP";
	case TR_BUS2_ST_CLAIMED:
		return "I2C2 leased by the HP";
	default:
		if (before == TR_BUS2_ST_OFFERED) {
			return he->leased != leased_before
			           ? "I2C2 leased by the HP and returned within one frame: back on the HE"
			           : "I2C2 offer withdrawn (not claimed): back on the HE";
		}
		return "I2C2 back on the HE";
	}
}

/* ---- HP ------------------------------------------------------------------------------------ */

static void hp_set(volatile tr_bus2_t *r, uint32_t st, uint32_t token)
{
	r->hp_state = tr_bus2_word(st, token);
}

void tr_bus2_hp_boot(volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_aborts  = 0u;
	r->hp_i2s_fu  = 0u;
	r->hp_i2s_err = 0u;
	r->hp_acq     = 0u;
	r->hp_token   = 0u;
	hp_set(r, TR_BUS2_HP_NONE, 0u);
	barrier();
}

void tr_bus2_hp_want(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	hp->base_beat = r->he_beat;
	hp->token     = 0u;
	hp_set(r, TR_BUS2_HP_WANT, 0u);
	barrier();
}

bool tr_bus2_hp_poll(tr_bus2_hp_t *hp, volatile tr_bus2_t *r)
{
	uint32_t he = r->he_state;

	r->hp_beat++;
	if (word_state(he) != TR_BUS2_HE_OFFER || r->he_beat == hp->base_beat) {
		return false; /* no offer, or one left by an HE that is not alive now */
	}
	uint32_t tok = r->he_token;

	if (tok == 0u || word_token(he) != (tok & TR_BUS2_TOK_MASK)) {
		return false; /* the token word is not the one this state word was published with */
	}
	hp->token = tok;
	return true;
}

bool tr_bus2_hp_enter(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_beat++;
	r->hp_token = hp->token;
	barrier();                            /* the token is visible before the state that names it */
	hp_set(r, TR_BUS2_HP_BUS, hp->token); /* first say "bus step" ... */
	barrier();                            /* ... fenced ... */
	/* ... THEN look at the offer (Dekker with tr_bus2_he_boot / withdraw): ONE load of the state
	 * word, token included, decides. */
	uint32_t he = r->he_state;

	barrier(); /* the state word is loaded before the token word below */
	return he == tr_bus2_word(TR_BUS2_HE_OFFER, hp->token) && r->he_token == hp->token;
}

void tr_bus2_hp_leave(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_beat++;
	hp_set(r, TR_BUS2_HP_HELD, hp->token);
	barrier();
}

void tr_bus2_hp_return(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_token = hp->token;
	barrier();
	hp_set(r, TR_BUS2_HP_RETURN, hp->token); /* the HE accepts it only with the token it offered */
	r->hp_acq++;
	barrier();
}

void tr_bus2_hp_abort(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_aborts++;
	tr_bus2_hp_want(hp, r, barrier); /* new liveness baseline: only an HE alive AFTER this counts */
}
