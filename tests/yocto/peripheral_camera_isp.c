/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit coverage for the ISP-fed colour path of the Linux camera backend
 * (src/backends/camera/yocto_isp_capture.h, reached through y_open() in
 * yocto_drv.c): fall-through when the ISP stack is absent or the format is
 * not an ISP output, multi-planar format negotiation, the capture /
 * release / timeout / integrity path and the configure_isp decline.
 *
 * yocto_drv.c is #included (same technique as peripheral_camera.c) so every
 * kernel call goes through the g_cam_* seams; no /dev node is touched.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_camera_isp
 *   ctest --test-dir build -R alp_test_peripheral_camera_isp
 */

#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/camera/yocto_drv.c"

static uint32_t g_caps;           /* device_caps the node reports */
static uint32_t g_s_fmt_cc;       /* pixelformat the node saw in S_FMT */
static uint32_t g_planes;         /* num_planes the node reports back */
static uint32_t g_sizeimage;      /* plane sizeimage (0 = w*h*2 for RGB565) */
static bool     g_adjust;         /* node silently changes the width */
static bool     g_parm_fail;      /* S_PARM fails */
static int      g_open_errno;     /* isp_open failure errno (0 = succeed) */
static int      g_querycap_errno; /* QUERYCAP failure errno (0 = succeed) */
static int      g_open_calls;     /* isp node open attempts */
static int      g_discover_calls;
static int      g_qbuf, g_streamon, g_streamoff;
static int      g_poll_ret;
static uint32_t g_dq_flags;
static uint32_t g_dq_bytesused;
static uint32_t g_dq_index;
static int      g_fd = -1;

static int fake_isp_open(uint32_t camera_id)
{
	(void)camera_id;
	++g_open_calls;
	if (g_open_errno != 0) {
		errno = g_open_errno;
		return -1;
	}
	g_fd = open("/dev/null", O_RDWR);
	return g_fd;
}

static int fake_ioctl(int fd, unsigned long req, void *arg)
{
	(void)fd;
	switch (req) {
	case VIDIOC_QUERYCAP: {
		if (g_querycap_errno != 0) {
			errno = g_querycap_errno;
			return -1;
		}
		struct v4l2_capability *c = arg;
		c->capabilities           = V4L2_CAP_DEVICE_CAPS;
		c->device_caps            = g_caps;
		return 0;
	}
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		g_s_fmt_cc            = f->fmt.pix_mp.pixelformat;
		if (g_adjust) f->fmt.pix_mp.width -= 2u;
		f->fmt.pix_mp.num_planes                = (uint8_t)g_planes;
		f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width * 2u;
		f->fmt.pix_mp.plane_fmt[0].sizeimage =
		    g_sizeimage != 0u ? g_sizeimage : f->fmt.pix_mp.width * f->fmt.pix_mp.height * 2u;
		return 0;
	}
	case VIDIOC_S_PARM:
		if (g_parm_fail) {
			errno = ENOTTY;
			return -1;
		}
		return 0;
	case VIDIOC_G_PARM: {
		struct v4l2_streamparm *sp                = arg;
		sp->parm.capture.timeperframe.numerator   = 1u;
		sp->parm.capture.timeperframe.denominator = 30u;
		return 0;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		r->count                      = CAM_NBUF;
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b       = arg;
		b->m.planes[0].length       = 1280u * 720u * 2u;
		b->m.planes[0].m.mem_offset = b->index * 4096u;
		return 0;
	}
	case VIDIOC_QBUF:
		++g_qbuf;
		return 0;
	case VIDIOC_DQBUF: {
		struct v4l2_buffer *b    = arg;
		b->index                 = g_dq_index;
		b->flags                 = g_dq_flags;
		b->timestamp.tv_sec      = 7;
		b->timestamp.tv_usec     = 500000;
		b->m.planes[0].bytesused = g_dq_bytesused;
		return 0;
	}
	case VIDIOC_STREAMON:
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
	return calloc(1, len);
}

static void fake_munmap(void *p, size_t len)
{
	(void)len;
	free(p);
}

static alp_status_t fake_discover(uint32_t camera_id, cam_t *c)
{
	(void)camera_id;
	(void)c;
	++g_discover_calls;
	return ALP_ERR_NOT_READY; /* the raw path ran: no sensor in this test */
}

