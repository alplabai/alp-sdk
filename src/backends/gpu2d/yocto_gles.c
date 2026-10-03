/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux GPU backend for <alp/gpu2d.h>: fill / blit / blend executed on
 * the SoC's 3D GPU through the vendor-supplied EGL + OpenGL ES 3 stack.
 *
 * ====== ADR 0017 Tier-1.5 (thin glue over the vendor GLES/EGL stack) -- BENCH-UNVERIFIED ======
 * The Mali-G31 on RZ/V2N is a 3D GPU with no 2D blit engine, so there is
 * no D/AVE-style register block to drive.  The vendor already ships the
 * whole userspace driver (Renesas meta-rz-graphics `mali-library`:
 * libEGL / libGLESv2 + egl.pc / glesv2.pc).  This file only maps the three
 * portable 2D ops onto standard Khronos EGL 1.4 + GLES 3.0 calls -- no
 * vendor header, no vendor symbol, so any conformant EGL/GLES3 stack
 * (Mali, Vivante, Mesa) works unchanged.  Nothing proprietary is copied
 * into this tree; the DDK stays in the Renesas layer / private mirror and
 * the build links it from the sysroot (ALP_SDK_USE_GPU2D_GLES, see
 * src/yocto/CMakeLists.txt).
 *
 * @par Supported silicon: any Linux target with an EGL/GLES3 driver (wildcard
 *      registration; RZ/V2N-family Mali-G31 is the intended one).
 *
 * How an op runs (the caller's pixels live in CPU memory, so the GPU path
 * is upload -> draw -> read back):
 *   1. upload the clipped src (and, for a blend, the dst) rect to textures,
 *   2. draw one quad into an off-screen FBO sized to the rect, fetching the
 *      source with texelFetch (1:1, no filtering) under the blend factors
 *      of the requested mode,
 *   3. glReadPixels the rect back over the caller's dst.
 * Channel ops are per-component, so the BGRA byte order of ARGB8888 in
 * little-endian memory needs no swizzle (only the fill colour swaps R/B).
 *
 * Delegation: anything this path cannot or should not do is handed to the
 * portable CPU path (alp_gpu2d_sw_ops()), so the "write once" contract
 * holds op by op -- formats other than ARGB8888, strides that are not a
 * whole number of pixels, rects below ALP_GPU2D_GLES_MIN_PIXELS, a GL error,
 * and the whole handle when no EGL/GLES context can be created (no
 * compositor / no GPU).  open() therefore succeeds on every Linux target.
 *
 * Context: a 1x1 pbuffer on EGL_DEFAULT_DISPLAY.  On the Renesas default
 * Mali `wayland` userspace variant that needs a running Wayland compositor
 * (alp-display / weston); without one open() degrades to the CPU path.
 * Each op makes the context current on the calling thread and releases it,
 * under a backend mutex, so ops may come from different threads.
 *
 * @par Cost: ROM ~6 KB; RAM ~100 B + GL driver allocations.
 * @par Performance: O(w * h) copies each way plus the GPU draw.  The
 *      upload/readback round trip makes small rects SLOWER than the CPU
 *      path; ALP_GPU2D_GLES_MIN_PIXELS is an UNMEASURED starting point to
 *      be tuned on silicon.  Do not read this backend as "GPU is always
 *      faster" -- it pays off for large, blend-heavy layers.
 */

#include <alp/peripheral.h>

#if defined(ALP_SDK_USE_GPU2D_GLES)

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>

#include "gpu2d_ops.h"

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif

/* Rects smaller than this many pixels go to the CPU path (the GPU round
 * trip costs more than the copy).  Unmeasured -- tune on silicon. */
#ifndef ALP_GPU2D_GLES_MIN_PIXELS
#define ALP_GPU2D_GLES_MIN_PIXELS 16384u
#endif

typedef struct {
	EGLDisplay      dpy;
	EGLContext      ctx;
	EGLSurface      surf;
	GLuint          prog;
	GLuint          fbo;
	GLuint          tex_src;
	GLuint          tex_dst;
	pthread_mutex_t lock;
} gles_state_t;

/* The pool is one handle (gpu2d_dispatch.c), so one static state suffices;
 * be_data points at it while the handle is open and GL-capable. */
static gles_state_t _g;

static const char _vs_src[] = "#version 300 es\n"
                              "layout(location = 0) in vec2 p;\n"
                              "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

