/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file gstalpcamerasrc.c
 * @brief alpcamerasrc: a GStreamer live source built on <alp/camera.h>.
 *
 * The V2N CRU emits CR10 (packed RAW10, 8x row stride), which v4l2src and
 * videoconvert cannot read.  <alp/camera.h> unpacks it; this element turns
 * the result into GRAY8, GRAY16_LE or NV12 (mono: Y = pixel, UV = 128) so
 * omxh264enc and the rest of a normal pipeline can consume it:
 *
 *   gst-launch-1.0 alpcamerasrc ! video/x-raw,format=NV12,width=1456,height=1088,framerate=30/1 \
 *       ! omxh264enc ! h264parse ! rtph264pay ! udpsink host=... port=5000
 *
 * Mono sensors map straight through (NV12 at the sensor size).  A colour Bayer
 * sensor (e.g. IMX296 colour, SBGGR10 only) is opened as RAW10 and debayered
 * on the CPU: each 2x2 quad becomes one NV12 pixel, so the output is half the
 * sensor size (1456x1088 -> NV12 728x544).  No ISP, no AWB, no AE; r-gain and
 * b-gain are a fixed white balance and bayer-order names the sensor's order
 * because <alp/camera.h> does not report it:
 *
 *   gst-launch-1.0 alpcamerasrc bayer-order=bggr ! video/x-raw,format=NV12,width=728,height=544,framerate=30/1 ! ...
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/base/gstpushsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <alp/camera.h>
#include <alp/e1m_pinout.h>

#include "alp_gst_convert.h"

GST_DEBUG_CATEGORY_STATIC(alp_camera_src_debug);
#define GST_CAT_DEFAULT alp_camera_src_debug

#define GST_TYPE_ALP_CAMERA_SRC (gst_alp_camera_src_get_type())
G_DECLARE_FINAL_TYPE(GstAlpCameraSrc, gst_alp_camera_src, GST, ALP_CAMERA_SRC, GstPushSrc)

struct _GstAlpCameraSrc {
	GstPushSrc    parent;
	guint         camera_id;
	alp_camera_t *cam;
	gint          flushing; /* set by unlock(), cleared by unlock_stop(); read in create() */
	GstVideoInfo  info;
	gboolean      bayer;       /* opened as RAW10, debayered to half-size NV12 */
	gchar        *bayer_order; /* property: rggb / bggr / grbg / gbrg */
	gdouble       r_gain, b_gain;
	int           order;
	uint32_t      rg_q8, bg_q8;
};

G_DEFINE_TYPE(GstAlpCameraSrc, gst_alp_camera_src, GST_TYPE_PUSH_SRC)

enum { PROP_0, PROP_CAMERA_ID, PROP_BAYER_ORDER, PROP_R_GAIN, PROP_B_GAIN };

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw, format = (string) { GRAY8, GRAY16_LE, NV12 }, "
                    "width = (int) [ 2, 8192 ], height = (int) [ 2, 8192 ], "
                    "framerate = (fraction) [ 1/1, 255/1 ]"));

static GstCaps *gst_alp_camera_src_fixate(GstBaseSrc *bsrc, GstCaps *caps)
{
	/* No size is knowable before the sensor opens: default to the IMX296 mode. */
	GstStructure *s = NULL;
	caps            = gst_caps_make_writable(caps);
	s               = gst_caps_get_structure(caps, 0);
	gst_structure_fixate_field_nearest_int(s, "width", 1456);
	gst_structure_fixate_field_nearest_int(s, "height", 1088);
	gst_structure_fixate_field_nearest_fraction(s, "framerate", 30, 1);
	if (gst_structure_has_field(s, "format"))
		gst_structure_fixate_field_string(s, "format", "GRAY8");
	return GST_BASE_SRC_CLASS(gst_alp_camera_src_parent_class)->fixate(bsrc, caps);
}

static void close_camera(GstAlpCameraSrc *self)
{
	if (self->cam == NULL) return;
	alp_camera_stop(self->cam);
	alp_camera_close(self->cam);
	self->cam = NULL;
}

