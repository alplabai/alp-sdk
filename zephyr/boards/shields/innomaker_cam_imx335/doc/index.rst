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
``drivers/video/imx335.c`` (``CONFIG_VIDEO_IMX335``) as-is (ADR 0017 Tier 1,
upstream-native) -- there is no alp-sdk-vendored IMX335 driver. One repo
patch applies on top of it,
``zephyr/patches/zephyr/0004-imx335-2lane-link-freq-and-binning.patch``
(registered in ``zephyr/patches.yml``, applied per-module by
``scripts/bootstrap.sh`` via ``west patch apply``, and checked by
``scripts/verify_west_patches.py``). Bench runs 316-330 (E1M-AEN803
2026W36-0001) found and fixed three real upstream-driver bugs at this
sensor's 2-lane/10-bit/1188 Mbps-per-lane configuration -- see the patch's
own comment in ``zephyr/patches.yml`` for the full derivation and bench
evidence:

1. registers ``VIDEO_CID_LINK_FREQ`` (the driver had none), so the DW CSI-2
   host programs the sensor's real 594 MHz link frequency instead of
   mis-approximating it from ``VIDEO_CID_PIXEL_RATE`` (left untouched --
   396 MHz is the correct v4l2 pixel-array-clock value);
2. writes the ``SYSMODE``/MIPI output-timing registers this lane count and
   bit rate need (the driver never wrote them for ANY configuration) --
   without this, the CSI-2 receiver logged a payload-checksum/CRC error
   storm at 1188 Mbps/lane (bench run 318); with it, 0 CRC errors (runs 319
   onward);
3. fixes the 2x2-binned mode's ``HNUM``/``Y_OUT_SIZE`` -- upstream left
   ``HNUM`` at its full-resolution value, so the sensor transmitted a
   1308x984 frame into this driver's own 1296x972 buffer (a 12-pixel/
   12-line overrun corrupting memory past the buffer).

**If you already have a Zephyr checkout with alp-sdk's 0001-0003 zephyr
patches applied**, re-running the whole bootstrap will fail applying 0001
again (``west patch`` is not idempotent against an already-patched tree) --
apply 0004 by hand instead: either start from a clean Zephyr checkout, or
apply just the new patch with ``git apply
zephyr/patches/zephyr/0004-imx335-2lane-link-freq-and-binning.patch`` from
the Zephyr repo root. **Without 0004 applied, this shield's D-PHY runs at
the wrong link frequency (990 MHz instead of 594 MHz), the CSI-2 receiver
logs an ERRSOTSYNCHS/PHY_FATAL error storm even before that (missing
SYSMODE/MIPI timing), and the 2x2-binned mode overruns its capture buffer**
-- if you build this shield against an unpatched Zephyr tree, expect all
three symptoms, not silicon damage.

Issue #2327: raw capture only through ``<alp/camera.h>``, no ISP/AE/
streaming. The driver boots at its native 2592x1944; this shield's example
(``examples/aen/aen-camera-firstlight``) requests the 2x2-binned 1296x972
mode explicitly, and discards the first captured frame after stream start
(see that example's ``src/main.c``).

.. important::
   The 24 MHz ``fixed-clock`` in this shield's overlay must match the
   actual crystal on the camera module in hand -- the IMX335 driver accepts
   6/18/24/27/74 MHz INCK (``imx335_set_input_clk()``,
   ``drivers/video/imx335.c``); 24 MHz is the value that INNO-MAKER's own
   CAM-IMX335-5MP module ships.

.. note::
   **Bench-verified on unit E1M-AEN803 2026W36-0001 (bench runs 316-330 --
   see** ``metadata/chips/imx335.yaml`` **and** ``changelog.d/2327.md``
   **for the full history):** with the patch above applied, and this
   shield's Camera-mode + explicit ``csi-pixclk-hz`` = 200 MHz
   configuration (see the overlay's own comment for why Camera mode, not
   Controller mode), 6/6 consecutive clean 1296x972 RAW10 raw captures (run
   330): 0 CSI CRC errors, 0 IPI-fatal events, correct stride, no overrun.
   D-PHY Stop-state check (``STOPSTATE 0x00010003`` before stream start)
   passed normally once the patch's MIPI-timing registers were written --
   no ``no-lp11-clock-lane-park``-style skip property was needed. The
   module answered at CCI 0x1A (the only device on the J5 camera bus) with
   CAM_EN (J5 pin 11) untouched -- that bench unit's E1M-EVK carries the J5
   pin-11 pull-up rework (``docs/boards/e1m-evk.md``, originally fitted for
   OV5647), so whether this module self-enables on a STOCK carrier (no
   rework) is NOT established by this result; do not add IMX335 to any
   "self-enables" list on the strength of it. The first captured frame
   after STANDBY release had darkened lower rows in one run (329) but not
   the next (330) -- this example discards the first frame for this
   sensor rather than claiming the cause is understood.

   **NOT bench-verified -- never claim beyond the above:**

   - frame rate/fps (not measured on this path);
   - CSI-2 D-PHY lock at 1188 Mbps/lane through the E1M SoM R2 pinout
     adapter on a 2-lane sensor specifically -- 1188 Mbps/lane itself IS
     bench-proven, but only on data lane D0 (IMX296, 1 lane, run 292); D1
     through the adapter is untested;
   - ISP/AE/colour (no ISP path exists for this sensor);
   - the sensor's full-resolution (non-binned) mode.

Requirements
************

A board that provides a carrier connector shield defining ``csi_interface``,
``csi_ep_in``, ``csi_i2c`` and ``csi_capture_port``.

Programming
***********

Set ``-DSHIELD="e1m_evk_rpi_csi innomaker_cam_imx335"`` (carrier shield
first, camera shield second) when building for a board that provides the
carrier connector shield.