/* texelFetch at the fragment's own pixel: exact 1:1 copy of the source. */
static const char _fs_src[] = "#version 300 es\n"
                              "precision highp float;\n"
                              "uniform highp sampler2D t;\n"
                              "out vec4 c;\n"
                              "void main() { c = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }\n";

static GLuint _compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glDeleteShader(s);
		return 0u;
	}
	return s;
}

static void _teardown(gles_state_t *g)
{
	if (g->dpy != EGL_NO_DISPLAY) {
		if (eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) == EGL_TRUE) {
			if (g->prog != 0u) {
				glDeleteProgram(g->prog);
			}
			if (g->fbo != 0u) {
				glDeleteFramebuffers(1, &g->fbo);
			}
			GLuint t[2] = { g->tex_src, g->tex_dst };
			glDeleteTextures(2, t);
		}
		eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (g->ctx != EGL_NO_CONTEXT) {
			eglDestroyContext(g->dpy, g->ctx);
		}
		if (g->surf != EGL_NO_SURFACE) {
			eglDestroySurface(g->dpy, g->surf);
		}
		eglTerminate(g->dpy);
	}
	g->dpy  = EGL_NO_DISPLAY;
	g->ctx  = EGL_NO_CONTEXT;
	g->surf = EGL_NO_SURFACE;
}

/* Create the context + program once.  Returns true with the context
 * released from this thread; false leaves nothing allocated. */
static bool _init(gles_state_t *g)
{
	*g = (gles_state_t){ .dpy = EGL_NO_DISPLAY, .ctx = EGL_NO_CONTEXT, .surf = EGL_NO_SURFACE };

	g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint maj, min;
	if (g->dpy == EGL_NO_DISPLAY || eglInitialize(g->dpy, &maj, &min) != EGL_TRUE) {
		g->dpy = EGL_NO_DISPLAY;
		return false;
	}
	if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
		goto fail;
	}
	const EGLint cfg_attr[] = { EGL_SURFACE_TYPE,
		                        EGL_PBUFFER_BIT,
		                        EGL_RENDERABLE_TYPE,
		                        EGL_OPENGL_ES3_BIT,
		                        EGL_RED_SIZE,
		                        8,
		                        EGL_GREEN_SIZE,
		                        8,
		                        EGL_BLUE_SIZE,
		                        8,
		                        EGL_ALPHA_SIZE,
		                        8,
		                        EGL_NONE };
	EGLConfig    cfg;
	EGLint       n = 0;
	if (eglChooseConfig(g->dpy, cfg_attr, &cfg, 1, &n) != EGL_TRUE || n < 1) {
		goto fail;
	}
	const EGLint pb_attr[]  = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	g->surf                 = eglCreatePbufferSurface(g->dpy, cfg, pb_attr);
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	g->ctx                  = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (g->surf == EGL_NO_SURFACE || g->ctx == EGL_NO_CONTEXT ||
	    eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) != EGL_TRUE) {
		goto fail;
	}

	GLuint vs = _compile(GL_VERTEX_SHADER, _vs_src);
	GLuint fs = _compile(GL_FRAGMENT_SHADER, _fs_src);
	if (vs != 0u && fs != 0u) {
		g->prog = glCreateProgram();
		glAttachShader(g->prog, vs);
		glAttachShader(g->prog, fs);
		glLinkProgram(g->prog);
		GLint ok = 0;
		glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
		if (!ok) {
			glDeleteProgram(g->prog);
			g->prog = 0u;
		}
	}
	if (vs != 0u) {
		glDeleteShader(vs);
	}
	if (fs != 0u) {
		glDeleteShader(fs);
	}
	if (g->prog == 0u) {
		goto fail;
	}
	glGenFramebuffers(1, &g->fbo);
	glGenTextures(1, &g->tex_src);
	glGenTextures(1, &g->tex_dst);
	if (glGetError() != GL_NO_ERROR) {
		goto fail;
	}
	pthread_mutex_init(&g->lock, NULL);
	eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return true;

fail:
	_teardown(g);
	return false;
}

/* ---- helpers ---------------------------------------------------------- */

static bool _gl_ok_surface(const alp_gpu2d_surface_t *s)
{
	return s->format == ALP_GPU2D_FMT_ARGB8888 && (s->stride_bytes % 4u) == 0u;
}

