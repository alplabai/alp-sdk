/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit coverage for the Linux V4L2/media-controller camera backend
 * (src/backends/camera/yocto_drv.c): discovery failure, media-graph walk,
 * format negotiation along the chain, frame-rate programming, the CR10
 * unpacker, and the capture/release/timeout path.
 *
 * The real backend .c is #included (same technique as peripheral_pwm.c)
 * so every kernel call can be scripted through its g_cam_ioctl / g_cam_poll
 * / g_cam_mmap seam -- no /dev/media* or /dev/video* is touched.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_camera
 *   ctest --test-dir build -R alp_test_peripheral_camera
 */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/camera/yocto_drv.c"

#define FD_SENSOR 10
#define FD_CSI    11
#define FD_VIDEO  12

/* ---- scripted kernel -------------------------------------------- */

static uint32_t g_sensor_codes[4]; /* media-bus codes the sensor offers */
static unsigned g_nsensor_codes;
static int      g_sfmt_calls; /* VIDIOC_SUBDEV_S_FMT count */
static int      g_sfmt_fd[8];
static uint32_t g_sfmt_pad[8];
static uint32_t g_video_fourcc; /* what the video S_FMT saw */
static int      g_qbuf, g_streamon, g_streamoff;
static int64_t  g_pixel_rate = 100000000;
static int64_t  g_hblank     = 400;
static int64_t  g_vblank     = 100;
static int      g_poll_ret;
static uint32_t g_dq_index;
static void    *g_maps[CAM_NBUF];
static unsigned g_nmap;
static int      g_munmap_calls;
static bool     g_sfmt_adjust;     /* subdev "adjusts" the requested size */
static bool     g_query_fail;      /* VBLANK query fails */
static int64_t  g_vb_step;         /* VBLANK step */
static uint32_t g_dq_flags;        /* flags on the dequeued buffer */
static uint32_t g_dq_bytesused;    /* bytesused on the dequeued buffer (0 = unreported) */
static int      g_qbuf_fail_after; /* QBUF fails once this many succeeded (-1 = never) */
static bool     g_streamon_fail;
static int      g_hop_fd[3];

static int fake_ioctl(int fd, unsigned long req, void *arg)
{
	switch (req) {
	case VIDIOC_SUBDEV_ENUM_MBUS_CODE: {
		struct v4l2_subdev_mbus_code_enum *e = arg;
		if (e->index >= g_nsensor_codes) {
			errno = EINVAL;
			return -1;
		}
		e->code = g_sensor_codes[e->index];
		return 0;
	}
	case VIDIOC_SUBDEV_ENUM_FRAME_SIZE: {
		struct v4l2_subdev_frame_size_enum *e = arg;
		if (e->index != 0u) {
			errno = EINVAL;
			return -1;
		}
		e->min_width  = 4;
		e->min_height = 4;
		e->max_width  = 1280;
		e->max_height = 800;
		return 0;
	}
	case VIDIOC_SUBDEV_S_FMT: {
		struct v4l2_subdev_format *f = arg;
		if (g_sfmt_calls < 8) {
			g_sfmt_fd[g_sfmt_calls]  = fd;
			g_sfmt_pad[g_sfmt_calls] = f->pad;
		}
		++g_sfmt_calls;
		if (g_sfmt_adjust) f->format.width -= 2u; /* silently adjusted */
		return 0;
	}
	case VIDIOC_S_FMT: {
		struct v4l2_format *f   = arg;
		g_video_fourcc          = f->fmt.pix.pixelformat;
		f->fmt.pix.bytesperline = f->fmt.pix.width * 8u; /* CRU reports width*8 */
		f->fmt.pix.sizeimage    = f->fmt.pix.bytesperline * f->fmt.pix.height;
		return 0;
	}
	case VIDIOC_G_EXT_CTRLS: {
		struct v4l2_ext_controls *cs = arg;
		struct v4l2_ext_control  *c  = cs->controls;
		if (c->id == V4L2_CID_PIXEL_RATE) {
			c->value64 = g_pixel_rate;
		} else if (c->id == V4L2_CID_HBLANK) {
			c->value = (int32_t)g_hblank;
		} else if (c->id == V4L2_CID_VBLANK) {
			c->value = (int32_t)g_vblank;
		} else {
			return -1;
		}
		return 0;
	}
	case VIDIOC_S_EXT_CTRLS: {
		struct v4l2_ext_controls *cs = arg;
		if (cs->controls->id == V4L2_CID_VBLANK) g_vblank = cs->controls->value;
		return 0;
	}
	case VIDIOC_QUERY_EXT_CTRL: {
		struct v4l2_query_ext_ctrl *q = arg;
		if (g_query_fail) {
			errno = EINVAL;
			return -1;
		}
		q->minimum = 10;
		q->maximum = 5000;
		q->step    = (uint64_t)g_vb_step;
		return 0;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		r->count                      = CAM_NBUF;
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		b->length             = 96u * 4u; /* stride 12*8 x height 4 */
		b->m.offset           = b->index * 4096u;
		return 0;
	}
	case VIDIOC_QBUF:
		if (g_qbuf_fail_after >= 0 && g_qbuf >= g_qbuf_fail_after) {
			errno = EINVAL;
			return -1;
		}
		++g_qbuf;
		return 0;
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b = arg;
		b->index              = g_dq_index;
		b->timestamp.tv_sec   = 5;
		b->timestamp.tv_usec  = 250000;
		b->flags              = g_dq_flags;
		b->bytesused          = g_dq_bytesused;
		return 0;
	}
	case VIDIOC_STREAMON:
		if (g_streamon_fail) {
			errno = EIO;
			return -1;
		}
		++g_streamon;
		return 0;
	case VIDIOC_STREAMOFF:
		++g_streamoff;
		return 0;
	default:
		errno = ENOTTY;
		return -1;
	}
}

