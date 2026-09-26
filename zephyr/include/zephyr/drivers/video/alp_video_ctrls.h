/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SDK-owned, cross-sensor Zephyr video CIDs.  V4L2/Zephyr's private-CID
 * convention (VIDEO_CID_PRIVATE_BASE, see <zephyr/drivers/video-controls.h>)
 * reserves a range for controls no standard V4L2 class defines; each CID
 * value only needs to be unique WITHIN one device's own control registry
 * (video_init_ctrl() / video_set_ctrl() key off (dev, id) together), so two
 * unrelated sensor drivers may reuse the same private-range value without
 * colliding -- see video_alif.h's own VIDEO_CID_ALIF_* block for the
 * precedent this header follows.
 *
 * This header exists so a control every <alp/camera.h> Zephyr-video backend
 * (zephyr_video.c / alif_isp_pico.c / v2n_n44_isp.c) drives on a caller's
 * behalf has ONE name, rather than each sensor driver and each backend
 * redefining the same numeric value under a different macro (the trap
 * IMX296_CID_TRIGGER_MODE's driver-private definition in imx296.c used to
 * force on every consumer -- issue #2287). A sensor driver that supports a
 * control this header names should register it under this exact CID, not a
 * driver-private one of its own.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_VIDEO_ALP_VIDEO_CTRLS_H_
#define ZEPHYR_INCLUDE_DRIVERS_VIDEO_ALP_VIDEO_CTRLS_H_

#include <zephyr/drivers/video-controls.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Free-run vs external-trigger exposure mode.  Backs the portable
 * <alp/camera.h> alp_camera_set_trigger_mode() / alp_camera_trigger_t API --
 * a backend that reaches a sensor implementing this CID sets it directly on
 * that sensor's Zephyr video device with a plain struct video_control.
 *
 * Values are numerically identical to alp_camera_trigger_t on purpose (0 =
 * free-run, 1 = external), so a backend can pass the portable enum's value
 * straight through as this control's .val with no translation table.
 *
 * First implementor: zephyr/drivers/video/imx296.c (Sony IMX296 fast
 * trigger mode, "Mode Transitions of Global Shutter Operation" / page 66 --
 * the switch can only be made via sensor standby, so a sensor implementing
 * this CID is expected to reject a write with -EBUSY while streaming,
 * exactly like IMX296_CID_TRIGGER_MODE did before this header existed).
 * A sensor with no trigger input at all simply never registers this CID;
 * video_set_ctrl() then returns -ENOTSUP, which every backend maps to
 * ALP_ERR_NOSUPPORT.
 */
#define VIDEO_CID_ALP_TRIGGER_MODE (VIDEO_CID_PRIVATE_BASE + 0x01)

#define VIDEO_ALP_TRIGGER_MODE_FREE_RUN 0
#define VIDEO_ALP_TRIGGER_MODE_EXTERNAL 1

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VIDEO_ALP_VIDEO_CTRLS_H_ */
