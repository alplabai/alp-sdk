/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ISP-fed colour capture for the Linux camera backend (yocto_drv.c).
 *
 * NOT a standalone translation unit: yocto_drv.c #includes this after its
 * ioctl/poll/mmap test seams, so the ISP path shares them.
 *
 * ADR 0017 Tier-1.5 (thin glue over a vendor V4L2 node): this file writes no ISP
 * driver and no sensor tuning.  It only reads frames from the V4L2 node the
 * Renesas RZ/V2N ISP Support Package (Arm Mali-C55 / IV021 kernel driver +
 * its userspace control daemon) already exposes, /dev/video<N>fr, a
 * V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE capture node that delivers
 * demosaiced, auto-exposed, auto-white-balanced colour frames.  The
 * package is licence-gated and opt-in (meta-alp-sdk ALP_ENABLE_ISP).
 *
 * @par Selection
 *      yocto_isp_open() is tried first by y_open().  It declines with
 *      ALP_ERR_NOSUPPORT -- and the raw media-controller path runs exactly
 *      as before -- when the ISP stack is absent (no /dev/video<N>fr), when
 *      the node is not a multi-planar capture node, or for a pixel format
 *      the ISP path does not produce (GREY8 / RAW8 / RAW10 stay on the raw
 *      path).  camera_id N maps to /dev/video<N>fr, the ISP's own sensor
 *      index (the order of `sensor-i2c` in the ISP device-tree node).
 *
 * @par Formats
 *      ALP_PIXFMT_RGB565 -> V4L2_PIX_FMT_RGB565, ALP_PIXFMT_NV12 ->
 *      V4L2_PIX_FMT_NV12 (single plane, Y then interleaved UV).  Both are
 *      in the ISP's documented output list.  The requested size must be one
 *      the ISP node accepts as-is (readback checked): the active crop and
 *      sensor mode are set by the ISP init script, not here.
 *
 * @par Status
 *      BENCH-UNVERIFIED: unit-tested against a scripted ioctl hook only.
 *      The Support Package ships one reference sensor; other Bayer
 *      sensors (IMX219) need their own port (docs/v2n-isp.md).  alp_camera_configure_isp() is
 *      ALP_ERR_NOSUPPORT: AE/AWB are owned by the ISP's userspace daemon,
 *      whose control IDs are not mapped onto alp_camera_isp_config_t.
 */

#ifndef ALP_BACKENDS_CAMERA_YOCTO_ISP_CAPTURE_H
#define ALP_BACKENDS_CAMERA_YOCTO_ISP_CAPTURE_H

/* Test seam: open the ISP node for camera_id; returns an fd or -1/errno. */
static int (*g_cam_isp_open)(uint32_t camera_id);

typedef struct {
	int      fd;
	uint32_t width;
	uint32_t height;
	size_t   sizeimage; /* per-frame bytes the node reports */

	unsigned nbuf;
	void    *map[CAM_NBUF];
	size_t   map_len[CAM_NBUF];
	bool     held[CAM_NBUF]; /* app owns map[i] until release() */
	bool     streaming;
	bool     got_frame;
} isp_cam_t;