static int fake_poll(struct pollfd *p, int timeout_ms)
{
	(void)timeout_ms;
	p->revents = g_poll_ret > 0 ? POLLIN : 0;
	return g_poll_ret;
}

static void *fake_mmap(int fd, size_t len, off_t off)
{
	(void)fd;
	(void)off;
	g_maps[g_nmap] = calloc(1, len);
	return g_maps[g_nmap++];
}

static void fake_munmap(void *p, size_t len)
{
	(void)len;
	++g_munmap_calls;
	free(p);
}

/* No ISP node: the raw media-controller path under test must not depend on
 * whether the machine running the test has /dev/video0fr. */
static int fake_no_isp(uint32_t camera_id)
{
	(void)camera_id;
	errno = ENOENT;
	return -1;
}

static void reset(void)
{
	g_nsensor_codes = 0;
	g_sfmt_calls    = 0;
	g_video_fourcc  = 0;
	g_qbuf = g_streamon = g_streamoff = 0;
	g_pixel_rate                      = 100000000;
	g_hblank                          = 400;
	g_vblank                          = 100;
	g_nmap                            = 0;
	g_munmap_calls                    = 0;
	g_sfmt_adjust                     = false;
	g_query_fail                      = false;
	g_vb_step                         = 1;
	g_dq_flags                        = 0;
	g_dq_bytesused                    = 0;
	g_qbuf_fail_after                 = -1;
	g_streamon_fail                   = false;
	g_cam_discover                    = NULL;
	g_cam_ioctl                       = fake_ioctl;
	g_cam_poll                        = fake_poll;
	g_cam_mmap                        = fake_mmap;
	g_cam_munmap                      = fake_munmap;
	g_cam_isp_open                    = fake_no_isp;
}

static void make_chain(cam_t *c)
{
	memset(c, 0, sizeof(*c));
	c->nhop            = 3;
	c->hop[0].fd       = FD_SENSOR;
	c->hop[0].src_pad  = 0;
	c->hop[1].fd       = FD_CSI;
	c->hop[1].sink_pad = 0;
	c->hop[1].src_pad  = 1;
	c->hop[2].fd       = FD_VIDEO;
	c->hop[2].function = MEDIA_ENT_F_IO_V4L;
}