static void reset(void)
{
	g_caps           = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
	g_s_fmt_cc       = 0;
	g_planes         = 1;
	g_sizeimage      = 0;
	g_adjust         = false;
	g_parm_fail      = false;
	g_open_errno     = 0;
	g_querycap_errno = 0;
	g_open_calls     = 0;
	g_discover_calls = 0;
	g_qbuf = g_streamon = g_streamoff = 0;
	g_poll_ret                        = 1;
	g_dq_flags                        = 0;
	g_dq_bytesused                    = 0;
	g_dq_index                        = 0;
	g_fd                              = -1;
	g_cam_ioctl                       = fake_ioctl;
	g_cam_poll                        = fake_poll;
	g_cam_mmap                        = fake_mmap;
	g_cam_munmap                      = fake_munmap;
	g_cam_isp_open                    = fake_isp_open;
	g_cam_discover                    = fake_discover;
}

static alp_camera_config_t cfg(alp_pixfmt_t pf)
{
	alp_camera_config_t c = {
		.camera_id = 0, .width = 1280, .height = 720, .fps = 30, .format = pf
	};
	return c;
}

static alp_camera_backend_state_t fresh_state(void)
{
	alp_camera_backend_state_t st;
	memset(&st, 0, sizeof(st));
	st.ops = &_ops;
	return st;
}

/* ---- fall-through to the raw path -------------------------------- */

static void test_absent_isp_falls_through(void)
{
	reset();
	g_open_errno                  = ENOENT; /* no /dev/video0fr */
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(g_open_calls, 1);
	ALP_ASSERT_EQ_INT(g_discover_calls, 1); /* raw path took over */
	ALP_ASSERT_TRUE(st.ops == &_ops);
}

static void test_raw_formats_skip_the_isp(void)
{
	alp_pixfmt_t raw[] = { ALP_PIXFMT_GREY8, ALP_PIXFMT_RAW8, ALP_PIXFMT_RAW10 };
	for (unsigned i = 0; i < 3u; ++i) {
		reset();
		alp_camera_config_t        c  = cfg(raw[i]);
		alp_camera_backend_state_t st = fresh_state();
		ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_NOT_READY);
		ALP_ASSERT_EQ_INT(g_open_calls, 0); /* node never touched */
		ALP_ASSERT_EQ_INT(g_discover_calls, 1);
	}
}

static void test_non_mplane_node_declines(void)
{
	reset();
	g_caps                        = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(g_discover_calls, 1);
	ALP_ASSERT_TRUE(st.ops == &_ops);
	ALP_ASSERT_TRUE(fcntl(g_fd, F_GETFD) < 0); /* fd closed on decline */
}

static void test_open_error_is_not_swallowed(void)
{
	reset();
	g_open_errno                  = EBUSY; /* another process owns the node */
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), alp_status_from_posix_errno(EBUSY));
	ALP_ASSERT_EQ_INT(g_discover_calls, 0);
}

static void test_querycap_failure_is_not_swallowed(void)
{
	reset();
	g_querycap_errno              = EIO; /* node exists but is unusable */
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), alp_status_from_posix_errno(EIO));
	ALP_ASSERT_EQ_INT(g_discover_calls, 0);
	ALP_ASSERT_TRUE(fcntl(g_fd, F_GETFD) < 0);

	reset();
	g_querycap_errno = ENOTTY; /* not a V4L2 node: raw path takes over */
	st               = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(g_discover_calls, 1);
}

/* ---- negotiation --------------------------------------------------- */

static void test_negotiation(void)
{
	reset();
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_OK);
	ALP_ASSERT_TRUE(st.ops == &_isp_ops);
	ALP_ASSERT_EQ_INT(g_s_fmt_cc, V4L2_PIX_FMT_RGB565);
	ALP_ASSERT_EQ_INT(st.fps_x1000, 30000);
	ALP_ASSERT_EQ_INT(g_discover_calls, 0);
	st.ops->close(&st);
	ALP_ASSERT_TRUE(st.be_data == NULL);
	ALP_ASSERT_TRUE(fcntl(g_fd, F_GETFD) < 0);

	reset();
	c           = cfg(ALP_PIXFMT_NV12);
	st          = fresh_state();
	g_sizeimage = 1280u * 720u * 3u / 2u;
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_OK);
	ALP_ASSERT_EQ_INT(g_s_fmt_cc, V4L2_PIX_FMT_NV12);
	st.ops->close(&st);

	/* a node that cannot apply the fps request keeps its own and still opens */
	reset();
	g_parm_fail = true;
	c           = cfg(ALP_PIXFMT_RGB565);
	st          = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_OK);
	st.ops->close(&st);
}

