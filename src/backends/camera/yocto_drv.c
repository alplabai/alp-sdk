/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto camera backend: V4L2 + media-controller.  Binds the
 * alp_camera dispatcher's ops vtable to a sensor -> (CSI-2 rx) -> capture
 * video node pipeline.  Written against the generic kernel ABI, so it is
 * SoC-agnostic (silicon_ref "*"); the RZ/V2N CSI-2 + CRU pipeline is the
 * reference target.
 *
 * @par Discovery
 *      camera_id N names a sensor through the device-tree alias
 *      `alp-camera<N>` (/proc/device-tree/aliases/alp-camera<N> holds the
 *      sensor node's DT path).  Every /dev/media* is scanned with
 *      MEDIA_IOC_G_TOPOLOGY for the MEDIA_ENT_F_CAM_SENSOR entity whose
 *      subdev's `device/of_node` resolves to that same DT node; enabled
 *      data links are then followed source -> sink until the
 *      MEDIA_ENT_F_IO_V4L entity (the capture node).  No entity name is
 *      hard-coded and no link is ever changed (links are immutable on the
 *      RZ CRU).  A missing alias or an absent sensor is ALP_ERR_NOT_READY.
 *
 * @par Format negotiation
 *      The requested alp pixfmt picks the media-bus code the sensor offers
 *      (VIDIOC_SUBDEV_ENUM_MBUS_CODE); width/height must be a size the
 *      sensor produces natively (no scaler in the path, otherwise
 *      ALP_ERR_INVAL).  The format is pushed along the chain with
 *      VIDIOC_SUBDEV_S_FMT, then VIDIOC_S_FMT on the video node:
 *        - ALP_PIXFMT_GREY8 : Y8 -> GREY, or Y10 -> CR10 unpacked >>2
 *        - ALP_PIXFMT_RAW8  : Bayer8 -> the matching Bayer fourcc, or Y8
 *        - ALP_PIXFMT_RAW10 : Bayer10 / Y10 -> CR10 unpacked to one
 *                             uint16 per pixel
 *      CR10 is the RZ CRU's 64-bit packed 10-bit layout (6 pixels per
 *      little-endian word, 10 bits each, row pitch = bytesperline).
 *      Colour formats (RGB/YUV/NV12) are not produced by this backend: a
 *      sensor-to-colour conversion is the ISP/colour-processing layer's
 *      job, so they return ALP_ERR_NOSUPPORT -- unless the Renesas ISP
 *      stack is installed (see @par ISP), which produces RGB565 and NV12.
 *
 * @par ISP
 *      When the opt-in RZ/V2N ISP Support Package is on the image, camera N
 *      also has an ISP capture node /dev/video<N>fr.  An ALP_PIXFMT_RGB565
 *      or ALP_PIXFMT_NV12 open is then served from that node (a Bayer
 *      sensor streaming demosaiced colour); every other format, and every
 *      image without the package, takes the media-controller path above.
 *      See yocto_isp_capture.h and docs/v2n-isp.md.
 *
 * @par Frame rate
 *      fps 0 keeps the driver default.  Otherwise V4L2_CID_VBLANK on the
 *      sensor is set from V4L2_CID_PIXEL_RATE and the current HBLANK,
 *      clamped to the control's [minimum, maximum] and rounded to its
 *      step.  The request is a request (camera.h): a rate the sensor
 *      cannot reach is logged and settles on the nearest one, never an
 *      open failure.  alp_camera_get_fps() reports the settled rate x1000.
 *
 * @par Frame integrity
 *      A dequeued buffer flagged V4L2_BUF_FLAG_ERROR, or one whose
 *      bytesused is short of the negotiated frame, is requeued and
 *      reported as ALP_ERR_IO; it is never handed to the caller.
 *
 * @par Status
 *      Compiled and unit-tested against an ioctl hook; on-target capture
 *      is BENCH-UNVERIFIED in this file's own CI.  configure_isp() is
 *      ALP_ERR_NOSUPPORT.  The Bayer8 -> V4L2_PIX_FMT_S*8 mapping (k_raw8)
 *      is UNVERIFIED against the RZ CRU format table (RAW8 Bayer may be a
 *      CRU-specific fourcc, as RAW10 is); a mismatch surfaces as a loud
 *      ALP_ERR_INVAL from the S_FMT readback, never as silent corruption.
 *      CR10 + Y10 (OV9281) is the bench-proven path.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "camera_ops.h"
#include "common/alp_errno.h"

#define CAM_NBUF      4u
#define CAM_MAX_CHAIN 8u
#define CAM_MAX_CODES 32u
#define CAM_MAX_MEDIA 16

/* RZ CRU 10-bit raw, 64-bit packed.  Newer uapi headers name it
 * V4L2_PIX_FMT_RAW_CRU10; the fourcc is the stable part. */
