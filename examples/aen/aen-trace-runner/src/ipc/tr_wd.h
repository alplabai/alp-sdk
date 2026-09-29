/* src/ipc/tr_wd.h -- the HE's A32 renderer watchdog (TR_M55_AUTOLAUNCH), pure C.
 *
 * Decides which stub command (TR_CTRL_LAUNCH / TR_CTRL_HALT / nothing) the HE
 * writes, from the stub's mailbox state and the time. No Zephyr, no mailbox
 * access: src/platform/a32.c reads the mailbox, calls tr_wd_poll() and writes
 * the command; tests/host/test_wd.c drives the same code against a model stub.
 *
 * Rules (2026-09-24, after a cold boot on silicon HALTed a renderer that was
 * still initialising, three times, until an 8-command budget ran out):
 *   1. Nothing until the stub is alive (mailbox magic + a known stub_state).
 *      While it is not, the first-frame clock does not start: an HE that
 *      boots before the A32 chain waits for it, bounded only by the caller.
 *   2. A command is not stacked on an unconsumed one -- except a LAUNCH left
 *      under a RUNNING renderer (written while the release stub was already
 *      self-LAUNCHing): that one is only consumed after the renderer returns,
 *      so a HALT overwrites it.
 *   3. PARKED or FAULT: LAUNCH. Straight away after our own HALT (a recovery
 *      is HALT then LAUNCH, never HALT alone); at boot a PARKED stub gets
 *      TR_WD_SETTLE_US first (the release stub self-LAUNCHes after its MRAM
 *      copy); otherwise not before the backoff deadline of the last LAUNCH.
 *   4. RUNNING with no frame yet since boot / the last LAUNCH: HALT only once
 *      the first-frame timeout (TR_WD_FIRST_FRAME_US, doubled per failed
 *      LAUNCH since frames last ran TR_WD_WINDOW_US, capped at
 *      << TR_WD_BACKOFF_MAX_SHIFT) has passed. Never on a 100 ms miss.
 *   5. RUNNING after frames landed: a 100 ms miss (the caller's stall
 *      watchdog) is a stall -- HALT, then rule 3 relaunches.
 *   6. RUNNING a payload that is not ours (ctrl_entry/len/crc differ from
 *      the image this HE LAUNCHes -- a renderer left by an older release):
 *      HALT, then rule 3 LAUNCHes ours. Checked before adopting at boot.
 *   7. After a HALT (ours, or one seen pending -- a previous HE boot's),
 *      RUNNING with nothing pending: the renderer consumed the HALT and the
 *      stub is still parking (core 0 waits for core 1,
 *      then a set/way pass) -- wait, never a second HALT.
 * No budget: attempts are unlimited, spaced by the backoff.
 *
 * Known limits (the HE cannot recover these; a power cycle does):
 *   - a renderer hung so hard it never reads ctrl_cmd again: our HALT stays
 *     pending (TR_WD_PENDING) with the stub RUNNING, forever;
 *   - a stub core 0 that faults again before its park loop (stub.c
 *     stub_fault: "only a reset recovers it"): FAULT with our LAUNCH
 *     pending, forever.
 * tr_wd_pending_us() measures both; src/platform/a32.c logs it every second
 * and exports it as tr_a32_pending_ms. Future work: reset the A32 from the
 * HE through the SE (SERVICE_BOOT_RESET_CPU, hal_alif se_services
 * services_lib_api.h), unproven on this part -- then HALT pending past a
 * bound becomes a CPU reset + LAUNCH instead of a log line.
 */
#ifndef TR_WD_H
#define TR_WD_H

#include <stdbool.h>
#include <stdint.h>

#include "tr_mbox.h" /* TR_CTRL_*, TR_STUB_* */

/* First frame after a LAUNCH (or after the stub is first seen alive). The
 * renderer's LAUNCH -> first frame is ~0.1 s (CRC of the 452 KB image,
 * self-checks, render_init, one frame; estimate from host timings, measured
 * on silicon by the stamps a32.c prints), so 2 s is a 20x margin. */