static void test_negotiation_failures_restore_the_ops(void)
{
	reset();
	g_adjust                      = true; /* width silently changed */
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_INVAL);
	ALP_ASSERT_TRUE(st.be_data == NULL);
	ALP_ASSERT_TRUE(fcntl(g_fd, F_GETFD) < 0);

	reset();
	g_sizeimage = 100u; /* smaller than one frame */
	st          = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_IO);

	/* two planes: not one contiguous frame -> decline, raw path runs */
	reset();
	g_planes = 2;
	st       = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(g_discover_calls, 1);
	ALP_ASSERT_TRUE(st.ops == &_ops);
}

/* ---- streaming ---------------------------------------------------- */

static void test_capture_release_and_timeout(void)
{
	reset();
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_OK);
	const alp_camera_ops_t *ops = st.ops;

	alp_camera_frame_t f;
	memset(&f, 0, sizeof(f));
	ALP_ASSERT_EQ_INT(ops->capture(&st, &f, 10), ALP_ERR_NOT_READY); /* not streaming */

	ALP_ASSERT_EQ_INT(ops->start(&st), ALP_OK);
	ALP_ASSERT_EQ_INT(g_qbuf, CAM_NBUF);
	ALP_ASSERT_EQ_INT(g_streamon, 1);

	g_poll_ret = 0;
	ALP_ASSERT_EQ_INT(ops->capture(&st, &f, 10), ALP_ERR_TIMEOUT);

	g_poll_ret     = 1;
	g_dq_index     = 2;
	g_dq_bytesused = 1280u * 720u * 2u;
	ALP_ASSERT_EQ_INT(ops->capture(&st, &f, 10), ALP_OK);
	ALP_ASSERT_TRUE(f.data != NULL);
	ALP_ASSERT_EQ_INT(f.size, 1280u * 720u * 2u);
	ALP_ASSERT_EQ_INT(f.timestamp_us, 7500000);

	int q = g_qbuf;
	ALP_ASSERT_EQ_INT(ops->release(&st, &f), ALP_OK);
	ALP_ASSERT_EQ_INT(g_qbuf, q + 1);                        /* requeued while streaming */
	ALP_ASSERT_EQ_INT(ops->release(&st, &f), ALP_ERR_INVAL); /* not held any more */

	ALP_ASSERT_EQ_INT(ops->stop(&st), ALP_OK);
	ALP_ASSERT_EQ_INT(g_streamoff, 1);
	ops->close(&st);
}

static void test_capture_integrity(void)
{
	reset();
	alp_camera_config_t        c  = cfg(ALP_PIXFMT_RGB565);
	alp_camera_backend_state_t st = fresh_state();
	ALP_ASSERT_EQ_INT(y_open(&c, &st, NULL), ALP_OK);
	const alp_camera_ops_t *ops = st.ops;
	ALP_ASSERT_EQ_INT(ops->start(&st), ALP_OK);

	alp_camera_frame_t f;
	memset(&f, 0, sizeof(f));
	int q      = g_qbuf;
	g_dq_flags = V4L2_BUF_FLAG_ERROR;
	ALP_ASSERT_EQ_INT(ops->capture(&st, &f, 10), ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(g_qbuf, q + 1); /* corrupt frame requeued, not handed out */

	g_dq_flags     = 0;
	g_dq_bytesused = 100u; /* short frame */
	ALP_ASSERT_EQ_INT(ops->capture(&st, &f, 10), ALP_ERR_IO);

	ALP_ASSERT_EQ_INT(ops->configure_isp(&st, NULL), ALP_ERR_NOSUPPORT);
	ops->close(&st);
}

int main(void)
{
	test_absent_isp_falls_through();
	test_raw_formats_skip_the_isp();
	test_non_mplane_node_declines();
	test_open_error_is_not_swallowed();
	test_querycap_failure_is_not_swallowed();
	test_negotiation();
	test_negotiation_failures_restore_the_ops();
	test_capture_release_and_timeout();
	test_capture_integrity();

	ALP_TEST_SUMMARY();
}