#define CAM_FOURCC_CR10 v4l2_fourcc('C', 'R', '1', '0')

/* media_v2_link.flags is __u32, but the uapi MEDIA_LNK_FL_LINK_TYPE mask is
 * (0xf << 28), a signed-int overflow.  Unsigned copies keep the & and ==
 * comparisons free of sign conversion. */
#define CAM_LNK_TYPE_MASK      (0xfu << 28)
#define CAM_LNK_TYPE_INTERFACE (1u << 28)

/* One hop of the sensor -> capture-node chain. */
typedef struct {
	uint32_t ent_id;
	uint32_t function;
	uint32_t sink_pad; /* pad index data enters (unused for the sensor) */
	uint32_t src_pad;  /* pad index data leaves (unused for the video node) */
	uint32_t major;
	uint32_t minor;
	int      fd;
} cam_hop_t;

typedef struct cam {
	cam_hop_t hop[CAM_MAX_CHAIN]; /* [0] = sensor, [n-1] = video node */
	unsigned  nhop;

	/* negotiated format */
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t stride; /* bytesperline of the video node */
	bool     unpack; /* CR10 -> out_bytes/pixel in out[] */
	unsigned out_bytes;
	unsigned shift;

	/* streaming */
	unsigned nbuf;
	void    *map[CAM_NBUF];
	size_t   map_len[CAM_NBUF];
	uint8_t *out[CAM_NBUF];
	bool     held[CAM_NBUF]; /* app owns out[i] (unpack) / map[i] (direct) */
	bool     streaming;
	bool     got_frame;
} cam_t;

/* ------------------------------------------------------------------ */
/* Test seam: every kernel call funnels through these pointers so a    */
/* test can script the whole negotiation/streaming path without real   */
/* /dev nodes.  Only tests/yocto/peripheral_camera.c (which #includes   */
/* this file) replaces them.                                           */
/* ------------------------------------------------------------------ */
static int (*g_cam_ioctl)(int fd, unsigned long req, void *arg);
static int (*g_cam_poll)(struct pollfd *p, int timeout_ms);
static void *(*g_cam_mmap)(int fd, size_t len, off_t off);
static void (*g_cam_munmap)(void *p, size_t len);
static alp_status_t (*g_cam_discover)(uint32_t camera_id, struct cam *c);

