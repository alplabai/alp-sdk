/* src/ipc/tr_pslot.h -- HP -> HE pose slot, a seqlock (not a queue: only the
 * newest pose matters). Design: docs/superpowers/specs/
 * 2026-09-24-npu-body-control-design.md section 4. Address: tr_memmap.h
 * TR_MEM_PSLOT, SRAM0 0x0237F200, the sound ring's 4 KiB page (never mapped
 * by the A32). Both M55s run CONFIG_DCACHE=n, so no cache maintenance --
 * only the ordering barrier tr_mbox.h's protocol already uses.
 *
 * The TEXTBOOK odd/even seqlock, not tr_mbox.h's simpler single-bump variant
 * (an earlier version of this file used that instead, reasoning a single
 * atomic 32-bit write was enough -- REVIEWED AND FOUND WRONG: a writer that
 * bumps seq only once, after the body, leaves a window where the body is
 * being mutated while seq still reads as its OLD, valid-looking value; a
 * reader whose entire read (both barrier() calls) lands inside that window
 * sees the SAME unchanged seq at the re-check and accepts a torn body. Live
 * repro: tests/host/test_pslot.c's "left mid-publish" case, which the old
 * single-bump code accepted and this protocol rejects). Protocol: the
 * writer bumps seq odd BEFORE touching any body byte, writes the body,
 * bumps seq even after -- so ANY reader that samples seq while a write is
 * in progress, however long that write takes, sees an odd value and can
 * reject immediately, without needing the body to have changed again by the
 * time it re-checks. The reader: read seq s0; reject if unchanged since
 * last_seq OR odd; barrier(); copy the body; barrier(); reject if seq no
 * longer equals s0 (a second publish started, or the in-flight one
 * finished, mid-copy). Deliberately not tr_mbox_t itself: different
 * producer/consumer pair (HP -> HE, not M55 <-> A32), nothing gained from
 * sharing the type.
 *
 * NOTE ON THE DESIGN DOC'S TABLE vs THIS LAYOUT: section 4 places the
 * thumbnail at +0x80, but its own listed fields (tr_pose_t 102 B + infer_us
 * + pre_us + hp_state, all uint32, tightly packed with no gap before a
 * uint8_t array) put it at +0x84 -- the same kind of table/struct mismatch
 * tr_mbox.h's header comment records for tr_frame_in_t. This file's
 * _Static_assert block below is the real, binding layout. Total slot size
 * 0xA84 (2692 B) still sits well inside the sound ring's page (0x0237F200 +
 * 0xA84 = 0x0237FC84 < 0x0237FFFF). */
#ifndef TR_PSLOT_H
#define TR_PSLOT_H

#include <stdbool.h>
#include <stdint.h>

#include "../vision/pose.h"

#define TR_PSLOT_MAGIC   0x54525053u /* 'TRPS' */
#define TR_PSLOT_VERSION 1u

#define TR_PSLOT_THUMB_W     64
#define TR_PSLOT_THUMB_H     40
#define TR_PSLOT_THUMB_BYTES (TR_PSLOT_THUMB_W * TR_PSLOT_THUMB_H) /* 2560, GREY8 */

/* tr_pslot_t.hp_state: what the HP was doing when it last wrote the slot --
 * lets the HE's HUD (design sec 6) and any bench tooling tell "no player"
 * (a valid, empty-booth pose) apart from "the HP pipeline itself is stuck". */
#define TR_HP_STATE_RUNNING   0u /* normal: camera + NPU + decode all ran this frame */
#define TR_HP_STATE_NO_CAMERA 1u /* tr_camera_open() failed or has not been retried yet */
#define TR_HP_STATE_I2C_STUCK 2u /* the I2C1 unstick sequence did not clear the SCCB bus */
#define TR_HP_STATE_NO_FRAME  3u /* camera open but no frame arrived this pass (queue empty) */
#define TR_HP_STATE_SRAM1_NOT_READY \
	4u /* CAM_POOL (SRAM1) gate not confirmed yet -- design fix
                                        * round 2, hp_vision/src/main.c tr_sram1_ready() */

typedef struct {
	/* +0x00, 16 B: identity + generation. */
	uint32_t magic, version, seq, frame_no;
	/* +0x10: the pose itself, then this pass's cost, DWT-timed on the HP. */
	tr_pose_t pose;
	uint32_t  infer_us; /* NPU invoke only (Vela: ~9,220 typical) */
	uint32_t  pre_us;   /* tr_movenet_input() letterbox */
	uint32_t  hp_state; /* +0x80: TR_HP_STATE_* */
	/* +0x84: HUD thumbnail (design sec 6) -- 10x nearest-sample decimation
	 * of the RAW 640x400 frame (hp_vision/src/main.c make_thumb(); NOT the
	 * letterboxed/padded 192x192 tensor, see camera_ae.h's header note on
	 * why that buffer is a biased source), 64x40 GREY8. Valid only when
	 * hp_state == RUNNING. */
	uint8_t thumb[TR_PSLOT_THUMB_BYTES];
} tr_pslot_t;

#include <stddef.h>
_Static_assert(offsetof(tr_pslot_t, pose) == 0x10, "pose must start at +0x10 (design sec 4)");
_Static_assert(sizeof(tr_pose_t) == 102, "tr_pose_t layout drifted -- see this file's header note");
_Static_assert(offsetof(tr_pslot_t, thumb) == 0x84,
               "thumb offset drifted -- see this file's header note");
_Static_assert(sizeof(tr_pslot_t) == 0xA84,
               "tr_pslot_t total size drifted -- update the header note's arithmetic");

/* HP side: write a new pose. Copies pose + infer_us/pre_us/hp_state and the
 * thumbnail (thumb may be NULL to leave it unchanged -- e.g. an hp_state
 * that isn't RUNNING has no fresh frame to downsample from), barrier(), then
 * bumps seq by one -- see the protocol note above. frame_no is the caller's
 * free-running counter (e.g. total frames captured), carried through same as
 * the body, unlike seq. */
void tr_pslot_write(volatile tr_pslot_t *s,
                    const tr_pose_t     *pose,
                    uint32_t             infer_us,
                    uint32_t             pre_us,
                    uint32_t             hp_state,
                    const uint8_t       *thumb,
                    uint32_t             frame_no,
                    void (*barrier)(void));

/* HE side: copy the slot if seq is EVEN (rejects a write left mid-publish)
 * and unchanged across the copy (rejects one that starts or finishes during
 * the copy), magic/version checked too. Returns false (out untouched) on a
 * torn/mid-publish read, a version/magic mismatch, or seq == last_seq
 * (nothing new). */
bool tr_pslot_read(const volatile tr_pslot_t *s,
                   uint32_t                   last_seq,
                   tr_pslot_t                *out,
                   uint32_t                  *seq,
                   void (*barrier)(void));

#endif /* TR_PSLOT_H */
