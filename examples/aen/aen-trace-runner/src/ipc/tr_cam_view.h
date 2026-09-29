/* src/ipc/tr_cam_view.h -- HP -> A32 live camera view descriptor (fix round 7,
 * item 5: "CAMERA 640x400 . NPU <Hz>" picture-in-picture; design:
 * docs/superpowers/specs/2026-09-24-npu-body-control-design.md sec 13).
 *
 * NOT the pixels themselves -- those stay exactly where they already are,
 * CAM_POOL (SRAM1 0x02480000..0x024FFFFF, design sec 2), read by the A32
 * renderer the same way the HP's own NPU pre-process already does. This is
 * a small SRAM0 descriptor (address of the latest COMPLETE frame in that
 * pool, its dimensions, its frame number) plus a seqlock, the same
 * odd-before/even-after protocol as tr_pslot.h.
 *
 * TEAR RISK, BY DESIGN (see the design doc for the full ruling): the video
 * buffer pool has only CONFIG_VIDEO_BUFFER_POOL_NUM_MAX=2 buffers
 * (hp_vision's board overlay). Holding a buffer past its normal release
 * point -- to guarantee it can never be recycled for a NEW DMA capture
 * while the A32 is still reading it -- would cost hp_vision a frame of
 * NPU throughput every single tick (a pipeline stall this design goes to
 * real lengths elsewhere to avoid, e.g. camera.c's own newest-frame-only
 * FIFO policy). Instead: the HP publishes the SAME seq-before/seq-after
 * protocol as tr_pslot_write() around JUST this small descriptor -- it does
 * NOT extend the buffer's own release deadline at all, tr_camera_release()
 * still runs exactly where it always has, right after the NPU's own
 * pre-process copies out of the buffer. The reader (the A32 renderer) must
 * treat a torn OR STALE read (seq changed again between its own start and
 * end of copying pixel rows out of buf_addr, i.e. the buffer got recycled
 * and possibly already holds a NEW, different frame's bytes mid-copy) as
 * "skip this frame, keep last frame's PiP on screen" -- never as a hard
 * error. A torn/recycled read is therefore a rendered-a-half-old-frame
 * COSMETIC risk (one stale or split frame in a picture-in-picture, at most
 * a 33 ms glitch, self-correcting next frame), not a data-loss or crash
 * risk -- deliberately accepted rather than paying the throughput cost of
 * a buffer hold, per the design ruling.
 */
#ifndef TR_CAM_VIEW_H
#define TR_CAM_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#define TR_CAM_VIEW_MAGIC   0x54524356u /* 'TRCV' */
#define TR_CAM_VIEW_VERSION 2u /* 2: + rotate (half/half layout); a v1 HP reads as unpublished */

typedef struct {
	uint32_t magic, version, seq;
	uint32_t buf_addr; /* physical address in CAM_POOL (SRAM1) of the latest COMPLETE frame */
	uint32_t frame_no;
	uint16_t width, height; /* 640, 400 -- carried rather than assumed, so a future sensor
				  * mode change is a data change, not a silent mismatch */
	uint16_t rotate;        /* the raw frame's rotation to upright (src/vision/cam_rot.h
				  * TR_CAM_ROTATE, the HP's build switch): the reader draws what the
				  * HP applied, so pixels and pose keypoints always agree */
	uint16_t mirror;        /* 1: the sensor mirrors the upright view (cam_rot.h TR_CAM_MIRROR,
				  * read back from the sensor); the pixels arrive mirrored, so the
				  * reader draws them as-is -- informational. Was a zero pad in v2. */
} tr_cam_view_t;

/* HP side: publish a new frame's location. Same protocol as
 * tr_pslot_write() -- see this file's header for why the buffer itself is
 * NOT held past its normal release. */
void tr_cam_view_write(volatile tr_cam_view_t *s, uint32_t buf_addr, uint32_t frame_no, uint16_t width,
			uint16_t height, uint16_t rotate, uint16_t mirror, void (*barrier)(void));

/* A32 side: copy the descriptor if seq is even and unchanged across the
 * copy, magic/version checked too -- identical shape to tr_pslot_read().
 * out->buf_addr/width/height are the buffer's OWN claim, not yet range-
 * checked against CAM_POOL -- the caller (a32/renderer/render.c's
 * render_video_band()) does that BEFORE touching a single pixel byte, not
 * as a post-copy re-check (fix round 12, review: an EARLIER version of
 * this promised a tr_cam_view_still_seq() re-check after the pixel copy,
 * but a call that only ever fires once the (much slower) copy has already
 * run cannot prevent an out-of-bounds read in the first place -- it could
 * only flag stale CONTENT after the fact, this file's own "cosmetic risk,
 * accept it" ruling above, so render.c's own call was made, its result
 * discarded, and nothing acted on it: dead code promising a check that
 * wasn't happening. tr_cam_view_still_seq() below is still correct and
 * still tested (tests/host/test_cam_view.c) -- just not this reader's own
 * mitigation for the tear risk above; a future caller that genuinely needs
 * a post-copy recycle check (not just an address-safety one) can still
 * reach for it). */
bool tr_cam_view_read(const volatile tr_cam_view_t *s, tr_cam_view_t *out, void (*barrier)(void));

/* true when `s->seq` is still exactly `seq` -- for a caller that wants to
 * detect a mid-copy buffer recycle the seqlock's own read-side check could
 * not see (that check only spans the tiny descriptor copy, not a much
 * longer pixel-row copy after it). Available, host-tested
 * (tests/host/test_cam_view.c) -- see tr_cam_view_read()'s own comment for
 * why the A32 renderer uses a range check instead, not this. */
static inline bool tr_cam_view_still_seq(const volatile tr_cam_view_t *s, uint32_t seq)
{
	return s->seq == seq;
}

#endif /* TR_CAM_VIEW_H */