static void _tex_setup(GLuint tex)
{
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* Upload the w x h rect at (x, y) of @p s (caller memory) into the bound
 * texture; NULL pixels just allocates storage. */
static void
_tex_upload(const alp_gpu2d_surface_t *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (s != NULL) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->stride_bytes / 4u));
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, (GLint)x);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, (GLint)y);
	}
	glTexImage2D(GL_TEXTURE_2D,
	             0,
	             GL_RGBA8,
	             (GLsizei)w,
	             (GLsizei)h,
	             0,
	             GL_RGBA,
	             GL_UNSIGNED_BYTE,
	             s != NULL ? s->base : NULL);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
}

static void _set_blend(alp_gpu2d_blend_mode_t mode)
{
	switch (mode) {
	case ALP_GPU2D_BLEND_REPLACE:
		glDisable(GL_BLEND);
		return;
	case ALP_GPU2D_BLEND_SRC_OVER: /* straight alpha; dst.a = sa + da*(1-sa) like the CPU path */
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case ALP_GPU2D_BLEND_ADDITIVE:
		glBlendFunc(GL_ONE, GL_ONE);
		break;
	default: /* MULTIPLY */
		glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO);
		break;
	}
	glBlendEquation(GL_FUNC_ADD);
	glEnable(GL_BLEND);
}

/* Run one op on the GPU.  @p src == NULL means a solid fill of @p fill
 * (ARGB8888 value).  Returns false -- with the caller's dst untouched --
 * if anything failed, so the caller can fall back to the CPU path. */
static bool _gl_run(gles_state_t              *g,
                    const alp_gpu2d_surface_t *src,
                    uint32_t                   sx,
                    uint32_t                   sy,
                    const alp_gpu2d_surface_t *dst,
                    uint32_t                   dx,
                    uint32_t                   dy,
                    uint32_t                   w,
                    uint32_t                   h,
                    alp_gpu2d_blend_mode_t     mode,
                    uint32_t                   fill)
{
	bool ok = false;

	pthread_mutex_lock(&g->lock);
	if (eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx) != EGL_TRUE) {
		goto out;
	}
	while (glGetError() != GL_NO_ERROR) {
	}

	/* dst texture is the FBO colour attachment; it only needs the old dst
	 * pixels when a blend reads them. */
	_tex_setup(g->tex_dst);
	_tex_upload((src != NULL && mode != ALP_GPU2D_BLEND_REPLACE) ? dst : NULL, dx, dy, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->tex_dst, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		goto done;
	}
	glViewport(0, 0, (GLsizei)w, (GLsizei)h);
	glDisable(GL_SCISSOR_TEST);

	if (src == NULL) {
		/* Memory order of ARGB8888 is B,G,R,A, so GL's .r component is blue. */
		glClearColor((GLfloat)(fill & 0xFFu) / 255.0f,
		             (GLfloat)((fill >> 8) & 0xFFu) / 255.0f,
		             (GLfloat)((fill >> 16) & 0xFFu) / 255.0f,
		             (GLfloat)((fill >> 24) & 0xFFu) / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	} else {
		static const GLfloat quad[8] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
		_tex_setup(g->tex_src);
		_tex_upload(src, sx, sy, w, h);
		glUseProgram(g->prog);
		glUniform1i(glGetUniformLocation(g->prog, "t"), 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g->tex_src);
		_set_blend(mode);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glDisableVertexAttribArray(0);
		glDisable(GL_BLEND);
	}
	if (glGetError() != GL_NO_ERROR) {
		goto done;
	}

	glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(dst->stride_bytes / 4u));
	glPixelStorei(GL_PACK_SKIP_PIXELS, (GLint)dx);
	glPixelStorei(GL_PACK_SKIP_ROWS, (GLint)dy);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, dst->base);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
	ok = (glGetError() == GL_NO_ERROR);

done:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
out:
	pthread_mutex_unlock(&g->lock);
	return ok;
}

/* ---- ops -------------------------------------------------------------- */

static alp_status_t gl_open(alp_gpu2d_backend_state_t *state, alp_capabilities_t *caps_out)
{
	/* Degrade, never fail: no context -> be_data stays NULL and every op
	 * runs on the CPU path, same as the wildcard sw_fallback would. */
	if (_init(&_g)) {
		state->be_data = &_g;
		if (caps_out != NULL) {
			caps_out->flags |= (uint32_t)(ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED);
		}
	} else {
		state->be_data = NULL;
	}
	return ALP_OK;
}