/* Discovery seam: a three-hop chain on real (closable) /dev/null fds. */
static alp_status_t fake_discover(uint32_t camera_id, cam_t *c)
{
	(void)camera_id;
	make_chain(c);
	for (unsigned i = 0; i < 3u; ++i) {
		g_hop_fd[i]  = open("/dev/null", O_RDWR | O_CLOEXEC);
		c->hop[i].fd = g_hop_fd[i];
	}
	return ALP_OK;
}

static alp_camera_config_t cfg_of(alp_pixfmt_t f, uint16_t w, uint16_t h, uint8_t fps)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);
	cfg.width               = w;
	cfg.height              = h;
	cfg.fps                 = fps;
	cfg.format              = f;
	return cfg;
}

/* ---- tests -------------------------------------------------------- */

static void test_missing_alias_is_not_ready(void)
{
	reset();
	alp_camera_config_t        cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	cfg.camera_id = 987654u; /* no such DT alias */
	ALP_ASSERT_EQ_INT(y_open(&cfg, &st, NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_TRUE(st.be_data == NULL);
}

static void test_negotiation_y10_grey8(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);

	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(c.fourcc, CAM_FOURCC_CR10);
	ALP_ASSERT_EQ_INT(g_video_fourcc, CAM_FOURCC_CR10);
	ALP_ASSERT_TRUE(c.unpack);
	ALP_ASSERT_EQ_INT(c.out_bytes, 1);
	ALP_ASSERT_EQ_INT(c.shift, 2);
	ALP_ASSERT_EQ_INT(c.stride, 96);
	/* sensor src, csi sink, csi src -- in that order */
	ALP_ASSERT_EQ_INT(g_sfmt_calls, 3);
	ALP_ASSERT_EQ_INT(g_sfmt_fd[0], FD_SENSOR);
	ALP_ASSERT_EQ_INT(g_sfmt_fd[1], FD_CSI);
	ALP_ASSERT_EQ_INT(g_sfmt_pad[1], 0);
	ALP_ASSERT_EQ_INT(g_sfmt_pad[2], 1);
}

static void test_negotiation_other_pixfmts(void)
{
	reset();
	cam_t c;
	make_chain(&c);

	/* Y8 sensor: GREY8 goes straight through, no unpack. */
	g_sensor_codes[0]       = MEDIA_BUS_FMT_Y8_1X8;
	g_nsensor_codes         = 1;
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(c.fourcc, V4L2_PIX_FMT_GREY);
	ALP_ASSERT_TRUE(!c.unpack);

	/* Bayer10 sensor: RAW10 unpacks to uint16. */
	g_sensor_codes[0] = MEDIA_BUS_FMT_SGRBG10_1X10;
	cfg               = cfg_of(ALP_PIXFMT_RAW10, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_TRUE(c.unpack);
	ALP_ASSERT_EQ_INT(c.out_bytes, 2);
	ALP_ASSERT_EQ_INT(c.shift, 0);

	/* A size the sensor does not produce is INVAL (no scaler). */
	cfg = cfg_of(ALP_PIXFMT_RAW10, 2000, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_ERR_INVAL);

	/* Colour formats are not produced here. */
	cfg = cfg_of(ALP_PIXFMT_RGB565, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_ERR_NOSUPPORT);

	/* GREY8 against a Bayer-only sensor: nothing to offer. */
	cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_ERR_NOSUPPORT);
}

static void test_fps(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);

	/* 10 MHz / ((12+400) * (4+vblank)) = 30 fps */
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 30);
	g_pixel_rate            = 10000000;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(g_vblank, 10000000 / (30 * 412) - 4);
	uint32_t fps = cam_read_fps_x1000(&c);
	ALP_ASSERT_TRUE(fps > 29900u && fps < 30300u);

	/* VBLANK step 8: rounded down onto min + n*step (10 + 8n). */
	g_vb_step = 8;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(g_vblank, 802);
	g_vb_step = 1;

	/* 1 MHz pixel clock cannot reach 255 fps: vblank clamps up to the
	 * minimum (10) and open still succeeds at the nearest rate. */
	cfg          = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 255);
	g_pixel_rate = 1000000;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(g_vblank, 10);
	fps = cam_read_fps_x1000(&c); /* 1e6 / (412 * 14) */
	ALP_ASSERT_TRUE(fps > 173000u && fps < 173600u);

	/* 1 fps is below what the sensor can slow to: clamps to the maximum. */
	cfg          = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 1);
	g_pixel_rate = 100000000;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(g_vblank, 5000);

	/* No VBLANK range: request ignored (logged), open still OK. */
	g_query_fail = true;
	g_vblank     = 100;
	cfg          = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 30);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(g_vblank, 100);
}

