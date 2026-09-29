/* src/ipc/tr_cam_view.c -- see tr_cam_view.h. Same shape as tr_pslot.c. */
#include "tr_cam_view.h"

void tr_cam_view_write(volatile tr_cam_view_t *s,
                       uint32_t                buf_addr,
                       uint32_t                frame_no,
                       uint16_t                width,
                       uint16_t                height,
                       uint16_t                rotate,
                       uint16_t                mirror,
                       void (*barrier)(void))
{
	s->seq = s->seq + 1u; /* even -> odd */
	barrier();
	s->magic    = TR_CAM_VIEW_MAGIC;
	s->version  = TR_CAM_VIEW_VERSION;
	s->buf_addr = buf_addr;
	s->frame_no = frame_no;
	s->width    = width;
	s->height   = height;
	s->rotate   = rotate;
	s->mirror   = mirror;
	barrier();
	s->seq = s->seq + 1u; /* odd -> even */
	barrier();
}

bool tr_cam_view_read(const volatile tr_cam_view_t *s, tr_cam_view_t *out, void (*barrier)(void))
{
	uint32_t s0 = s->seq;

	if (s0 & 1u) {
		return false;
	}
	barrier();
	*out = *s;
	barrier();
	if (s->seq != s0) {
		return false;
	}
	if (out->magic != TR_CAM_VIEW_MAGIC || out->version != TR_CAM_VIEW_VERSION) {
		return false;
	}
	return true;
}