static void gl_close(alp_gpu2d_backend_state_t *state)
{
	if (state->be_data != NULL) {
		_teardown((gles_state_t *)state->be_data);
		pthread_mutex_destroy(&((gles_state_t *)state->be_data)->lock);
		state->be_data = NULL;
	}
}

static alp_status_t gl_fill_rect(alp_gpu2d_backend_state_t *state,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   x,
                                 uint32_t                   y,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 uint32_t                   argb_color)
{
	const alp_gpu2d_ops_t *sw = alp_gpu2d_sw_ops();
	if (state->be_data != NULL && _gl_ok_surface(dst) && alp_gpu2d_clip_rect(dst, x, y, &w, &h) &&
	    (uint64_t)w * h >= ALP_GPU2D_GLES_MIN_PIXELS) {
		if (_gl_run((gles_state_t *)state->be_data,
		            NULL,
		            0,
		            0,
		            dst,
		            x,
		            y,
		            w,
		            h,
		            ALP_GPU2D_BLEND_REPLACE,
		            argb_color)) {
			return ALP_OK;
		}
	}
	return sw->fill_rect(state, dst, x, y, w, h, argb_color);
}

/* Shared blit/blend body: GPU when eligible, CPU otherwise. */
static alp_status_t gl_composite(alp_gpu2d_backend_state_t *state,
                                 const alp_gpu2d_surface_t *src,
                                 uint32_t                   sx,
                                 uint32_t                   sy,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   dx,
                                 uint32_t                   dy,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 alp_gpu2d_blend_mode_t     mode)
{
	const alp_gpu2d_ops_t *sw = alp_gpu2d_sw_ops();
	uint32_t               cw = w, ch = h;
	if (state->be_data != NULL && _gl_ok_surface(src) && _gl_ok_surface(dst) &&
	    alp_gpu2d_clip_rect(src, sx, sy, &cw, &ch) && alp_gpu2d_clip_rect(dst, dx, dy, &cw, &ch) &&
	    (uint64_t)cw * ch >= ALP_GPU2D_GLES_MIN_PIXELS) {
		if (_gl_run((gles_state_t *)state->be_data, src, sx, sy, dst, dx, dy, cw, ch, mode, 0u)) {
			return ALP_OK;
		}
	}
	return (mode == ALP_GPU2D_BLEND_REPLACE)
	           ? sw->blit(state, src, sx, sy, dst, dx, dy, w, h)
	           : sw->blend(state, src, sx, sy, dst, dx, dy, w, h, mode);
}

static alp_status_t gl_blit(alp_gpu2d_backend_state_t *state,
                            const alp_gpu2d_surface_t *src,
                            uint32_t                   sx,
                            uint32_t                   sy,
                            const alp_gpu2d_surface_t *dst,
                            uint32_t                   dx,
                            uint32_t                   dy,
                            uint32_t                   w,
                            uint32_t                   h)
{
	return gl_composite(state, src, sx, sy, dst, dx, dy, w, h, ALP_GPU2D_BLEND_REPLACE);
}

static alp_status_t gl_blend(alp_gpu2d_backend_state_t *state,
                             const alp_gpu2d_surface_t *src,
                             uint32_t                   sx,
                             uint32_t                   sy,
                             const alp_gpu2d_surface_t *dst,
                             uint32_t                   dx,
                             uint32_t                   dy,
                             uint32_t                   w,
                             uint32_t                   h,
                             alp_gpu2d_blend_mode_t     mode)
{
	return gl_composite(state, src, sx, sy, dst, dx, dy, w, h, mode);
}

static const alp_gpu2d_ops_t _ops = {
	.open      = gl_open,
	.fill_rect = gl_fill_rect,
	.blit      = gl_blit,
	.blend     = gl_blend,
	.close     = gl_close,
};

/* Wildcard, priority 50: outranks the priority-0 CPU fallback on every
 * Linux target this is linked into, never an exact-silicon engine
 * (D/AVE 2D registers at 100).  Same convention as the Linux backends of
 * the other classes (jpeg sw_baseline, display yocto_drv). */
ALP_BACKEND_REGISTER(gpu2d,
                     yocto_gles,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* ALP_SDK_USE_GPU2D_GLES */
