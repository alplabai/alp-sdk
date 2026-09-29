/* src/ipc/tr_aring.c -- see tr_aring.h. Pure C, barrier supplied by the
 * caller (same convention as tr_mbox.c), builds for host, HE and HP. */
#include "tr_aring.h"

void tr_aring_init(volatile tr_aring_t *r, void (*barrier)(void))
{
	if (r->magic == TR_ARING_MAGIC && r->version == TR_ARING_VERSION) {
		r->head     = r->tail; /* drop whatever a previous HE left unread */
		r->hp_state = TR_ARING_HP_OFF;
		barrier();
		return;
	}
	r->magic = 0u;
	barrier();
	for (volatile uint32_t *p = &r->version; p < (volatile uint32_t *)(r + 1); p++) {
		*p = 0u;
	}
	r->version = TR_ARING_VERSION;
	barrier();
	r->magic = TR_ARING_MAGIC;
	barrier();
}

bool tr_aring_push(volatile tr_aring_t *r, uint8_t kind, uint8_t param, void (*barrier)(void))
{
	uint32_t h = r->head;

	if (h - r->tail >= TR_ARING_CAP) {
		r->dropped = r->dropped + 1u;
		return false;
	}
	volatile tr_aev_t *e = &r->ev[h & (TR_ARING_CAP - 1u)];
	e->kind  = kind;
	e->param = param;
	barrier(); /* slot visible before the head bump advertises it */
	r->head = h + 1u;
	barrier();
	return true;
}

bool tr_aring_pop(volatile tr_aring_t *r, tr_aev_t *out, void (*barrier)(void))
{
	if (r->magic != TR_ARING_MAGIC || r->version != TR_ARING_VERSION) {
		return false; /* not set up, or an HE built against another tr_aring_t layout */
	}
	uint32_t h = r->head;
	uint32_t t = r->tail;

	if (h == t) {
		return false;
	}
	if (h - t > TR_ARING_CAP) {
		r->tail = h;
		return false;
	}
	barrier(); /* head read before the slot read */
	volatile tr_aev_t *e = &r->ev[t & (TR_ARING_CAP - 1u)];
	out->kind  = e->kind;
	out->param = e->param;
	out->pad0  = 0u;
	out->pad1  = 0u;
	barrier(); /* slot read before the tail bump frees it */
	r->tail = t + 1u;
	return true;
}