/* A subdev that silently adjusts the format did not accept it. */
static void test_subdev_adjust_is_inval(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	g_sfmt_adjust           = true;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_ERR_INVAL);
	g_sfmt_adjust = false;
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
}

/* Pack @p v (10-bit) pixel x of a row into the 6-per-64-bit-word layout. */
static void pack_px(uint8_t *row, uint32_t x, uint32_t v)
{
	uint8_t *wp   = row + (x / 6u) * 8u;
	unsigned bit0 = (x % 6u) * 10u;
	for (unsigned b = 0; b < 10u; ++b) {
		if ((v >> b) & 1u) wp[(bit0 + b) / 8u] |= (uint8_t)(1u << ((bit0 + b) % 8u));
	}
}

static void test_cr10_unpack(void)
{
	enum { W = 13, H = 2, STRIDE = 24 }; /* 13 px -> 3 words per row */
	uint8_t  src[STRIDE * H];
	uint32_t val[W * H];
	memset(src, 0, sizeof(src));
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			val[y * W + x] = (x * 83u + y * 301u + 5u) & 0x3FFu;
			pack_px(src + y * STRIDE, x, val[y * W + x]);
		}
	}

	uint8_t g8[W * H];
	cam_cr10_unpack(src, STRIDE, W, H, g8, 1, 2);
	bool ok8 = true;
	for (uint32_t i = 0; i < W * H; ++i)
		ok8 = ok8 && g8[i] == (val[i] >> 2);
	ALP_ASSERT_TRUE(ok8);

	uint8_t r10[W * H * 2];
	cam_cr10_unpack(src, STRIDE, W, H, r10, 2, 0);
	bool ok16 = true;
	for (uint32_t i = 0; i < W * H; ++i) {
		ok16 = ok16 && (uint32_t)(r10[2 * i] | (r10[2 * i + 1] << 8)) == val[i];
	}
	ALP_ASSERT_TRUE(ok16);
}

static void test_capture_release_and_timeout(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(cam_alloc_buffers(&c), ALP_OK);
	ALP_ASSERT_EQ_INT(c.nbuf, CAM_NBUF);

	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	st.be_data = &c;
	alp_camera_frame_t fr;

	/* capture before start -> NOT_READY */
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_ERR_NOT_READY);

	ALP_ASSERT_EQ_INT(y_start(&st), ALP_OK);
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_OK); /* idempotent */
	ALP_ASSERT_EQ_INT(g_qbuf, CAM_NBUF);
	ALP_ASSERT_EQ_INT(g_streamon, 1);

	/* nothing arrives: TIMEOUT (first-frame hint goes to stderr) */
	g_poll_ret = 0;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_ERR_TIMEOUT);

	/* a frame in buffer 2: pixel 7 of row 1 = 0x3FF -> 255 after >>2 */
	pack_px((uint8_t *)g_maps[2] + 96, 7, 0x3FFu);
	g_poll_ret = 1;
	g_dq_index = 2;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_OK);
	ALP_ASSERT_EQ_INT(fr.size, 12 * 4);
	ALP_ASSERT_EQ_INT(((uint8_t *)fr.data)[12 + 7], 255);
	ALP_ASSERT_EQ_INT(fr.timestamp_us, 5250000);

	/* the unpacked frame's V4L2 buffer was requeued at capture time */
	int q = g_qbuf;
	ALP_ASSERT_EQ_INT(y_release(&st, &fr), ALP_OK);
	ALP_ASSERT_EQ_INT(g_qbuf, q);
	ALP_ASSERT_EQ_INT(y_release(&st, &fr), ALP_ERR_INVAL); /* double release */

	ALP_ASSERT_EQ_INT(y_stop(&st), ALP_OK);
	ALP_ASSERT_EQ_INT(y_stop(&st), ALP_OK); /* idempotent */
	ALP_ASSERT_EQ_INT(g_streamoff, 1);

	cam_free_buffers(&c);
}