static int cam_ioctl(int fd, unsigned long req, void *arg)
{
	if (g_cam_ioctl != NULL) return g_cam_ioctl(fd, req, arg);
	int rc;
	do {
		rc = ioctl(fd, req, arg);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

static int cam_poll(struct pollfd *p, int timeout_ms)
{
	if (g_cam_poll != NULL) return g_cam_poll(p, timeout_ms);
	int rc;
	do {
		rc = poll(p, 1, timeout_ms);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

static void *cam_mmap(int fd, size_t len, off_t off)
{
	if (g_cam_mmap != NULL) return g_cam_mmap(fd, len, off);
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
	return p == MAP_FAILED ? NULL : p;
}

static void cam_munmap(void *p, size_t len)
{
	if (g_cam_munmap != NULL) {
		g_cam_munmap(p, len);
		return;
	}
	(void)munmap(p, len);
}

/* ------------------------------------------------------------------ */
/* CR10 unpack                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Unpack an RZ CRU CR10 frame.
 *
 * Six 10-bit pixels live in the low 60 bits of each little-endian
 * 64-bit word; rows start every @p stride bytes.  The caller guarantees
 * `stride >= ceil(w / 6) * 8`.  Each sample is `(v >> shift)` written as
 * one byte (@p out_bytes == 1) or one little-endian uint16 (== 2).
 */
static void cam_cr10_unpack(const uint8_t *src,
                            uint32_t       stride,
                            uint32_t       w,
                            uint32_t       h,
                            uint8_t       *dst,
                            unsigned       out_bytes,
                            unsigned       shift)
{
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *row = src + (size_t)y * stride;
		for (uint32_t x = 0; x < w;) {
			const uint8_t *wp   = row + (size_t)(x / 6u) * 8u;
			uint64_t       word = 0;
			for (unsigned b = 0; b < 8u; ++b) {
				word |= (uint64_t)wp[b] << (8u * b);
			}
			/* one 64-bit word carries up to six pixels */
			for (unsigned k = 0; k < 6u && x < w; ++k, ++x) {
				uint32_t v = (uint32_t)((word >> (k * 10u)) & 0x3FFu) >> shift;
				if (out_bytes == 1u) {
					*dst++ = (uint8_t)v;
				} else {
					*dst++ = (uint8_t)(v & 0xFFu);
					*dst++ = (uint8_t)(v >> 8);
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Pixel format table                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t mbus;
	uint32_t fourcc;
	bool     unpack; /* CR10 unpack needed */
	unsigned out_bytes;
	unsigned shift;
} cam_fmt_t;

static const cam_fmt_t k_grey8[] = {
	{ MEDIA_BUS_FMT_Y8_1X8, V4L2_PIX_FMT_GREY, false, 1u, 0u },
	{ MEDIA_BUS_FMT_Y10_1X10, CAM_FOURCC_CR10, true, 1u, 2u },
};
static const cam_fmt_t k_raw8[] = {
	{ MEDIA_BUS_FMT_SBGGR8_1X8, V4L2_PIX_FMT_SBGGR8, false, 1u, 0u },
	{ MEDIA_BUS_FMT_SGBRG8_1X8, V4L2_PIX_FMT_SGBRG8, false, 1u, 0u },
	{ MEDIA_BUS_FMT_SGRBG8_1X8, V4L2_PIX_FMT_SGRBG8, false, 1u, 0u },
	{ MEDIA_BUS_FMT_SRGGB8_1X8, V4L2_PIX_FMT_SRGGB8, false, 1u, 0u },
	{ MEDIA_BUS_FMT_Y8_1X8, V4L2_PIX_FMT_GREY, false, 1u, 0u },
};
static const cam_fmt_t k_raw10[] = {
	{ MEDIA_BUS_FMT_SBGGR10_1X10, CAM_FOURCC_CR10, true, 2u, 0u },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, CAM_FOURCC_CR10, true, 2u, 0u },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, CAM_FOURCC_CR10, true, 2u, 0u },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, CAM_FOURCC_CR10, true, 2u, 0u },
	{ MEDIA_BUS_FMT_Y10_1X10, CAM_FOURCC_CR10, true, 2u, 0u },
};

static const cam_fmt_t *cam_pick_fmt(alp_pixfmt_t pf, const uint32_t *codes, unsigned ncodes)
{
	const cam_fmt_t *tab;
	size_t           n;
	switch (pf) {
	case ALP_PIXFMT_GREY8:
		tab = k_grey8;
		n   = sizeof(k_grey8) / sizeof(k_grey8[0]);
		break;
	case ALP_PIXFMT_RAW8:
		tab = k_raw8;
		n   = sizeof(k_raw8) / sizeof(k_raw8[0]);
		break;
	case ALP_PIXFMT_RAW10:
		tab = k_raw10;
		n   = sizeof(k_raw10) / sizeof(k_raw10[0]);
		break;
	default:
		return NULL;
	}
	for (size_t i = 0; i < n; ++i) {
		for (unsigned j = 0; j < ncodes; ++j) {
			if (codes[j] == tab[i].mbus) return &tab[i];
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static bool cam_get_ctrl(int fd, uint32_t id, int64_t *v)
{
	struct v4l2_ext_control  c;
	struct v4l2_ext_controls cs;
	memset(&c, 0, sizeof(c));
	memset(&cs, 0, sizeof(cs));
	c.id        = id;
	cs.which    = V4L2_CTRL_WHICH_CUR_VAL;
	cs.count    = 1;
	cs.controls = &c;
	if (cam_ioctl(fd, VIDIOC_G_EXT_CTRLS, &cs) < 0) return false;
	/* PIXEL_RATE is INTEGER64 (value64); the blanking controls are 32-bit. */
	*v = (id == V4L2_CID_PIXEL_RATE) ? (int64_t)c.value64 : (int64_t)c.value;
	return true;
}

static bool cam_set_ctrl(int fd, uint32_t id, int32_t v)
{
	struct v4l2_ext_control  c;
	struct v4l2_ext_controls cs;
	memset(&c, 0, sizeof(c));
	memset(&cs, 0, sizeof(cs));
	c.id        = id;
	c.value     = v;
	cs.which    = V4L2_CTRL_WHICH_CUR_VAL;
	cs.count    = 1;
	cs.controls = &c;
	return cam_ioctl(fd, VIDIOC_S_EXT_CTRLS, &cs) == 0;
}

/* Settled frame rate x1000 from the sensor's current timing; 0 if the
 * sensor does not expose PIXEL_RATE/HBLANK/VBLANK. */
static uint32_t cam_read_fps_x1000(const cam_t *c)
{
	int     fd = c->hop[0].fd;
	int64_t pr, hb, vb;
	if (!cam_get_ctrl(fd, V4L2_CID_PIXEL_RATE, &pr) || !cam_get_ctrl(fd, V4L2_CID_HBLANK, &hb) ||
	    !cam_get_ctrl(fd, V4L2_CID_VBLANK, &vb)) {
		return 0u;
	}
	int64_t total = ((int64_t)c->width + hb) * ((int64_t)c->height + vb);
	if (total <= 0 || pr <= 0) return 0u;
	return (uint32_t)((pr * 1000 + total / 2) / total);
}

static alp_status_t cam_set_fps(const cam_t *c, uint8_t fps)
{
	int     fd = c->hop[0].fd;
	int64_t pr, hb;
	if (!cam_get_ctrl(fd, V4L2_CID_PIXEL_RATE, &pr) || !cam_get_ctrl(fd, V4L2_CID_HBLANK, &hb) ||
	    pr <= 0) {
		/* The sensor driver has no settable rate; keep its own. */
		fprintf(stderr, "alp_camera: sensor has no rate controls, fps request ignored\n");
		return ALP_OK;
	}
	struct v4l2_query_ext_ctrl q;
	memset(&q, 0, sizeof(q));
	q.id = V4L2_CID_VBLANK;
	if (cam_ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &q) < 0) {
		fprintf(stderr, "alp_camera: sensor has no VBLANK range, fps request ignored\n");
		return ALP_OK;
	}

	int64_t line = (int64_t)c->width + hb;
	if (line <= 0) return ALP_ERR_IO;
	int64_t want   = pr / ((int64_t)fps * line) - (int64_t)c->height;
	int64_t vblank = want;
	if (vblank > q.maximum) vblank = q.maximum;
	if (vblank < q.minimum) vblank = q.minimum;
	if (q.step > 1u) vblank = q.minimum + (vblank - q.minimum) / (int64_t)q.step * (int64_t)q.step;
	if (vblank != want) {
		fprintf(stderr,
		        "alp_camera: %u fps is outside the sensor's range; using the nearest rate\n",
		        (unsigned)fps);
	}
	if (!cam_set_ctrl(fd, V4L2_CID_VBLANK, (int32_t)vblank)) return ALP_ERR_IO;
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Format negotiation                                                  */
/* ------------------------------------------------------------------ */

static alp_status_t cam_subdev_set_fmt(int fd, uint32_t pad, uint32_t w, uint32_t h, uint32_t code)
{
	struct v4l2_subdev_format f;
	memset(&f, 0, sizeof(f));
	f.which         = V4L2_SUBDEV_FORMAT_ACTIVE;
	f.pad           = pad;
	f.format.width  = w;
	f.format.height = h;
	f.format.code   = code;
	f.format.field  = V4L2_FIELD_NONE;
	if (cam_ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) < 0) return alp_status_from_posix_errno(errno);
	/* A subdev that silently adjusted the request did not accept it. */
	if (f.format.width != w || f.format.height != h || f.format.code != code) {
		return ALP_ERR_INVAL;
	}
	return ALP_OK;
}

static alp_status_t cam_configure(cam_t *c, const alp_camera_config_t *cfg)
{
	const cam_hop_t *sens = &c->hop[0];

	/* 1. media-bus code the sensor offers for the requested pixfmt */
	uint32_t codes[CAM_MAX_CODES];
	unsigned ncodes = 0;
	for (; ncodes < CAM_MAX_CODES; ++ncodes) {
		struct v4l2_subdev_mbus_code_enum e;
		memset(&e, 0, sizeof(e));
		e.pad   = sens->src_pad;
		e.index = ncodes;
		e.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		if (cam_ioctl(sens->fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &e) < 0) break;
		codes[ncodes] = e.code;
	}
	const cam_fmt_t *fmt = cam_pick_fmt(cfg->format, codes, ncodes);
	if (fmt == NULL) return ALP_ERR_NOSUPPORT;

	/* 2. size must be one the sensor produces natively */
	bool size_ok = false;
	for (uint32_t i = 0; !size_ok; ++i) {
		struct v4l2_subdev_frame_size_enum e;
		memset(&e, 0, sizeof(e));
		e.pad   = sens->src_pad;
		e.index = i;
		e.code  = fmt->mbus;
		e.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		if (cam_ioctl(sens->fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &e) < 0) break;
		size_ok = cfg->width >= e.min_width && cfg->width <= e.max_width &&
		          cfg->height >= e.min_height && cfg->height <= e.max_height;
	}
	if (!size_ok) return ALP_ERR_INVAL;

	/* 3. push the format down the chain: sensor src, then each
	 *    intermediate subdev's sink + src pad */
	alp_status_t rc =
	    cam_subdev_set_fmt(sens->fd, sens->src_pad, cfg->width, cfg->height, fmt->mbus);
	if (rc != ALP_OK) return rc;
	for (unsigned i = 1; i + 1 < c->nhop; ++i) {
		rc = cam_subdev_set_fmt(
		    c->hop[i].fd, c->hop[i].sink_pad, cfg->width, cfg->height, fmt->mbus);
		if (rc != ALP_OK) return rc;
		rc =
		    cam_subdev_set_fmt(c->hop[i].fd, c->hop[i].src_pad, cfg->width, cfg->height, fmt->mbus);
		if (rc != ALP_OK) return rc;
	}

	/* 4. capture node */
	struct v4l2_format vf;
	memset(&vf, 0, sizeof(vf));
	vf.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vf.fmt.pix.width       = cfg->width;
	vf.fmt.pix.height      = cfg->height;
	vf.fmt.pix.pixelformat = fmt->fourcc;
	vf.fmt.pix.field       = V4L2_FIELD_NONE;
	if (cam_ioctl(c->hop[c->nhop - 1u].fd, VIDIOC_S_FMT, &vf) < 0) {
		return alp_status_from_posix_errno(errno);
	}
	if (vf.fmt.pix.pixelformat != fmt->fourcc || vf.fmt.pix.width != cfg->width ||
	    vf.fmt.pix.height != cfg->height) {
		return ALP_ERR_INVAL;
	}
	c->width     = cfg->width;
	c->height    = cfg->height;
	c->fourcc    = fmt->fourcc;
	c->stride    = vf.fmt.pix.bytesperline;
	c->unpack    = fmt->unpack;
	c->out_bytes = fmt->out_bytes;
	c->shift     = fmt->shift;
	if (c->unpack && c->stride < ((cfg->width + 5u) / 6u) * 8u) return ALP_ERR_IO;

	/* 5. frame rate (timing controls depend on the mode just set) */
	if (cfg->fps != 0u) {
		rc = cam_set_fps(c, cfg->fps);
		if (rc != ALP_OK) return rc;
	}
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

static void cam_free_buffers(cam_t *c)
{
	for (unsigned i = 0; i < CAM_NBUF; ++i) {
		if (c->map[i] != NULL) cam_munmap(c->map[i], c->map_len[i]);
		c->map[i] = NULL;
		free(c->out[i]);
		c->out[i] = NULL;
	}
	c->nbuf = 0;
}

static alp_status_t cam_alloc_buffers(cam_t *c)
{
	int fd = c->hop[c->nhop - 1u].fd;

	struct v4l2_requestbuffers rb;
	memset(&rb, 0, sizeof(rb));
	rb.count  = CAM_NBUF;
	rb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (cam_ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) return alp_status_from_posix_errno(errno);
	if (rb.count < 2u) return ALP_ERR_NOMEM;
	if (rb.count > CAM_NBUF) rb.count = CAM_NBUF;

	for (unsigned i = 0; i < rb.count; ++i) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index  = i;
		if (cam_ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return alp_status_from_posix_errno(errno);
		if (c->unpack && (size_t)b.length < (size_t)c->stride * c->height) return ALP_ERR_IO;
		c->map[i] = cam_mmap(fd, b.length, (off_t)b.m.offset);
		if (c->map[i] == NULL) return ALP_ERR_NOMEM;
		c->map_len[i] = b.length;
		c->nbuf       = i + 1u;
		if (c->unpack) {
			c->out[i] = malloc((size_t)c->width * c->height * c->out_bytes);
			if (c->out[i] == NULL) return ALP_ERR_NOMEM;
		}
	}
	return ALP_OK;
}

static alp_status_t cam_qbuf(cam_t *c, unsigned i)
{
	struct v4l2_buffer b;
	memset(&b, 0, sizeof(b));
	b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index  = i;
	if (cam_ioctl(c->hop[c->nhop - 1u].fd, VIDIOC_QBUF, &b) < 0) return ALP_ERR_IO;
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* Media-controller discovery                                          */
/* ------------------------------------------------------------------ */

typedef struct {
	struct media_v2_entity    *ent;
	struct media_v2_interface *ifc;
	struct media_v2_pad       *pad;
	struct media_v2_link      *lnk;
	uint32_t                   nent, nifc, npad, nlnk;
} cam_topo_t;

static const struct media_v2_pad *cam_find_pad(const cam_topo_t *t, uint32_t id)
{
	for (uint32_t i = 0; i < t->npad; ++i) {
		if (t->pad[i].id == id) return &t->pad[i];
	}
	return NULL;
}

static const struct media_v2_entity *cam_find_ent(const cam_topo_t *t, uint32_t id)
{
	for (uint32_t i = 0; i < t->nent; ++i) {
		if (t->ent[i].id == id) return &t->ent[i];
	}
	return NULL;
}

/* Interface (device node) major/minor of entity @p ent_id. */
static bool cam_ent_devnode(const cam_topo_t *t, uint32_t ent_id, uint32_t *maj, uint32_t *min)
{
	for (uint32_t i = 0; i < t->nlnk; ++i) {
		if ((t->lnk[i].flags & CAM_LNK_TYPE_MASK) != CAM_LNK_TYPE_INTERFACE ||
		    t->lnk[i].sink_id != ent_id) {
			continue;
		}
		for (uint32_t j = 0; j < t->nifc; ++j) {
			if (t->ifc[j].id == t->lnk[i].source_id) {
				*maj = t->ifc[j].devnode.major;
				*min = t->ifc[j].devnode.minor;
				return true;
			}
		}
	}
	return false;
}

/**
 * @brief Follow enabled data links from the sensor to the IO_V4L entity.
 *
 * Fills @p c->hop[] (sensor first, capture node last) with entity ids,
 * the pad indices on each side, and each hop's device-node numbers.
 */
static alp_status_t cam_walk(const cam_topo_t *t, uint32_t sensor_ent, cam_t *c)
{
	uint32_t cur       = sensor_ent;
	uint32_t next_sink = 0;
	c->nhop            = 0;
	for (;;) {
		if (c->nhop >= CAM_MAX_CHAIN) return ALP_ERR_NOT_READY;
		cam_hop_t *h = &c->hop[c->nhop];
		memset(h, 0, sizeof(*h));
		h->fd                           = -1;
		h->ent_id                       = cur;
		h->sink_pad                     = next_sink;
		const struct media_v2_entity *e = cam_find_ent(t, cur);
		if (e == NULL) return ALP_ERR_NOT_READY;
		h->function = e->function;
		if (!cam_ent_devnode(t, cur, &h->major, &h->minor)) return ALP_ERR_NOT_READY;
		++c->nhop;
		if (e->function == MEDIA_ENT_F_IO_V4L) return ALP_OK;

		bool found = false;
		for (uint32_t i = 0; i < t->nlnk && !found; ++i) {
			const struct media_v2_link *l = &t->lnk[i];
			if ((l->flags & CAM_LNK_TYPE_MASK) == CAM_LNK_TYPE_INTERFACE ||
			    !(l->flags & MEDIA_LNK_FL_ENABLED)) {
				continue;
			}
			const struct media_v2_pad *sp = cam_find_pad(t, l->source_id);
			const struct media_v2_pad *kp = cam_find_pad(t, l->sink_id);
			if (sp == NULL || kp == NULL || sp->entity_id != cur) continue;
			h->src_pad = sp->index;
			next_sink  = kp->index;
			cur        = kp->entity_id;
			found      = true;
		}
		if (!found) return ALP_ERR_NOT_READY;
	}
}

static int cam_open_char(uint32_t maj, uint32_t min, int flags)
{
	char path[96];
	char buf[256];
	(void)snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent", maj, min);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, sizeof(buf) - 1u);
	close(fd);
	if (n <= 0) return -1;
	buf[n]         = '\0';
	const char *dn = strstr(buf, "DEVNAME=");
	if (dn == NULL) return -1;
	dn += 8;
	size_t len = strcspn(dn, "\n");
	char   node[128];
	if (len == 0 || len + 6u >= sizeof(node)) return -1;
	(void)snprintf(node, sizeof(node), "/dev/%.*s", (int)len, dn);
	return open(node, flags | O_CLOEXEC);
}

/* True when the subdev (maj:min) hangs off the DT node @p want_real. */
static bool cam_sensor_matches(uint32_t maj, uint32_t min, const char *want_real)
{
	char path[128];
	char got[PATH_MAX];
	(void)snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/of_node", maj, min);
	if (realpath(path, got) == NULL) return false;
	return strcmp(got, want_real) == 0;
}

static bool cam_read_topology(int mfd, cam_topo_t *t, void **mem)
{
	struct media_v2_topology top;
	for (int attempt = 0; attempt < 3; ++attempt) {
		memset(&top, 0, sizeof(top));
		if (cam_ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &top) < 0) return false;
		uint64_t ver = top.topology_version;
		size_t   sz  = (size_t)top.num_entities * sizeof(*t->ent) +
		               (size_t)top.num_interfaces * sizeof(*t->ifc) +
		               (size_t)top.num_pads * sizeof(*t->pad) +
		               (size_t)top.num_links * sizeof(*t->lnk);
		void    *m   = calloc(1, sz != 0 ? sz : 1u);
		if (m == NULL) return false;
		t->ent             = m;
		t->ifc             = (void *)(t->ent + top.num_entities);
		t->pad             = (void *)(t->ifc + top.num_interfaces);
		t->lnk             = (void *)(t->pad + top.num_pads);
		top.ptr_entities   = (uintptr_t)t->ent;
		top.ptr_interfaces = (uintptr_t)t->ifc;
		top.ptr_pads       = (uintptr_t)t->pad;
		top.ptr_links      = (uintptr_t)t->lnk;
		if (cam_ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &top) == 0 && top.topology_version == ver) {
			t->nent = top.num_entities;
			t->nifc = top.num_interfaces;
			t->npad = top.num_pads;
			t->nlnk = top.num_links;
			*mem    = m;
			return true;
		}
		free(m); /* topology changed between the two calls: retry */
	}
	return false;
}

static void cam_close_hops(cam_t *c)
{
	for (unsigned i = 0; i < c->nhop; ++i) {
		if (c->hop[i].fd >= 0) close(c->hop[i].fd);
		c->hop[i].fd = -1;
	}
}

/* Resolve camera_id -> opened hop fds in @p c (NOT_READY when absent). */
static alp_status_t cam_discover(uint32_t camera_id, cam_t *c)
{
	char alias[96];
	char dtpath[PATH_MAX];
	char want_in[PATH_MAX + 32];
	char want[PATH_MAX];

	(void)snprintf(alias, sizeof(alias), "/proc/device-tree/aliases/alp-camera%u", camera_id);
	int afd = open(alias, O_RDONLY | O_CLOEXEC);
	if (afd < 0) return ALP_ERR_NOT_READY;
	ssize_t n = read(afd, dtpath, sizeof(dtpath) - 1u);
	close(afd);
	if (n <= 0) return ALP_ERR_NOT_READY;
	dtpath[n] = '\0';
	(void)snprintf(want_in, sizeof(want_in), "/proc/device-tree%s", dtpath);
	if (realpath(want_in, want) == NULL) return ALP_ERR_NOT_READY;

	for (int m = 0; m < CAM_MAX_MEDIA; ++m) {
		char mp[32];
		(void)snprintf(mp, sizeof(mp), "/dev/media%d", m);
		int mfd = open(mp, O_RDWR | O_CLOEXEC);
		if (mfd < 0) continue;

		cam_topo_t t;
		void      *mem = NULL;
		memset(&t, 0, sizeof(t));
		if (!cam_read_topology(mfd, &t, &mem)) {
			close(mfd);
			continue;
		}
		close(mfd);

		for (uint32_t i = 0; i < t.nent; ++i) {
			uint32_t maj, min;
			if (t.ent[i].function != MEDIA_ENT_F_CAM_SENSOR ||
			    !cam_ent_devnode(&t, t.ent[i].id, &maj, &min) ||
			    !cam_sensor_matches(maj, min, want)) {
				continue;
			}
			alp_status_t rc = cam_walk(&t, t.ent[i].id, c);
			free(mem);
			if (rc != ALP_OK) return rc;
			for (unsigned h = 0; h < c->nhop; ++h) {
				bool last    = (h + 1u == c->nhop);
				c->hop[h].fd = cam_open_char(
				    c->hop[h].major, c->hop[h].minor, last ? (O_RDWR | O_NONBLOCK) : O_RDWR);
				if (c->hop[h].fd < 0) {
					cam_close_hops(c);
					return ALP_ERR_NOT_READY;
				}
			}
			return ALP_OK;
		}
		free(mem);
	}
	return ALP_ERR_NOT_READY;
}

/* ------------------------------------------------------------------ */
/* Ops                                                                 */
/* ------------------------------------------------------------------ */

#include "yocto_isp_capture.h"

static void y_close(alp_camera_backend_state_t *st);

static alp_status_t
y_open(const alp_camera_config_t *cfg, alp_camera_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)caps_out;
	if (cfg->width == 0u || cfg->height == 0u) return ALP_ERR_INVAL;
	/* ISP stack present and the format is one it produces: serve it from
	 * the ISP node.  NOSUPPORT = not the ISP path, fall through. */
	alp_status_t isp_rc = isp_try_open(cfg, st);
	if (isp_rc != ALP_ERR_NOSUPPORT) return isp_rc;
	cam_t *c = calloc(1, sizeof(*c));
	if (c == NULL) return ALP_ERR_NOMEM;
	st->be_data = c;

	alp_status_t rc = (g_cam_discover != NULL ? g_cam_discover : cam_discover)(cfg->camera_id, c);
	if (rc == ALP_OK) rc = cam_configure(c, cfg);
	if (rc == ALP_OK) rc = cam_alloc_buffers(c);
	if (rc != ALP_OK) {
		y_close(st);
		return rc;
	}
	st->fps_x1000 = cam_read_fps_x1000(c);
	return ALP_OK;
}

static alp_status_t y_start(alp_camera_backend_state_t *st)
{
	cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	if (c->streaming) return ALP_OK;
	int          fd   = c->hop[c->nhop - 1u].fd;
	int          type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	alp_status_t rc   = ALP_OK;
	for (unsigned i = 0; i < c->nbuf && rc == ALP_OK; ++i) {
		/* Unpacked frames live in out[]; the V4L2 buffer is always ours. */
		if (!c->unpack && c->held[i]) continue; /* app still owns it until release() */
		rc = cam_qbuf(c, i);
	}
	if (rc == ALP_OK && cam_ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		rc = alp_status_from_posix_errno(errno);
	}
	if (rc != ALP_OK) {
		/* Drop whatever was queued so a retry starts from an empty queue. */
		(void)cam_ioctl(fd, VIDIOC_STREAMOFF, &type);
		return rc;
	}
	c->streaming = true;
	c->got_frame = false;
	return ALP_OK;
}

static alp_status_t y_stop(alp_camera_backend_state_t *st)
{
	cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	if (!c->streaming) return ALP_OK;
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (cam_ioctl(c->hop[c->nhop - 1u].fd, VIDIOC_STREAMOFF, &type) < 0) return ALP_ERR_IO;
	c->streaming = false;
	return ALP_OK;
}

static alp_status_t
y_capture(alp_camera_backend_state_t *st, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	cam_t *c = st->be_data;
	if (c == NULL || !c->streaming) return ALP_ERR_NOT_READY;
	int fd = c->hop[c->nhop - 1u].fd;

	/* Unpacked frames are decoded into an app-owned out[] slot, independent
	 * of the V4L2 buffer index, so a held frame is never overwritten. */
	unsigned slot = 0;
	if (c->unpack) {
		while (slot < c->nbuf && c->held[slot])
			++slot;
		if (slot == c->nbuf) return ALP_ERR_BUSY; /* release() a frame first */
	}

	struct pollfd p  = { .fd = fd, .events = POLLIN };
	int           to = timeout_ms > (uint32_t)INT_MAX ? -1 : (int)timeout_ms;
	int           pr = cam_poll(&p, to);
	if (pr < 0) return ALP_ERR_IO;
	if (pr == 0) {
		if (!c->got_frame) {
			fprintf(stderr,
			        "alp_camera: no first frame within %u ms; check the CSI-2 lane "
			        "polarity/mux and the sensor's stream state\n",
			        (unsigned)timeout_ms);
		}
		return ALP_ERR_TIMEOUT;
	}
	if (p.revents & (POLLERR | POLLNVAL)) return ALP_ERR_IO;

	struct v4l2_buffer b;
	memset(&b, 0, sizeof(b));
	b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	if (cam_ioctl(fd, VIDIOC_DQBUF, &b) < 0) {
		return errno == EAGAIN ? ALP_ERR_TIMEOUT : ALP_ERR_IO;
	}
	if (b.index >= c->nbuf) return ALP_ERR_IO;
	c->got_frame = true;

	/* A frame the CRU flagged corrupt, or one shorter than negotiated
	 * (bytesused 0 = driver does not report), is never handed out. */
	size_t need = (size_t)c->stride * c->height;
	if ((b.flags & V4L2_BUF_FLAG_ERROR) != 0u || (b.bytesused != 0u && b.bytesused < need)) {
		fprintf(stderr, "alp_camera: dropped a corrupt or short frame\n");
		(void)cam_qbuf(c, b.index);
		return ALP_ERR_IO;
	}

	if (c->unpack) {
		cam_cr10_unpack(
		    c->map[b.index], c->stride, c->width, c->height, c->out[slot], c->out_bytes, c->shift);
		/* The V4L2 buffer is free again as soon as it is unpacked. */
		alp_status_t qrc = cam_qbuf(c, b.index);
		if (qrc != ALP_OK) return qrc;
		c->held[slot] = true;
		out->data     = c->out[slot];
		out->size     = (size_t)c->width * c->height * c->out_bytes;
	} else {
		c->held[b.index] = true;
		out->data        = c->map[b.index];
		out->size        = b.bytesused != 0u ? b.bytesused : c->map_len[b.index];
	}
	out->timestamp_us = (uint64_t)b.timestamp.tv_sec * 1000000u + (uint64_t)b.timestamp.tv_usec;
	return ALP_OK;
}

static alp_status_t y_release(alp_camera_backend_state_t *st, alp_camera_frame_t *frame)
{
	cam_t *c = st->be_data;
	if (c == NULL) return ALP_ERR_NOT_READY;
	for (unsigned i = 0; i < c->nbuf; ++i) {
		void *mine = c->unpack ? (void *)c->out[i] : c->map[i];
		if (frame->data != mine || !c->held[i]) continue;
		c->held[i] = false;
		/* Unpacked: the V4L2 buffer was requeued at capture.  Direct: it is
		 * still dequeued; after stop() start() re-queues it. */
		return (!c->unpack && c->streaming) ? cam_qbuf(c, i) : ALP_OK;
	}
	return ALP_ERR_INVAL;
}

static alp_status_t y_configure_isp(alp_camera_backend_state_t    *st,
                                    const alp_camera_isp_config_t *isp)
{
	(void)st;
	(void)isp;
	return ALP_ERR_NOSUPPORT;
}

static void y_close(alp_camera_backend_state_t *st)
{
	cam_t *c = st->be_data;
	if (c == NULL) return;
	if (c->streaming) (void)y_stop(st);
	cam_free_buffers(c);
	cam_close_hops(c);
	free(c);
	st->be_data = NULL;
}

static const alp_camera_ops_t _ops = {
	.open          = y_open,
	.start         = y_start,
	.stop          = y_stop,
	.capture       = y_capture,
	.release       = y_release,
	.configure_isp = y_configure_isp,
	.close         = y_close,
};

ALP_BACKEND_REGISTER(camera,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
