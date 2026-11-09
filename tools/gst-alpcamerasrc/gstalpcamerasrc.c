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
 * Mono / Bayer-as-mono sensors only: the SDK does no demosaic or ISP.
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
	GstVideoInfo  info;
};

G_DEFINE_TYPE(GstAlpCameraSrc, gst_alp_camera_src, GST_TYPE_PUSH_SRC)

enum { PROP_0, PROP_CAMERA_ID };

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

	close_camera(self); /* renegotiation */
	/* GRAY8 and NV12 come from the SDK's 8-bit mono path, GRAY16_LE from its 10-bit one. */
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(self->camera_id);
	cfg.width               = (uint16_t)w;
	cfg.height              = (uint16_t)h;
	/* nearest integer rate: 30000/1001 -> 30 */
	guint fps_d = MAX(GST_VIDEO_INFO_FPS_D(&self->info), 1);
	cfg.fps     = (uint8_t)((GST_VIDEO_INFO_FPS_N(&self->info) + fps_d / 2u) / fps_d);
	cfg.format  = fmt == GST_VIDEO_FORMAT_GRAY16_LE ? ALP_PIXFMT_RAW10 : ALP_PIXFMT_GREY8;

	self->cam = alp_camera_open(&cfg);
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

static gboolean gst_alp_camera_src_unlock(GstBaseSrc *bsrc)
{
	/* create() polls in 200 ms slices and re-checks flushing, so nothing to wake. */
	(void)bsrc;
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
		if (GST_BASE_SRC_IS_FLUSHING(psrc)) return GST_FLOW_FLUSHING;
		/* TIMEOUT: poll again.  IO: the backend dropped a corrupt or short frame; ask again. */
		if (st != ALP_ERR_TIMEOUT && st != ALP_ERR_IO) {
			GST_ELEMENT_ERROR(
			    self, RESOURCE, READ, ("camera capture failed"), ("alp status %d", (int)st));
			return GST_FLOW_ERROR;
		}
	}

	size_t need = (size_t)w * h * (g16 ? 2u : 1u);
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
	if (g16) {
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
	if (id == PROP_CAMERA_ID)
		self->camera_id = g_value_get_uint(v);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, p);
}

static void get_property(GObject *o, guint id, GValue *v, GParamSpec *p)
{
	GstAlpCameraSrc *self = GST_ALP_CAMERA_SRC(o);
	if (id == PROP_CAMERA_ID)
		g_value_set_uint(v, self->camera_id);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, p);
}

static void gst_alp_camera_src_class_init(GstAlpCameraSrcClass *klass)
{
	GObjectClass    *go = G_OBJECT_CLASS(klass);
	GstElementClass *ec = GST_ELEMENT_CLASS(klass);
	GstBaseSrcClass *bs = GST_BASE_SRC_CLASS(klass);
	GstPushSrcClass *ps = GST_PUSH_SRC_CLASS(klass);

	go->set_property = set_property;
	go->get_property = get_property;
	g_object_class_install_property(go,
	                                PROP_CAMERA_ID,
	                                g_param_spec_uint("camera-id",
	                                                  "Camera id",
	                                                  "alp_camera camera_id (ALP_E1M_CAM0 = 0)",
	                                                  0,
	                                                  255,
	                                                  ALP_E1M_CAM0,
	                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata(
	    ec,
	    "Alp camera source",
	    "Source/Video",
	    "Mono camera via <alp/camera.h>: CR10 to GRAY8 / GRAY16_LE / NV12",
	    "Alp Lab AB");
	gst_element_class_add_static_pad_template(ec, &src_template);

	bs->fixate   = gst_alp_camera_src_fixate;
	bs->set_caps = gst_alp_camera_src_set_caps;
	bs->stop     = gst_alp_camera_src_stop;
	bs->unlock   = gst_alp_camera_src_unlock;
	ps->create   = gst_alp_camera_src_create;
}

static void gst_alp_camera_src_init(GstAlpCameraSrc *self)
{
	self->camera_id = ALP_E1M_CAM0;
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