/* Unpacked frames requeue the V4L2 buffer at capture; a held frame is
 * never overwritten; corrupt/short frames are requeued and rejected. */
static void test_capture_integrity(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(cam_alloc_buffers(&c), ALP_OK);
	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	st.be_data = &c;
	alp_camera_frame_t fr;
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_OK);
	g_poll_ret = 1;
	g_dq_index = 1;

	/* corrupt frame: requeued, IO, nothing held */
	int q      = g_qbuf;
	g_dq_flags = V4L2_BUF_FLAG_ERROR;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(g_qbuf, q + 1);
	ALP_ASSERT_TRUE(!c.held[0] && !c.held[1]);

	/* short frame (bytesused < stride*height = 384) */
	g_dq_flags     = 0;
	g_dq_bytesused = 100;
	q              = g_qbuf;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(g_qbuf, q + 1);

	/* good frame: V4L2 buffer requeued right away, app holds slot 0 */
	g_dq_bytesused = 96u * 4u;
	q              = g_qbuf;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_OK);
	ALP_ASSERT_EQ_INT(g_qbuf, q + 1);
	void *first = fr.data;
	ALP_ASSERT_TRUE(first == c.out[0]);

	/* the same V4L2 index again must land in a different out[] slot */
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_OK);
	ALP_ASSERT_TRUE(fr.data == c.out[1] && fr.data != first);

	/* release returns the slot without another QBUF */
	q = g_qbuf;
	ALP_ASSERT_EQ_INT(y_release(&st, &fr), ALP_OK);
	ALP_ASSERT_EQ_INT(g_qbuf, q);

	/* all slots held -> BUSY */
	c.held[1] = c.held[2] = c.held[3] = true;
	ALP_ASSERT_EQ_INT(y_capture(&st, &fr, 10), ALP_ERR_BUSY);

	c.streaming = false;
	cam_free_buffers(&c);
}

/* A failed start() leaves the queue empty so a retry works. */
static void test_start_failure_resets_queue(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	cam_t c;
	make_chain(&c);
	alp_camera_config_t cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 0);
	ALP_ASSERT_EQ_INT(cam_configure(&c, &cfg), ALP_OK);
	ALP_ASSERT_EQ_INT(cam_alloc_buffers(&c), ALP_OK);
	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	st.be_data = &c;

	g_qbuf_fail_after = 2; /* third QBUF fails */
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_ERR_IO);
	ALP_ASSERT_TRUE(!c.streaming);
	ALP_ASSERT_EQ_INT(g_streamoff, 1);

	g_qbuf_fail_after = -1;
	g_streamon_fail   = true;
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(g_streamoff, 2);

	g_streamon_fail = false;
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_OK);
	ALP_ASSERT_TRUE(c.streaming);

	c.streaming = false;
	cam_free_buffers(&c);
}

