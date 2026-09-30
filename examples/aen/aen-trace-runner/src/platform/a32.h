/* src/platform/a32.h -- the M55 end of the A32 renderer link (TR_RENDER=A32). */
#ifndef TR_PLATFORM_A32_H
#define TR_PLATFORM_A32_H

#include "../ipc/tr_mbox.h"

/*
 * Once per M55 boot, before the first tr_a32_present(): write the mailbox
 * identity and -- only with TR_M55_AUTOLAUNCH -- wait (bounded) for the stub,
 * then adopt a RUNNING renderer (self-LAUNCHed, or left by a previous M55
 * boot) or LAUNCH a PARKED one (src/ipc/tr_wd.h). The mailbox is in SRAM1,
 * which is powered only when the A32 chain booted: without it the first
 * access bus-faults.
 */
void tr_a32_boot(void);

/*
 * One frame: wait for the A32 to finish the frame published last time (100 us
 * poll, 100 ms stall watchdog; with TR_M55_AUTOLAUNCH a miss goes to
 * src/ipc/tr_wd.h, which gives a first frame longer), flip to it (blocks to vblank -- this is what paces
 * the game), then publish `in` for the now-free buffer. On a watchdog miss the
 * last frame stays up and `in` is published anyway.
 */
void tr_a32_present(const tr_frame_in_t *in);

/*
 * The first half of tr_a32_present() on its own: put the frame already
 * published on the glass (or hit the watchdog), and publish nothing. Before a
 * deliberate hold -- a published frame is not visible until it is flushed.
 */
void tr_a32_flush(void);

#endif /* TR_PLATFORM_A32_H */