#define TR_WD_FIRST_FRAME_US 2000000u
#define TR_WD_BACKOFF_MAX_SHIFT 4u    /* 2, 4, 8, 16, then every 32 s */
#define TR_WD_SETTLE_US      500000u  /* a PARKED stub at boot may still self-LAUNCH */
#define TR_WD_WINDOW_US      60000000u /* landed frames this long reset the backoff */

/* Why tr_wd_poll() did (or did not) command. */
typedef enum {
	TR_WD_NOT_ALIVE,     /* no magic / unknown stub_state: nothing to command yet */
	TR_WD_PENDING,       /* the previous command is not consumed yet */
	TR_WD_SETTLING,      /* boot: PARKED, waiting for a release self-LAUNCH */
	TR_WD_BACKING_OFF,   /* PARKED/FAULT, the next LAUNCH is not due yet */
	TR_WD_INITIALISING,  /* RUNNING, first frame not due yet */
	TR_WD_OK,            /* RUNNING, no stall seen */
	TR_WD_PARKING,       /* our HALT consumed, the stub not PARKED yet */
	TR_WD_LAUNCH_BOOT,   /* LAUNCH: PARKED at boot, no self-LAUNCH came */
	TR_WD_LAUNCH_HALTED, /* LAUNCH: our HALT parked it */
	TR_WD_LAUNCH_FAULT,  /* LAUNCH: the stub recorded a fault (or refused a LAUNCH) */
	TR_WD_LAUNCH_PARKED, /* LAUNCH: parked on its own (backoff due) */
	TR_WD_HALT_FIRST,    /* HALT: no first frame within the first-frame timeout */
	TR_WD_HALT_STALL,    /* HALT: a frame missed the stall watchdog mid-game */
	TR_WD_HALT_FOREIGN,  /* HALT: RUNNING a payload this HE does not LAUNCH */
	TR_WD_WHY_N
} tr_wd_why_t;

/* Short text for a tr_wd_why_t (log lines). */
const char *tr_wd_why_str(tr_wd_why_t why);

typedef struct {
	uint64_t wait_us;  /* start of the first-frame wait: stub first seen alive, or the last LAUNCH */
	uint64_t since_us; /* start of the current run of landed frames */
	uint64_t pending_us; /* since when a command has waited (PENDING / PARKING), if `pending` */
	uint32_t sent;     /* commands this boot */
	uint32_t tries;    /* LAUNCHes since frames last ran TR_WD_WINDOW_US: the backoff exponent */
	uint32_t last_cmd; /* last command written, TR_CTRL_* */
	bool     first;    /* no frame since boot / the last LAUNCH */
	bool     alive;    /* the stub has been seen alive */
	bool     running;  /* a run of landed frames is in progress */
	bool     pending;  /* the last poll found a command still in flight */
	bool     halting;  /* a HALT was sent or seen pending, the stub not parked since */
} tr_wd_t;

/* Boot: waiting for the first frame. */
void tr_wd_init(tr_wd_t *w, uint64_t now_us);

/* The first-frame deadline in force (wait_us + the backed-off timeout). */
uint64_t tr_wd_deadline_us(const tr_wd_t *w);

/* A frame landed at now_us. Returns true if it is the first since boot /
 * the last LAUNCH (then now_us - w->wait_us, read BEFORE the call, is the
 * LAUNCH -> first frame time). */
bool tr_wd_landed(tr_wd_t *w, uint64_t now_us);

/* How long the command in flight has waited as of the last poll (0: none). */
uint64_t tr_wd_pending_us(const tr_wd_t *w, uint64_t now_us);

/*
 * The command to write now (TR_CTRL_NONE: none), and why. `missed`: this
 * poll follows a frame that missed the stall watchdog (rule 5); boot polls
 * pass false. `ours`: the mailbox's ctrl_entry/len/crc are the image this HE
 * LAUNCHes (rule 6). A returned command counts as written: the caller must
 * write it.
 */
uint32_t tr_wd_poll(tr_wd_t *w, bool alive, uint32_t stub_state, uint32_t ctrl_cmd, bool ours, bool missed,
		    uint64_t now_us, tr_wd_why_t *why);

#endif /* TR_WD_H */
