/* src/ipc/tr_reset_guard.h -- the loop guard of the HE's last-resort whole-SoC reset.
 *
 * After a fatal error the HE records it (tr_he_fault.h) and asks the Secure Enclave to reset the
 * whole SoC (src/platform/he_fault.c), so the panel and the game come back without a person. A fault
 * that happens again right after every boot would reset forever, so the resets are counted in
 * always-on SRAM: the third fatal error in a row that comes before TR_RESET_GUARD_WINDOW_MS of
 * uptime halts instead (what Zephyr does without CONFIG_REBOOT), and the core stays inspectable.
 *
 * A fatal error after that many ms of uptime starts a new streak at 1 whatever the record says
 * (no timer is needed to end a streak). The flip side: a fault that comes more than the window
 * into EVERY boot is indistinguishable from a rare one and resets the SoC every ~30 s for good.
 * Cold SRAM (magic not ours, or a count above the limit) starts at 0. The count survives the SE's
 * SoC reset only if SRAM0 does; if it does not, the guard never trips (bench-verify, the
 * TR_BENCH_FAULT_INJECT word exists for that).
 *
 * Pure C so tests/host/test_reset_guard.c drives it. */
#ifndef TR_RESET_GUARD_H
#define TR_RESET_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#define TR_RESET_GUARD_MAGIC     0x47525354u /* 'GRST' */
#define TR_RESET_GUARD_WINDOW_MS 30000u
#define TR_RESET_GUARD_MAX       3u /* the Nth quick fatal error in a row halts */

typedef struct {
	volatile uint32_t magic;
	volatile uint32_t count; /* quick fatal errors in a row, this one included once recorded */
} tr_reset_guard_t;

#define TR_RESET_GUARD_SIZE 8u
_Static_assert(sizeof(tr_reset_guard_t) == TR_RESET_GUARD_SIZE, "tr_reset_guard_t layout");

/* A fatal error at `uptime_ms`: count it. True: ask for the SoC reset. False: halt (loop). */
static inline bool tr_reset_guard_allow(tr_reset_guard_t *g, uint32_t uptime_ms)
{
	/* A count above the limit is garbage (the record stops at the limit), not a streak. */
	uint32_t prev =
	    (g->magic == TR_RESET_GUARD_MAGIC && g->count <= TR_RESET_GUARD_MAX) ? g->count : 0u;
	uint32_t n = (uptime_ms < TR_RESET_GUARD_WINDOW_MS ? prev : 0u) + 1u;

	if (n > TR_RESET_GUARD_MAX) {
		n = TR_RESET_GUARD_MAX;
	}
	g->count = n;
	g->magic = TR_RESET_GUARD_MAGIC;
	return n < TR_RESET_GUARD_MAX;
}

#endif /* TR_RESET_GUARD_H */