/* y_open -> start -> y_close through the discovery seam. */
static void test_open_close_lifecycle(void)
{
	reset();
	g_sensor_codes[0] = MEDIA_BUS_FMT_Y10_1X10;
	g_nsensor_codes   = 1;
	g_cam_discover    = fake_discover;

	alp_camera_config_t        cfg = cfg_of(ALP_PIXFMT_GREY8, 12, 4, 30);
	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	g_pixel_rate = 10000000;
	ALP_ASSERT_EQ_INT(y_open(&cfg, &st, NULL), ALP_OK);
	ALP_ASSERT_TRUE(st.be_data != NULL);
	ALP_ASSERT_TRUE(st.fps_x1000 > 29900u && st.fps_x1000 < 30300u);
	ALP_ASSERT_EQ_INT(g_nmap, CAM_NBUF);
	ALP_ASSERT_EQ_INT(y_start(&st), ALP_OK);

	y_close(&st); /* stops, unmaps, closes fds, frees */
	ALP_ASSERT_TRUE(st.be_data == NULL);
	ALP_ASSERT_EQ_INT(g_streamoff, 1);
	ALP_ASSERT_EQ_INT(g_munmap_calls, CAM_NBUF);
	for (unsigned i = 0; i < 3u; ++i) {
		ALP_ASSERT_TRUE(fcntl(g_hop_fd[i], F_GETFD) < 0); /* closed */
	}

	/* failure after discovery (no code for the format) cleans up too */
	g_nsensor_codes = 0;
	memset(&st, 0, sizeof(st));
	ALP_ASSERT_EQ_INT(y_open(&cfg, &st, NULL), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_TRUE(st.be_data == NULL);
	for (unsigned i = 0; i < 3u; ++i) {
		ALP_ASSERT_TRUE(fcntl(g_hop_fd[i], F_GETFD) < 0);
	}
}

/* ---- media-graph walk -------------------------------------------- */

static void test_walk(void)
{
	struct media_v2_entity    ent[3] = { { .id = 1, .function = MEDIA_ENT_F_CAM_SENSOR },
		                                 { .id = 2, .function = MEDIA_ENT_F_VID_IF_BRIDGE },
		                                 { .id = 3, .function = MEDIA_ENT_F_IO_V4L } };
	struct media_v2_interface ifc[3];
	struct media_v2_pad       pad[4] = { { .id = 11, .entity_id = 1, .index = 0 },
		                                 { .id = 21, .entity_id = 2, .index = 0 },
		                                 { .id = 22, .entity_id = 2, .index = 1 },
		                                 { .id = 31, .entity_id = 3, .index = 0 } };
	struct media_v2_link      lnk[5];
	memset(ifc, 0, sizeof(ifc));
	memset(lnk, 0, sizeof(lnk));
	for (unsigned i = 0; i < 3; ++i) {
		ifc[i].id            = 201u + i;
		ifc[i].devnode.major = 81;
		ifc[i].devnode.minor = 40u + i;
		lnk[i].id            = 301u + i;
		lnk[i].source_id     = 201u + i;
		lnk[i].sink_id       = 1u + i;
		lnk[i].flags         = MEDIA_LNK_FL_INTERFACE_LINK;
	}
	lnk[3].id        = 101;
	lnk[3].source_id = 11;
	lnk[3].sink_id   = 21;
	lnk[3].flags     = MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE;
	lnk[4].id        = 102;
	lnk[4].source_id = 22;
	lnk[4].sink_id   = 31;
	lnk[4].flags     = MEDIA_LNK_FL_ENABLED;

	cam_topo_t t = { ent, ifc, pad, lnk, 3, 3, 4, 5 };
	cam_t      c;
	memset(&c, 0, sizeof(c));
	ALP_ASSERT_EQ_INT(cam_walk(&t, 1, &c), ALP_OK);
	ALP_ASSERT_EQ_INT(c.nhop, 3);
	ALP_ASSERT_EQ_INT(c.hop[0].src_pad, 0);
	ALP_ASSERT_EQ_INT(c.hop[1].sink_pad, 0);
	ALP_ASSERT_EQ_INT(c.hop[1].src_pad, 1);
	ALP_ASSERT_EQ_INT(c.hop[1].minor, 41);
	ALP_ASSERT_EQ_INT(c.hop[2].function, MEDIA_ENT_F_IO_V4L);
	ALP_ASSERT_EQ_INT(c.hop[2].minor, 42);

	/* a disabled link leaves no path to the capture node */
	lnk[4].flags = 0;
	ALP_ASSERT_EQ_INT(cam_walk(&t, 1, &c), ALP_ERR_NOT_READY);
}

int main(void)
{
	test_missing_alias_is_not_ready();
	test_negotiation_y10_grey8();
	test_negotiation_other_pixfmts();
	test_fps();
	test_subdev_adjust_is_inval();
	test_cr10_unpack();
	test_capture_release_and_timeout();
	test_capture_integrity();
	test_start_failure_resets_queue();
	test_open_close_lifecycle();
	test_walk();

	ALP_TEST_SUMMARY();
}
