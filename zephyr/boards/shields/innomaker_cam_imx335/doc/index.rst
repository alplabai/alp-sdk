.. _innomaker_cam_imx335_shield:

InnoMaker CAM-IMX335-5MP
#########################

Overview
********

The InnoMaker CAM-IMX335-5MP carries a Sony IMX335 5 Mpx MIPI CSI-2 image
sensor on the standard 15-pin RPi camera connector. This shield wires it in
the same board-agnostic shape as upstream's ``raspberry_pi_camera_module_2``
shield: a ``fixed-clock`` at 24 MHz feeds the sensor's INCK, and the
sensor's endpoint is left to be paired with whichever carrier connector
shield defines the ``csi_interface`` / ``csi_ep_in`` / ``csi_i2c`` /
``csi_capture_port`` labels.

On alp-sdk that carrier connector shield is the E1M-EVK's
``e1m_evk_rpi_csi`` shield.

Driver: this shield reuses upstream Zephyr v4.4.1's
``drivers/video/imx335.c`` (``CONFIG_VIDEO_IMX335``) as-is -- there is no
alp-sdk-vendored IMX335 driver. One repo patch applies on top of it,
``zephyr/patches/zephyr/0004-imx335-link-freq.patch`` (registered in
``zephyr/patches.yml``): it registers ``VIDEO_CID_LINK_FREQ`` and corrects
``VIDEO_CID_PIXEL_RATE`` so the DW CSI-2 host (``video_csi_dw.c``) programs
the sensor's real 594 MHz link frequency instead of mis-approximating it
from the upstream driver's uncorrected pixel rate -- see the patch's own
comment in ``zephyr/patches.yml`` for the full derivation.

Issue #2327, Stage A only: raw first-light capture through
``<alp/camera.h>``, no ISP/AE/streaming. The driver boots at its native
2592x1944; this shield's example (``examples/aen/aen-camera-firstlight``)
requests the 2x2-binned 1296x972 mode explicitly.

.. important::
   The 24 MHz ``fixed-clock`` in this shield's overlay must match the
   actual crystal on the camera module in hand -- the IMX335 driver accepts
   6/18/24/27/74 MHz INCK (``imx335_set_input_clk()``,
   ``drivers/video/imx335.c``); 24 MHz is the value that INNO-MAKER's own
   CAM-IMX335-5MP module ships.

.. note::
   **Silicon facts established on unit E1M-AEN803 2026W36-0001 (I2C
   identity probe ONLY -- see** ``metadata/chips/imx335.yaml``\
   **):** the module answers at CCI 0x1A (the only device on the J5 camera
   bus), and its registers read back at power-on-reset defaults (STANDBY
   0x3000=0x01, VMAX 0x3030-0x3032=0x001194=4500, HMAX 0x3034=0x0226=550,
   LANEMODE 0x3a01=0x03). The module self-powers -- CAM_EN (J5 pin 11) is
   untouched, same as the IMX296 shield's own note on that pin.

   **NOT bench-verified -- never claim beyond the I2C probe above:**

   - the sensor achieving CSI-2 D-PHY lock at 1188 Mbps/lane through the
     E1M SoM R2 pinout adapter (the highest measured link rate on any
     sensor on this bus to date is 875 Mbps, on a different sensor);
   - this shield's Controller-mode IPI timing (``csi-pixclk-hz``,
     ``csi-hsa``/``csi-hbp``/``csi-hline``/``csi-vsa``/``csi-vbp``/
     ``csi-vtotal`` on the ``&csi`` node) -- its derivation is INFERRED
     from the sensor's own read-back HMAX/VMAX register values and an
     ASSUMED 2x2-binned-mode line pacing, not a datasheet MIPI-timing-table
     citation (unlike the IMX296 shield's own derivation, which does cite
     one) -- see this shield's overlay for the full derivation and its own
     fallback note;
   - LP-11 clock-lane-park behaviour (``no-lp11-clock-lane-park`` is left
     unset pending a bench result, not because the sensor is known not to
     need it);
   - any frame ever captured through this shield.

Requirements
************

A board that provides a carrier connector shield defining ``csi_interface``,
``csi_ep_in``, ``csi_i2c`` and ``csi_capture_port``.

Programming
***********

Set ``-DSHIELD="e1m_evk_rpi_csi innomaker_cam_imx335"`` (carrier shield
first, camera shield second) when building for a board that provides the
carrier connector shield.
