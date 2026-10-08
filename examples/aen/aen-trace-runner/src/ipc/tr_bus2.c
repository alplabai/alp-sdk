/* src/ipc/tr_bus2.c -- see tr_bus2.h. Pure C, host + HE + HP. */
#include "tr_bus2.h"

static uint32_t tagged_state(uint32_t w)
{
	return (w & TR_BUS2_TAG_MASK) == TR_BUS2_TAG ? (w & 0xFFu) : 0u;
}

static uint32_t hp_st(const volatile tr_bus2_t *r)
{
	return tagged_state(r->hp_state);
}

static bool hp_holds(const volatile tr_bus2_t *r, uint32_t token)
{
	uint32_t s = hp_st(r);

	return (s == TR_BUS2_HP_BUS || s == TR_BUS2_HP_HELD) && r->hp_token == token;
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
	/* Dekker with tr_bus2_hp_enter(): void first, fence, THEN look at the HP's phase. */
	r->he_state = TR_BUS2_TAG | TR_BUS2_HE_OWNS;
	r->he_token = 0u;
	o->barrier();
	if (hp_st(r) != TR_BUS2_HP_BUS) {
		return false; /* no HP bus step running */
	}
	int64_t t0 = o->now_ms(o->ctx);

	while (hp_st(r) == TR_BUS2_HP_BUS) {
		if (o->now_ms(o->ctx) - t0 >= (int64_t)timeout_ms) {
			return true;
		}
		o->sleep_ms(o->ctx, 1u);
	}
	return false;
}

static void he_regain(tr_bus2_he_t *he, volatile tr_bus2_t *r, const tr_bus2_ops_t *o)
{
	r->he_state =
	    TR_BUS2_TAG | TR_BUS2_HE_OWNS; /* before the bus is re-armed: no HP entry passes */
	o->barrier();
	o->take(o->ctx);
	he->st = TR_BUS2_ST_OWNS;
	r->he_regains++;
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
	}
	switch (he->st) {
	case TR_BUS2_ST_OWNS:
		if (hp_st(r) == TR_BUS2_HP_WANT && he->hp_moved &&
		    now - he->hp_moved_ms <= (int64_t)TR_BUS2_HP_FRESH_MS) {
			uint32_t tok = nonce | 1u; /* never 0 */

			if (tok == r->hp_token) {
				tok += 2u;
			}
			o->give(o->ctx); /* the controller is stopped BEFORE anyone is told it is free */
			r->he_token = tok;
			o->barrier();
			r->he_state = TR_BUS2_TAG | TR_BUS2_HE_OFFER;
			o->barrier();
			he->st      = TR_BUS2_ST_OFFERED;
			he->token   = tok;
			he->t_offer = now;
		}
		break;
	case TR_BUS2_ST_OFFERED:
		if (hp_holds(r, he->token)) {
			he->st = TR_BUS2_ST_CLAIMED;
		} else if (hp_st(r) == TR_BUS2_HP_RETURN && r->hp_token == he->token) {
			he_regain(he, r, o); /* claimed and finished between two ticks */
		} else if (now - he->t_offer >= (int64_t)TR_BUS2_OFFER_TIMEOUT_MS) {
			/* Withdraw: void, fence, THEN look (Dekker with the HP's entry). An HP that
			 * slipped in meanwhile holds a lease the HE must honour: put the offer back. */
			r->he_state = TR_BUS2_TAG | TR_BUS2_HE_OWNS;
			o->barrier();
			if (hp_holds(r, he->token)) {
				r->he_state = TR_BUS2_TAG | TR_BUS2_HE_OFFER;
				o->barrier();
				he->st = TR_BUS2_ST_CLAIMED;
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
		if ((s == TR_BUS2_HP_RETURN && r->hp_token == he->token) || s == TR_BUS2_HP_NONE ||
		    s == TR_BUS2_HP_WANT) {
			he_regain(he, r, o);
		}
		break;
	}
	}
}

/* ---- HP ------------------------------------------------------------------------------------ */

static void hp_set(volatile tr_bus2_t *r, uint32_t st)
{
	r->hp_state = TR_BUS2_TAG | st;
}

void tr_bus2_hp_boot(volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_token = 0u;
	hp_set(r, TR_BUS2_HP_NONE);
	barrier();
}

void tr_bus2_hp_want(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	hp->base_beat = r->he_beat;
	hp->token     = 0u;
	hp_set(r, TR_BUS2_HP_WANT);
	barrier();
}

bool tr_bus2_hp_poll(tr_bus2_hp_t *hp, volatile tr_bus2_t *r)
{
	uint32_t he = r->he_state;

	r->hp_beat++;
	if ((he & TR_BUS2_TAG_MASK) != TR_BUS2_TAG || (he & 0xFFu) != TR_BUS2_HE_OFFER ||
	    r->he_beat == hp->base_beat) {
		return false; /* no offer, or one left by an HE that is not alive now */
	}
	hp->token = r->he_token;
	return hp->token != 0u;
}

bool tr_bus2_hp_enter(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_token = hp->token;
	hp_set(r, TR_BUS2_HP_BUS); /* first say "bus step" ... */
	barrier();                 /* ... fenced ... */
	uint32_t he =
	    r->he_state; /* ... THEN look at the offer (Dekker with tr_bus2_he_boot / withdraw) */

	return (he & TR_BUS2_TAG_MASK) == TR_BUS2_TAG && (he & 0xFFu) == TR_BUS2_HE_OFFER &&
	       r->he_token == hp->token;
}

void tr_bus2_hp_leave(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	(void)hp;
	hp_set(r, TR_BUS2_HP_HELD);
	barrier();
}

void tr_bus2_hp_return(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_token = hp->token;
	barrier();
	hp_set(r, TR_BUS2_HP_RETURN); /* the HE accepts it only with the token it offered */
	r->hp_acq++;
	barrier();
}

void tr_bus2_hp_abort(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void))
{
	r->hp_aborts++;
	tr_bus2_hp_want(hp, r, barrier); /* new liveness baseline: only an HE alive AFTER this counts */
}