static gboolean gst_alp_camera_src_set_caps(GstBaseSrc *bsrc, GstCaps *caps)
{
	GstAlpCameraSrc *self = GST_ALP_CAMERA_SRC(bsrc);
	if (!gst_video_info_from_caps(&self->info, caps)) return FALSE;

	GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT(&self->info);
	guint          w = GST_VIDEO_INFO_WIDTH(&self->info), h = GST_VIDEO_INFO_HEIGHT(&self->info);
	if (fmt == GST_VIDEO_FORMAT_NV12 && ((w | h) & 1u)) {
		GST_ERROR_OBJECT(self, "NV12 needs an even width and height, got %ux%u", w, h);
		return FALSE;
	}

	self->order = alp_gst_bayer_parse(self->bayer_order);
	if (self->order < 0) {
		GST_ERROR_OBJECT(self, "bayer-order must be rggb, bggr, grbg or gbrg");
		return FALSE;
	}
	self->rg_q8 = alp_gst_gain_q8(self->r_gain);
	self->bg_q8 = alp_gst_gain_q8(self->b_gain);

	close_camera(self); /* renegotiation */
	self->bayer = FALSE;
	/* GRAY8 and NV12 come from the SDK's 8-bit mono path, GRAY16_LE from its 10-bit one. */
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(self->camera_id);
	cfg.width               = (uint16_t)w;
	cfg.height              = (uint16_t)h;
	/* nearest integer rate: 30000/1001 -> 30 */
	guint fps_d = MAX(GST_VIDEO_INFO_FPS_D(&self->info), 1);
	cfg.fps     = (uint8_t)((GST_VIDEO_INFO_FPS_N(&self->info) + fps_d / 2u) / fps_d);
	cfg.format  = fmt == GST_VIDEO_FORMAT_GRAY16_LE ? ALP_PIXFMT_RAW10 : ALP_PIXFMT_GREY8;

	self->cam = alp_camera_open(&cfg);
	if (self->cam == NULL && fmt == GST_VIDEO_FORMAT_NV12) {
		/* Not a mono sensor: try a Bayer one at twice the NV12 size (2x2 superpixel). */
		alp_status_t mono_err = alp_last_error();
		cfg.width             = (uint16_t)(2u * w);
		cfg.height            = (uint16_t)(2u * h);
		cfg.format            = ALP_PIXFMT_RAW10;
		self->cam             = alp_camera_open(&cfg);
		if (self->cam != NULL) {
			self->bayer = TRUE;
		} else {
			GST_ERROR_OBJECT(self,
			                 "mono open failed (%d); Bayer open at %ux%u failed (%d)",
			                 (int)mono_err,
			                 2u * w,
			                 2u * h,
			                 (int)alp_last_error());
			return FALSE;
		}
	}
	if (self->cam == NULL) {
		GST_ERROR_OBJECT(self,
		                 "alp_camera_open(camera %u, %ux%u) failed, status %d",
		                 self->camera_id,
		                 w,
		                 h,
		                 (int)alp_last_error());
		return FALSE;
	}
	if (alp_camera_start(self->cam) != ALP_OK) {
		GST_ERROR_OBJECT(self, "alp_camera_start failed");
		close_camera(self);
		return FALSE;
	}
	return TRUE;
}

static gboolean gst_alp_camera_src_stop(GstBaseSrc *bsrc)
{
	close_camera(GST_ALP_CAMERA_SRC(bsrc));
	return TRUE;
}

/* create() polls in 200 ms slices and re-checks the flag, so unlock only sets it. */
static gboolean gst_alp_camera_src_unlock(GstBaseSrc *bsrc)
{
	g_atomic_int_set(&GST_ALP_CAMERA_SRC(bsrc)->flushing, 1);
	return TRUE;
}

static gboolean gst_alp_camera_src_unlock_stop(GstBaseSrc *bsrc)
{
	g_atomic_int_set(&GST_ALP_CAMERA_SRC(bsrc)->flushing, 0);
	return TRUE;
}

