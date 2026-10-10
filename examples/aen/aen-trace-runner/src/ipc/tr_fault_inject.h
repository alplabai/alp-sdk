/* src/ipc/tr_fault_inject.h -- BENCH ONLY (CMake TR_BENCH_FAULT_INJECT): make the HE take a fatal
 * error on demand, to exercise the fatal-error path (the record, the SE SoC reset and the loop guard
 * in tr_reset_guard.h) on silicon.
 *
 * The bench writes two words over SWD to TR_MEM_FAULT_INJECT: delay_ms first, magic LAST. The HE's
 * main loop calls k_panic() as soon as the magic is there and its uptime has passed the delay. The
 * word is NEVER cleared by the HE, so the fault comes back on every boot while SRAM0 keeps it: that
 * is what lets the guard be seen halting the third quick fatal error. Cold SRAM (the magic is not
 * ours) does nothing, and so does a build without the option.
 *
 * Pure C so tests/host/test_fault_inject.c drives it. */
#ifndef TR_FAULT_INJECT_H
#define TR_FAULT_INJECT_H

#include <stdbool.h>
#include <stdint.h>

#define TR_FAULT_INJECT_MAGIC 0x464A4E54u /* 'FJNT' */

typedef struct {
	volatile uint32_t delay_ms; /* fault once uptime is >= this */
	volatile uint32_t magic;    /* TR_FAULT_INJECT_MAGIC arms it; written last */
} tr_fault_inject_t;

#define TR_FAULT_INJECT_SIZE 8u
_Static_assert(sizeof(tr_fault_inject_t) == TR_FAULT_INJECT_SIZE, "tr_fault_inject_t layout");

static inline bool tr_fault_inject_due(const tr_fault_inject_t *f, uint32_t uptime_ms)
{
	return f->magic == TR_FAULT_INJECT_MAGIC && uptime_ms >= f->delay_ms;
}

#endif /* TR_FAULT_INJECT_H */
