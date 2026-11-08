/* src/ipc/tr_he_fault.h -- what the M55-HE leaves behind for its next boot: the last fatal error
 * it took, and the tail of its console, in always-on SRAM0 (the shared NC page, which survives a
 * warm reset like the I2C1 handover words do).
 *
 * WHY. The HE's .bss (and with it ram_console_buf) is zeroed by the startup code on every boot,
 * and ram_console restarts at offset 0, so the console text from before a reboot is gone; and a
 * fatal error halts the core (CONFIG_REBOOT is off, so RESET_ON_FATAL_ERROR does not apply), so a
 * reboot after one never happens from Zephyr. A reboot with no record here came from outside the
 * core (a flash tool's SYSRESETREQ, the Secure Enclave, a supply glitch); the console tail says what
 * it was doing.
 *
 *   tail   the last TR_HE_TAIL characters printk()ed, a ring (the glue tees the console into it);
 *   fault  reason / PC / LR / CFSR / HFSR / MMFAR / BFAR / uptime of the last fatal error, written
 *          by k_sys_fatal_error_handler() with the magic LAST.
 *
 * Pure C (no Zephyr) so tests/host/test_he_fault.c drives it. src/platform/he_fault.c is the glue. */
#ifndef TR_HE_FAULT_H
#define TR_HE_FAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TR_HE_FAULT_MAGIC 0x544C4648u /* 'HFLT' */
#define TR_HE_TAIL_MAGIC  0x4C494154u /* 'TAIL' */
#define TR_HE_TAIL        352u

typedef struct {
	volatile uint32_t magic; /* TR_HE_FAULT_MAGIC: the fault fields below are valid */
	volatile uint32_t reason;
	volatile uint32_t pc;
	volatile uint32_t lr;
	volatile uint32_t cfsr;
	volatile uint32_t hfsr;
	volatile uint32_t mmfar;
	volatile uint32_t bfar;
	volatile uint32_t uptime_ms;
	volatile uint32_t tail_magic; /* TR_HE_TAIL_MAGIC: tail_pos / tail are valid */
	volatile uint32_t tail_pos;   /* characters ever written; the next index is tail_pos % TAIL */
	volatile char     tail[TR_HE_TAIL];
} tr_he_fault_t;

#define TR_HE_FAULT_SIZE 0x18Cu
_Static_assert(sizeof(tr_he_fault_t) == TR_HE_FAULT_SIZE, "tr_he_fault_t layout");

/* What the next boot reads: a copy, so the SRAM record can be re-armed straight away. */
typedef struct {
	bool     fault; /* a fatal error was recorded */
	uint32_t reason, pc, lr, cfsr, hfsr, mmfar, bfar, uptime_ms;
	size_t   tail_len;
	char     tail[TR_HE_TAIL + 1u]; /* chronological, NUL-terminated */
} tr_he_fault_snap_t;

/* One console character into the ring (arms an uninitialised ring first). */
void tr_he_fault_tail_put(tr_he_fault_t *r, char c);

/* The fatal error, the magic written last. */
void tr_he_fault_record(tr_he_fault_t *r,
                        uint32_t       reason,
                        uint32_t       pc,
                        uint32_t       lr,
                        uint32_t       cfsr,
                        uint32_t       hfsr,
                        uint32_t       mmfar,
                        uint32_t       bfar,
                        uint32_t       uptime_ms);

/* Boot: copy what the last boot left into *s, then re-arm the record (fault cleared, ring empty).
 * Returns s->fault. A record whose magics are not ours (cold SRAM) reads as nothing. */
bool tr_he_fault_take(tr_he_fault_t *r, tr_he_fault_snap_t *s);

#endif /* TR_HE_FAULT_H */
