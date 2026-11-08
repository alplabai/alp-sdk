/* src/ipc/tr_he_fault.c -- see tr_he_fault.h. */
#include "tr_he_fault.h"

static void fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void arm_tail(tr_he_fault_t *r)
{
	for (size_t i = 0; i < TR_HE_TAIL; i++) {
		r->tail[i] = 0;
	}
	r->tail_pos = 0u;
	fence();
	r->tail_magic = TR_HE_TAIL_MAGIC;
	fence();
}

void tr_he_fault_tail_put(tr_he_fault_t *r, char c)
{
	if (r->tail_magic != TR_HE_TAIL_MAGIC) {
		arm_tail(r);
	}
	r->tail[r->tail_pos % TR_HE_TAIL] = c;
	r->tail_pos++;
}

void tr_he_fault_record(tr_he_fault_t *r,
                        uint32_t       reason,
                        uint32_t       pc,
                        uint32_t       lr,
                        uint32_t       cfsr,
                        uint32_t       hfsr,
                        uint32_t       mmfar,
                        uint32_t       bfar,
                        uint32_t       uptime_ms)
{
	r->reason    = reason;
	r->pc        = pc;
	r->lr        = lr;
	r->cfsr      = cfsr;
	r->hfsr      = hfsr;
	r->mmfar     = mmfar;
	r->bfar      = bfar;
	r->uptime_ms = uptime_ms;
	fence();
	r->magic = TR_HE_FAULT_MAGIC;
	fence();
}

bool tr_he_fault_take(tr_he_fault_t *r, tr_he_fault_snap_t *s)
{
	s->fault     = r->magic == TR_HE_FAULT_MAGIC;
	s->reason    = r->reason;
	s->pc        = r->pc;
	s->lr        = r->lr;
	s->cfsr      = r->cfsr;
	s->hfsr      = r->hfsr;
	s->mmfar     = r->mmfar;
	s->bfar      = r->bfar;
	s->uptime_ms = r->uptime_ms;
	s->tail_len  = 0u;
	s->tail[0]   = 0;

	if (r->tail_magic == TR_HE_TAIL_MAGIC) {
		uint32_t pos = r->tail_pos;
		size_t   n   = pos < TR_HE_TAIL ? pos : TR_HE_TAIL;
		uint32_t at  = pos < TR_HE_TAIL ? 0u : pos % TR_HE_TAIL; /* oldest character */

		for (size_t i = 0; i < n; i++) {
			s->tail[i] = r->tail[(at + i) % TR_HE_TAIL];
		}
		s->tail[n]  = 0;
		s->tail_len = n;
	}

	r->magic = 0u;
	arm_tail(r);
	return s->fault;
}