static GstFlowReturn gst_alp_camera_src_create(GstPushSrc *psrc, GstBuffer **out)
{
	GstAlpCameraSrc   *self = GST_ALP_CAMERA_SRC(psrc);
	GstVideoInfo      *vi   = &self->info;
	guint              w = GST_VIDEO_INFO_WIDTH(vi), h = GST_VIDEO_INFO_HEIGHT(vi);
	gboolean           g16 = GST_VIDEO_INFO_FORMAT(vi) == GST_VIDEO_FORMAT_GRAY16_LE;
	alp_camera_frame_t f;
	alp_status_t       st;

	if (self->cam == NULL) return GST_FLOW_NOT_NEGOTIATED;
	for (;;) {
		st = alp_camera_capture(self->cam, &f, 200);
		if (st == ALP_OK) break;
		if (g_atomic_int_get(&self->flushing)) return GST_FLOW_FLUSHING;
		/* TIMEOUT: poll again.  IO: the backend dropped a corrupt or short frame; ask again. */
		if (st != ALP_ERR_TIMEOUT && st != ALP_ERR_IO) {
			GST_ELEMENT_ERROR(
			    self, RESOURCE, READ, ("camera capture failed"), ("alp status %d", (int)st));
			return GST_FLOW_ERROR;
		}
	}

	size_t need = (size_t)w * h * (g16 ? 2u : 1u);
	if (self->bayer) need = (size_t)(2u * w) * (2u * h) * 2u;
	if (f.size < need) {
		alp_camera_release(self->cam, &f);
		GST_ELEMENT_ERROR(
		    self, STREAM, FAILED, ("short camera frame"), ("%zu < %zu bytes", f.size, need));
		return GST_FLOW_ERROR;
	}

	GstBuffer *buf = gst_buffer_new_allocate(NULL, GST_VIDEO_INFO_SIZE(vi), NULL);
	GstMapInfo m;
	if (buf == NULL || !gst_buffer_map(buf, &m, GST_MAP_WRITE)) {
		if (buf != NULL) gst_buffer_unref(buf);
		alp_camera_release(self->cam, &f);
		return GST_FLOW_ERROR;
	}
	const gsize ys = GST_VIDEO_INFO_PLANE_STRIDE(vi, 0);
	if (self->bayer) {
		alp_gst_bayer10_to_nv12(f.data,
		                        2u * w,
		                        2u * h,
		                        self->order,
		                        self->rg_q8,
		                        self->bg_q8,
		                        m.data,
		                        ys,
		                        GST_VIDEO_INFO_PLANE_OFFSET(vi, 1),
		                        GST_VIDEO_INFO_PLANE_STRIDE(vi, 1));
	} else if (g16) {
		alp_gst_raw10_to_gray16(f.data, w, h, m.data, ys);
	} else if (GST_VIDEO_INFO_FORMAT(vi) == GST_VIDEO_FORMAT_NV12) {
		alp_gst_gray8_to_nv12(f.data,
		                      w,
		                      h,
		                      m.data,
		                      ys,
		                      GST_VIDEO_INFO_PLANE_OFFSET(vi, 1),
		                      GST_VIDEO_INFO_PLANE_STRIDE(vi, 1));
	} else {
		alp_gst_gray8(f.data, w, h, m.data, ys);
	}
	gst_buffer_unmap(buf, &m);
	alp_camera_release(self->cam, &f);
	*out = buf; /* timestamps: do-timestamp is on, basesrc stamps with the pipeline clock */
	return GST_FLOW_OK;
}

static void set_property(GObject *o, guint id, const GValue *v, GParamSpec *p)
{
	GstAlpCameraSrc *self = GST_ALP_CAMERA_SRC(o);
	if (id == PROP_CAMERA_ID) {
		self->camera_id = g_value_get_uint(v);
	} else if (id == PROP_BAYER_ORDER) {
		g_free(self->bayer_order);
		self->bayer_order = g_value_dup_string(v);
	} else if (id == PROP_R_GAIN) {
		self->r_gain = g_value_get_double(v);
	} else if (id == PROP_B_GAIN) {
		self->b_gain = g_value_get_double(v);
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, p);
}

