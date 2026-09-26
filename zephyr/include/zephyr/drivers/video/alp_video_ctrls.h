/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alp SDK-owned, cross-sensor Zephyr video CIDs.  V4L2/Zephyr's private-CID
 * convention (VIDEO_CID_PRIVATE_BASE, see <zephyr/drivers/video-controls.h>)
 * reserves a range for controls no standard V4L2 class defines.
 *
 * UNIQUENESS SCOPE IS THE WHOLE DEVICE CHAIN, NOT ONE DEVICE.  Zephyr v4.4's
 * `video_find_ctrl()` (`video_ctrls.c`) walks `src_dev` up through every link
 * an m2m device declares (ISP -> CAM -> CSI -> sensor, e.g.
 * `isp_pico.c`'s own `REMOTE_DEVICE` chase) looking for the first device
 * whose OWN registry has the requested CID -- so a `video_set_ctrl(dev, ...)`
 * call on the ISP's device handle can still land on the sensor several links
 * away. Two links in the SAME chain reusing one private-range value are
 * therefore NOT independent the way video_alif.h's own VIDEO_CID_ALIF_*
 * block is (those are scoped to one specific controller device with nothing
 * upstream/downstream of it walking the same numeric space) -- picking a
 * value already used by another device on a chain this control might also
 * traverse resolves to whichever of the two comes first in the walk, not the
 * caller's intended target. Every macro in this header therefore lives in
 * its own dedicated sub-range (`VIDEO_CID_PRIVATE_BASE + 0x1000` and up),
 * clear of `video_alif.h`'s `VIDEO_CID_PRIVATE_BASE + 0..4` block that the
 * AEN camera/ISP chain also walks.
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
#define VIDEO_CID_ALP_TRIGGER_MODE (VIDEO_CID_PRIVATE_BASE + 0x1000)

#define VIDEO_ALP_TRIGGER_MODE_FREE_RUN 0
#define VIDEO_ALP_TRIGGER_MODE_EXTERNAL 1

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VIDEO_ALP_VIDEO_CTRLS_H_ */
