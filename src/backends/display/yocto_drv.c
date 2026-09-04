/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto display_* driver-class backend over DRM/KMS dumb
 * buffers (issue #1143).  Targets the RZ/V2N `du` + `dsi0` path (the
 * RK055HDMIPI4MA0 panel bound in meta-alp-sdk's e1m-x-evk.dtsi) but is
 * SoC-agnostic: it only speaks the generic KMS uAPI, never a
 * vendor-specific one.
 *
 * Direct ioctls against <drm/drm.h> + <drm/drm_mode.h> + <drm/drm_fourcc.h>
 * -- these are raw uapi headers shipped by every Linux userspace
 * toolchain (linux-libc-dev on Debian/Ubuntu; the Yocto sysroot's own
 * kernel-headers recipe), the SAME class of dependency the storage
 * backend already takes on <linux/fs.h> + <mtd/mtd-abi.h>.  Deliberately
 * NOT linked against libdrm: every struct/ioctl number used below is
 * defined in those three uapi headers alone, so this file adds no new
 * library dependency, only a few widely-available headers.
 *
 * cfg->display_id maps straight to the kernel's own enumeration index --
 * "/dev/dri/card<display_id>" -- mirroring the storage backend's
 * instance_id -> /dev/mmcblk<N> mapping note: which physical DU/DSI
 * controller lands on which card index is decided by DT probe order,
 * not by this backend.
 *
 * @par Modeset flow (open()): GETRESOURCES (two-call idiom) to list
 *      connectors/crtcs, GETCONNECTOR on each until a CONNECTED one
 *      with at least one mode is found (its modes[0] is used --  the
 *      kernel convention is that a connector's most-preferred mode
 *      sorts first), GETENCODER to read `possible_crtcs`, pick an
 *      unclaimed CRTC from the resource list, CREATE_DUMB + ADDFB2
 *      (DRM_FORMAT_ARGB8888, matching <alp/peripheral.h>'s
 *      ALP_PIXFMT_ARGB8888 byte-for-byte -- both are B,G,R,A in
 *      memory, little-endian) + MAP_DUMB + mmap(), then SETCRTC binds
 *      the new framebuffer to the chosen CRTC/connector/mode -- this
 *      IS the act of lighting the panel.
 *
 * @par Safety gate: alp_display_config_t.allow_modeset, default false.
 *      An earlier version of this file argued no gate was needed,
 *      reasoning that DRM_IOCTL_SET_MASTER's kernel-side exclusivity
 *      was guard enough.  That was wrong, in the way that matters.
 *      SET_MASTER only fails while ANOTHER PROCESS CURRENTLY HOLDS
 *      master.  When master is simply unheld -- an idle framebuffer
 *      console, a Wayland/X session VT-switched away or on an inactive
 *      logind seat (both DROP_MASTER), a headless SSH login on a board
 *      with a panel attached -- drm_master_open() hands US master and
 *      SETCRTC reprograms the live output.  alp_display_open() with the
 *      documented default display_id = 0 reaches every one of those.
 *      So opening now requires an explicit allow_modeset, exactly as
 *      storage requires allow_unsafe_write (#1140): a default-
 *      constructed config cannot take over a screen.  There is no
 *      lesser "read-only" open that would still satisfy
 *      <alp/display.h>'s contract, so the gate lives on open() rather
 *      than on the individual ops.
 *
 * @par Error mapping, corrected: SET_MASTER's failure is now checked
 *      immediately rather than carried in a flag, so it reaches the
 *      caller at all.  drm_setmaster_ioctl returns -EBUSY when another
 *      process holds master, which alp_status_from_posix_errno()
 *      already maps to ALP_ERR_BUSY -- no special case needed here.
 *      What the previous version got wrong was WHERE it detected the
 *      failure: it let SETCRTC be the test, and SETCRTC refuses with
 *      -EACCES, which that mapper does NOT carry, so the documented
 *      ALP_ERR_BUSY was unreachable and callers saw ALP_ERR_IO.
 *      Failing at SET_MASTER also avoids allocating and mapping a
 *      full-screen scanout buffer (~8 MB at 1080p) before finding out.
 *
 * @par What close() DOES do: it blanks the panel, and that is not a
 *      choice this file makes -- _teardown() issues RMFB on the
 *      framebuffer still being scanned out, and the DRM ABI requires
 *      the kernel to disable any CRTC or plane still using a removed
 *      framebuffer (drm_framebuffer_remove() ->
 *      atomic_remove_fb()/legacy_remove_fb()).  The minimal KMS
 *      examples that leave the last frame up do so precisely because
 *      they never RMFB; they just exit and let last-close reclaim it.
 *      It does NOT restore the pre-open CRTC state -- there is no
 *      recorded "previous mode" to go back to.
 *
 * Registered at priority 100 with vendor "linux"; the zephyr_stub
 * wildcard (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.  Selected on any silicon
 * (silicon_ref "*") because the KMS uAPI is SoC-agnostic -- the
 * device tree decides which physical DU/DSI/LCDC controller backs
 * each /dev/dri/cardN.
 *
 * @par Status: REAL implementation.  BENCH-UNVERIFIED -- no
 *      /dev/dri/card* node exists in this build environment, so the
 *      open()/get_caps()/blit()/clear()/close() paths have not been
 *      exercised against real V2N DU/DSI/Mali-DRM hardware.  Do not
 *      read anything in this file as silicon-proven.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>

#include "common/alp_errno.h"
#include "display_ops.h"

/* enum drm_connector_status's CONNECTED value (1) is stable DRM uAPI
 * (mirrored by libdrm's own drmModeConnection enum in xf86drmMode.h)
 * but the enum itself is kernel-internal (drivers/gpu/drm/drm_connector.h),
 * not exposed by any uapi header -- so it is named here instead of
 * pulled from a header that does not carry it. */
#define ALP_DRM_CONNECTOR_CONNECTED 1u

#define Y_DISPLAY_PATH_MAX        32
#define Y_DISPLAY_BYTES_PER_PIXEL 4u /* DRM_FORMAT_ARGB8888 */

typedef struct {
	int      fd;
	bool     have_master;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t fb_id;
	uint32_t dumb_handle;
	uint8_t *map;
	uint64_t map_size;
	uint32_t pitch;
	uint16_t width;
	uint16_t height;
} y_display_data_t;

/**
 * @brief Copy an ARGB8888 rect from @p pixels into the mapped
 *        framebuffer @p fb, honouring @p fb_pitch (which can exceed
 *        `fb_w * 4` -- the kernel is free to pad each scanline).
 *
 * Pure logic, no ioctl/mmap -- factored out so it is unit-testable
 * against a plain heap buffer standing in for the real mmap()ed
 * dumb-buffer pointer (see tests/yocto/peripheral_display.c).
 *
 * @return ALP_ERR_INVAL for a zero-area rect; ALP_ERR_OUT_OF_RANGE if
 *         the rect falls outside [0, fb_w) x [0, fb_h); ALP_OK
 *         otherwise.
 */
static alp_status_t _blit_copy(uint8_t    *fb,
                               uint32_t    fb_pitch,
                               uint16_t    fb_w,
                               uint16_t    fb_h,
                               uint16_t    x,
                               uint16_t    y,
                               uint16_t    w,
                               uint16_t    h,
                               const void *pixels)
{
	if (fb == NULL || pixels == NULL) return ALP_ERR_INVAL;
	if (w == 0u || h == 0u) return ALP_ERR_INVAL;
	if ((uint32_t)x + w > (uint32_t)fb_w || (uint32_t)y + h > (uint32_t)fb_h) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	const uint8_t *src = (const uint8_t *)pixels;
	for (uint16_t row = 0; row < h; ++row) {
		memcpy(fb + (size_t)(y + row) * fb_pitch + (size_t)x * Y_DISPLAY_BYTES_PER_PIXEL,
		       src + (size_t)row * w * Y_DISPLAY_BYTES_PER_PIXEL,
		       (size_t)w * Y_DISPLAY_BYTES_PER_PIXEL);
	}
	return ALP_OK;
}

/**
 * @brief Fill the mapped framebuffer @p fb with black (all-zero
 *        bytes), honouring @p fb_pitch row-by-row -- see _blit_copy()'s
 *        doxygen for why this is pure/testable logic split out of the
 *        real op.
 */
static alp_status_t _clear_fill(uint8_t *fb, uint32_t fb_pitch, uint16_t fb_w, uint16_t fb_h)
{
	if (fb == NULL) return ALP_ERR_INVAL;
	for (uint16_t row = 0; row < fb_h; ++row) {
		memset(fb + (size_t)row * fb_pitch, 0, (size_t)fb_w * Y_DISPLAY_BYTES_PER_PIXEL);
	}
	return ALP_OK;
}

/** @brief Release every resource y_open() may have acquired, in
 *         reverse order.  Safe to call from a partially-populated @p d
 *         (open()'s own unwind path). */
static void _teardown(y_display_data_t *d)
{
	if (d->map != NULL && d->map_size != 0u) munmap(d->map, d->map_size);
	if (d->fb_id != 0u) ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &d->fb_id);
	if (d->dumb_handle != 0u) {
		struct drm_mode_destroy_dumb destroy = { .handle = d->dumb_handle };
		ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (d->have_master) ioctl(d->fd, DRM_IOCTL_DROP_MASTER, NULL);
	if (d->fd >= 0) close(d->fd);
	free(d);
}

/**
 * @brief Walk the connector list in @p res looking for a CONNECTED
 *        connector with >= 1 mode; fills @p out_connector_id and
 *        @p out_mode with its first (kernel-preferred) mode.
 *
 * @return ALP_OK on a match; ALP_ERR_NOT_READY if every connector is
 *         disconnected or mode-less (no panel plugged in / EDID
 *         didn't resolve a mode) -- an honest "nothing to drive"
 *         signal, not a fabricated success.
 */
static alp_status_t _find_connected_connector(int                       fd,
                                              const uint32_t           *connector_ids,
                                              uint32_t                  count,
                                              uint32_t                 *out_connector_id,
                                              uint32_t                 *out_encoder_id,
                                              struct drm_mode_modeinfo *out_mode)
{
	for (uint32_t i = 0; i < count; ++i) {
		/* Counting call: count_modes MUST be 1 (not 0) with modes_ptr
		 * pointing at one scratch element -- drm_mode.h's own doxygen
		 * for struct drm_mode_get_connector documents that count_modes
		 * == 0 triggers a slow, blocking, flicker-causing force-probe
		 * whenever the caller already holds DRM master (which we do
		 * by the time this runs).  count_props/count_encoders staying
		 * 0 is fine -- only modes has this force-probe side effect. */
		struct drm_mode_modeinfo scratch_mode;
		memset(&scratch_mode, 0, sizeof(scratch_mode));
		struct drm_mode_get_connector conn;
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connector_ids[i];
		conn.count_modes  = 1u;
		conn.modes_ptr    = (uint64_t)(uintptr_t)&scratch_mode;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
		if (conn.connection != ALP_DRM_CONNECTOR_CONNECTED || conn.count_modes == 0u) continue;

		uint32_t                  n_encoders = conn.count_encoders;
		uint32_t                  n_modes    = conn.count_modes;
		uint32_t                 *encoders = calloc(n_encoders ? n_encoders : 1u, sizeof(uint32_t));
		struct drm_mode_modeinfo *modes    = calloc(n_modes, sizeof(struct drm_mode_modeinfo));
		if (encoders == NULL || modes == NULL) {
			free(encoders);
			free(modes);
			return ALP_ERR_NOMEM;
		}

		memset(&conn, 0, sizeof(conn));
		conn.connector_id   = connector_ids[i];
		conn.count_encoders = n_encoders;
		conn.encoders_ptr   = (uint64_t)(uintptr_t)encoders;
		conn.count_modes    = n_modes;
		conn.modes_ptr      = (uint64_t)(uintptr_t)modes;
		int rc              = ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
		/* Re-check the kernel's counts against what we sized for.  If a
		 * hotplug grew them between the two calls, drm_mode_getconnector
		 * skips the copy entirely and only writes back the new count --
		 * leaving these calloc'd arrays all-zero while count_modes reads
		 * non-zero, so modes[0] would be an all-zero drm_mode_modeinfo
		 * and mode.hdisplay == 0 would propagate into create.width.
		 * (Today that fails safe only because drm_mode_create_dumb
		 * rejects width == 0 with EINVAL -- not something to rely on.) */
		if (rc < 0 || conn.count_modes == 0u || conn.count_modes > n_modes ||
		    conn.count_encoders > n_encoders) {
			free(encoders);
			free(modes);
			continue;
		}

		*out_connector_id = connector_ids[i];
		*out_mode         = modes[0];
		/* Prefer the connector's CURRENTLY bound encoder (conn.encoder_id);
		 * an unlit connector reports 0 there, so fall back to the first
		 * entry of its supported-encoder list. */
		*out_encoder_id = (conn.encoder_id != 0u) ? conn.encoder_id
		                  : (n_encoders != 0u)    ? encoders[0]
		                                          : 0u;
		free(encoders);
		free(modes);
		if (*out_encoder_id == 0u) continue; /* connected, moded, but no usable encoder */
		return ALP_OK;
	}
	return ALP_ERR_NOT_READY;
}

/**
 * @brief Pick the first CRTC from @p crtc_ids that @p possible_crtcs
 *        (the encoder's bitmask, one bit per index into @p crtc_ids)
 *        allows.
 *
 * @return ALP_OK with *out_crtc_id filled, or ALP_ERR_NOT_READY if no
 *         bit in @p possible_crtcs falls within @p count -- the
 *         encoder can drive no CRTC this card actually has.
 */
static alp_status_t
_pick_crtc(const uint32_t *crtc_ids, uint32_t count, uint32_t possible_crtcs, uint32_t *out_crtc_id)
{
	/* possible_crtcs is a 32-bit bitmask (kernel-defined uAPI width); a
	 * shift of 32+ is UB, but no real DRM device carries anywhere near
	 * 32 CRTCs -- guard it anyway rather than trust that invariant. */
	for (uint32_t i = 0; i < count && i < 32u; ++i) {
		if ((possible_crtcs & (1u << i)) != 0u) {
			*out_crtc_id = crtc_ids[i];
			return ALP_OK;
		}
	}
	return ALP_ERR_NOT_READY;
}

static alp_status_t y_open(const alp_display_config_t  *cfg,
                           alp_display_backend_state_t *state,
                           alp_capabilities_t          *caps_out)
{
	if (cfg == NULL || state == NULL || caps_out == NULL) return ALP_ERR_INVAL;

	/* Opening a display here MEANS taking it over: we become DRM master
	 * and SETCRTC reprograms the physical output.  The kernel only stops
	 * that when another process is *currently* master -- it does not stop
	 * it when master is merely unheld, which is the common case on an
	 * idle fbcon, a VT-switched-away Wayland/X session, or a headless SSH
	 * login on a machine with a panel attached.  A default-constructed
	 * config must not be able to blank someone's screen, so require the
	 * caller to say so.  Same posture as alp_storage_config_t's
	 * allow_unsafe_write (#1140). */
	if (!cfg->allow_modeset) return ALP_ERR_INVAL;

	char path[Y_DISPLAY_PATH_MAX];
	int  n = snprintf(path, sizeof(path), "/dev/dri/card%u", (unsigned)cfg->display_id);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	y_display_data_t *d = (y_display_data_t *)calloc(1u, sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;
	d->fd = open(path, O_RDWR | O_CLOEXEC);
	if (d->fd < 0) {
		alp_status_t rc = alp_status_from_posix_errno(errno);
		free(d);
		return rc;
	}

	/* Fail here, not five ioctls later.  A previous version stored this
	 * in a bool and carried on through GETRESOURCES / GETCONNECTOR /
	 * CREATE_DUMB / ADDFB2 / MAP_DUMB / mmap -- allocating and mapping a
	 * full-screen scanout buffer (~8 MB at 1080p) before SETCRTC finally
	 * refused.  Worse, it advertised that refusal as ALP_ERR_BUSY, which
	 * it never was: drm_setmaster_ioctl returns -EBUSY when another
	 * process holds master, SETCRTC returns -EACCES, and EACCES is not
	 * in alp_status_from_posix_errno()'s switch so it lands on
	 * ALP_ERR_IO. */
	if (ioctl(d->fd, DRM_IOCTL_SET_MASTER, NULL) != 0) {
		/* alp_status_from_posix_errno() already maps EBUSY -> ALP_ERR_BUSY
		 * (src/common/alp_errno.h:57-59); EACCES is the one it lacks, and
		 * that is SETCRTC's refusal, not this call's. */
		alp_status_t rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}
	d->have_master = true;

	struct drm_mode_card_res res;
	memset(&res, 0, sizeof(res));
	if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		alp_status_t rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}

	uint32_t *connectors =
	    calloc(res.count_connectors ? res.count_connectors : 1u, sizeof(uint32_t));
	uint32_t *crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1u, sizeof(uint32_t));
	if (connectors == NULL || crtcs == NULL) {
		free(connectors);
		free(crtcs);
		_teardown(d);
		return ALP_ERR_NOMEM;
	}

	uint32_t n_connectors = res.count_connectors;
	uint32_t n_crtcs      = res.count_crtcs;
	memset(&res, 0, sizeof(res));
	res.count_connectors = n_connectors;
	res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	res.count_crtcs      = n_crtcs;
	res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
	/* Hotplug between the two calls, both directions.  If the count GREW
	 * the kernel skipped the copy entirely and only wrote back the new
	 * count, leaving these arrays all-zero -- so refuse.  Keep errno's own
	 * mapping only for a genuine ioctl failure: on the count-mismatch
	 * branch the ioctl SUCCEEDED, errno is whatever an unrelated earlier
	 * libc call left behind (nothing in src/ ever clears it), and mapping
	 * that would report e.g. ALP_ERR_NOT_READY for a hotplug race. */
	if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		alp_status_t rc = alp_status_from_posix_errno(errno);
		free(connectors);
		free(crtcs);
		_teardown(d);
		return rc;
	}
	if (res.count_connectors > n_connectors || res.count_crtcs > n_crtcs) {
		free(connectors);
		free(crtcs);
		_teardown(d);
		return ALP_ERR_IO;
	}
	/* If the count SHRANK the kernel copied fewer ids and the tail of each
	 * array is still calloc-zero.  Walking the pre-call length would hand
	 * object id 0 to GETCONNECTOR, and could return crtc_id = 0 into
	 * SETCRTC -- safe today only because the kernel rejects id 0, which is
	 * exactly the reliance the growth branch above refuses to make.  Use
	 * what the kernel actually reported. */
	n_connectors = res.count_connectors;
	n_crtcs      = res.count_crtcs;
	if (n_connectors == 0u || n_crtcs == 0u) {
		free(connectors);
		free(crtcs);
		_teardown(d);
		return ALP_ERR_NOT_READY;
	}

	uint32_t                 connector_id = 0, encoder_id = 0;
	struct drm_mode_modeinfo mode;
	memset(&mode, 0, sizeof(mode));
	alp_status_t rc = _find_connected_connector(
	    d->fd, connectors, n_connectors, &connector_id, &encoder_id, &mode);
	free(connectors);
	if (rc != ALP_OK) {
		free(crtcs);
		_teardown(d);
		return rc;
	}

	struct drm_mode_get_encoder enc;
	memset(&enc, 0, sizeof(enc));
	enc.encoder_id = encoder_id;
	if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0) {
		rc = alp_status_from_posix_errno(errno);
		free(crtcs);
		_teardown(d);
		return rc;
	}

	uint32_t crtc_id = 0;
	/* Prefer the encoder's already-bound CRTC (enc.crtc_id) so we don't
	 * steal a CRTC another encoder is actively driving; only fall back
	 * to scanning possible_crtcs when nothing is bound yet. */
	if (enc.crtc_id != 0u) {
		crtc_id = enc.crtc_id;
	} else {
		rc = _pick_crtc(crtcs, n_crtcs, enc.possible_crtcs, &crtc_id);
	}
	free(crtcs);
	if (rc != ALP_OK) {
		_teardown(d);
		return rc;
	}

	struct drm_mode_create_dumb create;
	memset(&create, 0, sizeof(create));
	create.width  = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp    = Y_DISPLAY_BYTES_PER_PIXEL * 8u;
	if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}
	d->dumb_handle = create.handle;
	d->pitch       = create.pitch;

	struct drm_mode_fb_cmd2 fb;
	memset(&fb, 0, sizeof(fb));
	fb.width        = mode.hdisplay;
	fb.height       = mode.vdisplay;
	fb.pixel_format = DRM_FORMAT_ARGB8888;
	fb.handles[0]   = d->dumb_handle;
	fb.pitches[0]   = d->pitch;
	if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0) {
		rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}
	d->fb_id = fb.fb_id;

	struct drm_mode_map_dumb map_req;
	memset(&map_req, 0, sizeof(map_req));
	map_req.handle = d->dumb_handle;
	if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
		rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}
	d->map_size = create.size;
	void *m =
	    mmap(NULL, d->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, (off_t)map_req.offset);
	if (m == MAP_FAILED) {
		rc          = alp_status_from_posix_errno(errno);
		d->map_size = 0u; /* nothing mapped -- _teardown() must not munmap() garbage */
		_teardown(d);
		return rc;
	}
	d->map = (uint8_t *)m;

	struct drm_mode_crtc set;
	memset(&set, 0, sizeof(set));
	set.set_connectors_ptr = (uint64_t)(uintptr_t)&connector_id;
	set.count_connectors   = 1u;
	set.crtc_id            = crtc_id;
	set.fb_id              = d->fb_id;
	set.mode               = mode;
	set.mode_valid         = 1u;
	if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
		rc = alp_status_from_posix_errno(errno);
		_teardown(d);
		return rc;
	}

	d->crtc_id      = crtc_id;
	d->connector_id = connector_id;
	d->width        = mode.hdisplay;
	d->height       = mode.vdisplay;

	state->be_data  = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t y_get_caps(alp_display_backend_state_t *state, alp_display_caps_t *out)
{
	y_display_data_t *d = (y_display_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	out->width  = d->width;
	out->height = d->height;
	out->format = ALP_PIXFMT_ARGB8888;
	return ALP_OK;
}

static alp_status_t y_blit(alp_display_backend_state_t *state,
                           uint16_t                     x,
                           uint16_t                     y,
                           uint16_t                     w,
                           uint16_t                     h,
                           const void                  *pixels)
{
	y_display_data_t *d = (y_display_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	return _blit_copy(d->map, d->pitch, d->width, d->height, x, y, w, h, pixels);
}

static alp_status_t y_clear(alp_display_backend_state_t *state)
{
	y_display_data_t *d = (y_display_data_t *)state->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	return _clear_fill(d->map, d->pitch, d->width, d->height);
}

/** @brief Tear the connection down -- see the file-header note on why
 *         this does NOT restore the pre-open CRTC state. */
static void y_close(alp_display_backend_state_t *state)
{
	y_display_data_t *d = (y_display_data_t *)state->be_data;
	if (d != NULL) {
		_teardown(d);
		state->be_data = NULL;
	}
}

static const alp_display_ops_t _ops = {
	.open     = y_open,
	.get_caps = y_get_caps,
	.blit     = y_blit,
	.clear    = y_clear,
	.close    = y_close,
};

ALP_BACKEND_REGISTER(display,
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