static int isp_node_open(uint32_t camera_id)
{
	if (g_cam_isp_open != NULL) return g_cam_isp_open(camera_id);
	char path[32];
	(void)snprintf(path, sizeof(path), "/dev/video%ufr", camera_id);
	return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static uint32_t isp_fourcc(alp_pixfmt_t pf)
{
	switch (pf) {
	case ALP_PIXFMT_RGB565:
		return V4L2_PIX_FMT_RGB565;
	case ALP_PIXFMT_NV12:
		return V4L2_PIX_FMT_NV12;
	default:
		return 0u;
	}
}

/* Smallest frame (bytes) that holds w x h in pixel format @p pf. */
static size_t isp_min_image(alp_pixfmt_t pf, uint32_t w, uint32_t h)
{
	size_t px = (size_t)w * h;
	return pf == ALP_PIXFMT_NV12 ? px + px / 2u : px * 2u;
}

static void isp_free_buffers(isp_cam_t *c)
{
	for (unsigned i = 0; i < CAM_NBUF; ++i) {
		if (c->map[i] != NULL) cam_munmap(c->map[i], c->map_len[i]);
		c->map[i] = NULL;
	}
	c->nbuf = 0;
}

static alp_status_t isp_qbuf(isp_cam_t *c, unsigned i)
{
	struct v4l2_plane  pl;
	struct v4l2_buffer b;
	memset(&pl, 0, sizeof(pl));
	memset(&b, 0, sizeof(b));
	b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory   = V4L2_MEMORY_MMAP;
	b.index    = i;
	b.m.planes = &pl;
	b.length   = 1u;
	return cam_ioctl(c->fd, VIDIOC_QBUF, &b) < 0 ? ALP_ERR_IO : ALP_OK;
}

static alp_status_t isp_alloc_buffers(isp_cam_t *c)
{
	struct v4l2_requestbuffers rb;
	memset(&rb, 0, sizeof(rb));
	rb.count  = CAM_NBUF;
	rb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (cam_ioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0) return alp_status_from_posix_errno(errno);
	if (rb.count < 2u) return ALP_ERR_NOMEM;
	if (rb.count > CAM_NBUF) rb.count = CAM_NBUF;

	for (unsigned i = 0; i < rb.count; ++i) {
		struct v4l2_plane  pl;
		struct v4l2_buffer b;
		memset(&pl, 0, sizeof(pl));
		memset(&b, 0, sizeof(b));
		b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory   = V4L2_MEMORY_MMAP;
		b.index    = i;
		b.m.planes = &pl;
		b.length   = 1u;
		if (cam_ioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) return alp_status_from_posix_errno(errno);
		if ((size_t)pl.length < c->sizeimage) return ALP_ERR_IO;
		c->map[i] = cam_mmap(c->fd, pl.length, (off_t)pl.m.mem_offset);
		if (c->map[i] == NULL) return ALP_ERR_NOMEM;
		c->map_len[i] = pl.length;
		c->nbuf       = i + 1u;
	}
	return ALP_OK;
}

/* Request the frame rate; log (never fail) when the node cannot apply it.
 * Returns the settled rate x1000, 0 when the node does not report one. */
static uint32_t isp_apply_fps(isp_cam_t *c, uint8_t fps)
{
	struct v4l2_streamparm sp;
	memset(&sp, 0, sizeof(sp));
	sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (fps != 0u) {
		sp.parm.capture.timeperframe.numerator   = 1u;
		sp.parm.capture.timeperframe.denominator = fps;
		if (cam_ioctl(c->fd, VIDIOC_S_PARM, &sp) < 0) {
			fprintf(
			    stderr, "alp_camera: ISP node did not apply the %u fps request\n", (unsigned)fps);
		}
	}
	memset(&sp, 0, sizeof(sp));
	sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (cam_ioctl(c->fd, VIDIOC_G_PARM, &sp) < 0) return 0u;
	uint32_t num = sp.parm.capture.timeperframe.numerator;
	uint32_t den = sp.parm.capture.timeperframe.denominator;
	if (num == 0u || den == 0u) return 0u;
	return (uint32_t)((uint64_t)den * 1000u / num);
}

static void isp_close(alp_camera_backend_state_t *st);

static alp_status_t isp_start(alp_camera_backend_state_t *st)
{
	isp_cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	if (c->streaming) return ALP_OK;
	int          type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	alp_status_t rc   = ALP_OK;
	for (unsigned i = 0; i < c->nbuf && rc == ALP_OK; ++i) {
		if (c->held[i]) continue; /* app still owns it until release() */
		rc = isp_qbuf(c, i);
	}
	if (rc == ALP_OK && cam_ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		rc = alp_status_from_posix_errno(errno);
	}
	if (rc != ALP_OK) {
		(void)cam_ioctl(c->fd, VIDIOC_STREAMOFF, &type);
		return rc;
	}
	c->streaming = true;
	c->got_frame = false;
	return ALP_OK;
}

static alp_status_t isp_stop(alp_camera_backend_state_t *st)
{
	isp_cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	if (!c->streaming) return ALP_OK;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (cam_ioctl(c->fd, VIDIOC_STREAMOFF, &type) < 0) return ALP_ERR_IO;
	c->streaming = false;
	return ALP_OK;
}

static alp_status_t
isp_capture(alp_camera_backend_state_t *st, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	isp_cam_t *c = st->be_data;
	if (c == NULL || !c->streaming) return ALP_ERR_NOT_READY;

	struct pollfd p  = { .fd = c->fd, .events = POLLIN };
	int           to = timeout_ms > (uint32_t)INT_MAX ? -1 : (int)timeout_ms;
	int           pr = cam_poll(&p, to);
	if (pr < 0) return ALP_ERR_IO;
	if (pr == 0) {
		if (!c->got_frame) {
			fprintf(stderr,
			        "alp_camera: no first ISP frame within %u ms; check that the ISP "
			        "userspace daemon is running (alp-isp-init.service) and the sensor is "
			        "streaming\n",
			        (unsigned)timeout_ms);
		}
		return ALP_ERR_TIMEOUT;
	}
	if (p.revents & (POLLERR | POLLNVAL)) return ALP_ERR_IO;

	struct v4l2_plane  pl;
	struct v4l2_buffer b;
	memset(&pl, 0, sizeof(pl));
	memset(&b, 0, sizeof(b));
	b.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory   = V4L2_MEMORY_MMAP;
	b.m.planes = &pl;
	b.length   = 1u;
	if (cam_ioctl(c->fd, VIDIOC_DQBUF, &b) < 0) {
		return errno == EAGAIN ? ALP_ERR_TIMEOUT : ALP_ERR_IO;
	}
	if (b.index >= c->nbuf) return ALP_ERR_IO;
	c->got_frame = true;

	/* Same integrity rule as the raw path: flagged or short frames are
	 * requeued and reported, never handed out. */
	if ((b.flags & V4L2_BUF_FLAG_ERROR) != 0u ||
	    (pl.bytesused != 0u && (size_t)pl.bytesused < c->sizeimage)) {
		fprintf(stderr, "alp_camera: dropped a corrupt or short ISP frame\n");
		(void)isp_qbuf(c, b.index);
		return ALP_ERR_IO;
	}
	c->held[b.index]  = true;
	out->data         = c->map[b.index];
	out->size         = pl.bytesused != 0u ? pl.bytesused : c->map_len[b.index];
	out->timestamp_us = (uint64_t)b.timestamp.tv_sec * 1000000u + (uint64_t)b.timestamp.tv_usec;
	return ALP_OK;
}

static alp_status_t isp_release(alp_camera_backend_state_t *st, alp_camera_frame_t *frame)
{
	isp_cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	for (unsigned i = 0; i < c->nbuf; ++i) {
		if (frame->data != c->map[i] || !c->held[i]) continue;
		c->held[i] = false;
		return c->streaming ? isp_qbuf(c, i) : ALP_OK;
	}
	return ALP_ERR_INVAL;
}

static alp_status_t isp_configure_isp(alp_camera_backend_state_t    *st,
                                      const alp_camera_isp_config_t *isp)
{
	(void)st;
	(void)isp;
	return ALP_ERR_NOSUPPORT;
}

static void isp_close(alp_camera_backend_state_t *st)
{
	isp_cam_t *c = st->be_data;
	if (c == NULL) return;
	if (c->streaming) (void)isp_stop(st);
	isp_free_buffers(c);
	if (c->fd >= 0) close(c->fd);
	free(c);
	st->be_data = NULL;
}

static const alp_camera_ops_t _isp_ops = {
	.open          = NULL, /* never dispatched: yocto_drv's y_open() routes here */
	.start         = isp_start,
	.stop          = isp_stop,
	.capture       = isp_capture,
	.release       = isp_release,
	.configure_isp = isp_configure_isp,
	.close         = isp_close,
};

/**
 * @brief Try to open camera @p cfg->camera_id through the ISP node.
 *
 * On ALP_OK the handle state is rebound to the ISP vtable.  ALP_ERR_NOSUPPORT
 * means "not the ISP path" and leaves @p st untouched so the caller falls
 * through to the raw media-controller path.
 */
static alp_status_t isp_try_open(const alp_camera_config_t *cfg, alp_camera_backend_state_t *st)
{
	uint32_t fourcc = isp_fourcc(cfg->format);
	if (fourcc == 0u) return ALP_ERR_NOSUPPORT;

	int fd = isp_node_open(cfg->camera_id);
	if (fd < 0) {
		return (errno == ENOENT || errno == ENODEV) ? ALP_ERR_NOSUPPORT
		                                            : alp_status_from_posix_errno(errno);
	}

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if (cam_ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		/* The node exists: report the real failure instead of letting the
		 * raw path mask it as NOSUPPORT.  ENOTTY = not a V4L2 node. */
		int err = errno;
		close(fd);
		return err == ENOTTY ? ALP_ERR_NOSUPPORT : alp_status_from_posix_errno(err);
	}
	uint32_t caps =
	    (cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0u ? cap.device_caps : cap.capabilities;
	if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0u || (caps & V4L2_CAP_STREAMING) == 0u) {
		close(fd);
		return ALP_ERR_NOSUPPORT;
	}

	isp_cam_t *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		close(fd);
		return ALP_ERR_NOMEM;
	}
	const alp_camera_ops_t *prev_ops = st->ops;
	c->fd                            = fd;
	st->be_data                      = c;
	st->ops                          = &_isp_ops;

	struct v4l2_format vf;
	memset(&vf, 0, sizeof(vf));
	vf.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vf.fmt.pix_mp.width       = cfg->width;
	vf.fmt.pix_mp.height      = cfg->height;
	vf.fmt.pix_mp.pixelformat = fourcc;
	vf.fmt.pix_mp.field       = V4L2_FIELD_ANY;
	alp_status_t rc           = ALP_OK;
	if (cam_ioctl(fd, VIDIOC_S_FMT, &vf) < 0) {
		rc = alp_status_from_posix_errno(errno);
	} else if (vf.fmt.pix_mp.pixelformat != fourcc || vf.fmt.pix_mp.width != cfg->width ||
	           vf.fmt.pix_mp.height != cfg->height) {
		rc = ALP_ERR_INVAL; /* the ISP node adjusted the request: not accepted */
	} else if (vf.fmt.pix_mp.num_planes != 1u) {
		rc = ALP_ERR_NOSUPPORT; /* one contiguous frame per buffer only */
	} else {
		c->width     = cfg->width;
		c->height    = cfg->height;
		c->sizeimage = vf.fmt.pix_mp.plane_fmt[0].sizeimage;
		if (c->sizeimage < isp_min_image(cfg->format, c->width, c->height)) rc = ALP_ERR_IO;
	}
	if (rc == ALP_OK) {
		st->fps_x1000 = isp_apply_fps(c, cfg->fps);
		rc            = isp_alloc_buffers(c);
	}
	if (rc != ALP_OK) {
		isp_close(st);
		st->ops = prev_ops; /* NOSUPPORT must fall through to the raw path */
		return rc;
	}
	return ALP_OK;
}

#endif /* ALP_BACKENDS_CAMERA_YOCTO_ISP_CAPTURE_H */