static void get_property(GObject *o, guint id, GValue *v, GParamSpec *p)
{
	GstAlpCameraSrc *self = GST_ALP_CAMERA_SRC(o);
	if (id == PROP_CAMERA_ID)
		g_value_set_uint(v, self->camera_id);
	else if (id == PROP_BAYER_ORDER)
		g_value_set_string(v, self->bayer_order);
	else if (id == PROP_R_GAIN)
		g_value_set_double(v, self->r_gain);
	else if (id == PROP_B_GAIN)
		g_value_set_double(v, self->b_gain);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, p);
}

static void finalize(GObject *o)
{
	g_free(GST_ALP_CAMERA_SRC(o)->bayer_order);
	G_OBJECT_CLASS(gst_alp_camera_src_parent_class)->finalize(o);
}

static void gst_alp_camera_src_class_init(GstAlpCameraSrcClass *klass)
{
	GObjectClass    *go = G_OBJECT_CLASS(klass);
	GstElementClass *ec = GST_ELEMENT_CLASS(klass);
	GstBaseSrcClass *bs = GST_BASE_SRC_CLASS(klass);
	GstPushSrcClass *ps = GST_PUSH_SRC_CLASS(klass);

	go->set_property = set_property;
	go->get_property = get_property;
	go->finalize     = finalize;
	g_object_class_install_property(go,
	                                PROP_CAMERA_ID,
	                                g_param_spec_uint("camera-id",
	                                                  "Camera id",
	                                                  "alp_camera camera_id (ALP_E1M_CAM0 = 0)",
	                                                  0,
	                                                  255,
	                                                  ALP_E1M_CAM0,
	                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property(
	    go,
	    PROP_BAYER_ORDER,
	    g_param_spec_string(
	        "bayer-order",
	        "Bayer order",
	        "Colour sensors only: rggb, bggr, grbg or gbrg (the API does not report it)",
	        "bggr",
	        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
	    go,
	    PROP_R_GAIN,
	    g_param_spec_double("r-gain",
	                        "Red gain",
	                        "Fixed white-balance gain on red (Bayer only)",
	                        0.0,
	                        16.0,
	                        1.0,
	                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
	    go,
	    PROP_B_GAIN,
	    g_param_spec_double("b-gain",
	                        "Blue gain",
	                        "Fixed white-balance gain on blue (Bayer only)",
	                        0.0,
	                        16.0,
	                        1.0,
	                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata(
	    ec,
	    "Alp camera source",
	    "Source/Video",
	    "Camera via <alp/camera.h>: mono CR10 to GRAY8 / GRAY16_LE / NV12, Bayer to half-size NV12",
	    "Alp Lab AB");
	gst_element_class_add_static_pad_template(ec, &src_template);

	bs->fixate      = gst_alp_camera_src_fixate;
	bs->set_caps    = gst_alp_camera_src_set_caps;
	bs->stop        = gst_alp_camera_src_stop;
	bs->unlock      = gst_alp_camera_src_unlock;
	bs->unlock_stop = gst_alp_camera_src_unlock_stop;
	ps->create      = gst_alp_camera_src_create;
}

static void gst_alp_camera_src_init(GstAlpCameraSrc *self)
{
	self->camera_id   = ALP_E1M_CAM0;
	self->bayer_order = g_strdup("bggr");
	self->r_gain = self->b_gain = 1.0;
	gst_base_src_set_live(GST_BASE_SRC(self), TRUE);
	gst_base_src_set_format(GST_BASE_SRC(self), GST_FORMAT_TIME);
	gst_base_src_set_do_timestamp(GST_BASE_SRC(self), TRUE);
}

#ifndef ALP_PLUGIN_VERSION
#define ALP_PLUGIN_VERSION "0.0.0"
#endif

static gboolean plugin_init(GstPlugin *plugin)
{
	GST_DEBUG_CATEGORY_INIT(alp_camera_src_debug, "alpcamerasrc", 0, "alpcamerasrc");
	return gst_element_register(plugin, "alpcamerasrc", GST_RANK_NONE, GST_TYPE_ALP_CAMERA_SRC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR,
                  GST_VERSION_MINOR,
                  alpcamera,
                  "Alp SDK camera source",
                  plugin_init,
                  ALP_PLUGIN_VERSION,
                  "Apache 2.0", /* GStreamer accepts only its fixed licence list */
                  "alp-sdk",
                  "https://github.com/alplabai/alp-sdk")
