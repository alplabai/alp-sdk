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
``scripts/verify_west_patches.py``). Bench runs 316-331 (E1M-AEN803
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
   storm at 1188 Mbps/lane (bench run 318); with it, 0 CRC errors (runs
   319-330);
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
   **Bench-verified on unit E1M-AEN803 2026W36-0001 (bench runs 316-331 --
   see** ``metadata/chips/imx335.yaml`` **and** ``changelog.d/2327.md``
   **for the full history):** with the patch above applied, and this
   shield's Camera-mode + explicit ``csi-pixclk-hz`` = 200 MHz
   configuration (see the overlay's own comment for why Camera mode, not
   Controller mode), 6/6 consecutive clean 1296x972 RAW10 raw captures (run
   330): 0 CSI CRC errors, 0 IPI-fatal events, correct stride, no overrun.
   **This VERIFIES 2-lane CSI-2 D-PHY lock at 1188 Mbps/lane on this unit
   and this E1M-EVK, through the SoM R2 pinout adapter** (runs 319-330).
   D-PHY Stop-state check (``STOPSTATE 0x00010003`` before stream start)
   passed normally once the patch's MIPI-timing registers were written --
   no ``no-lp11-clock-lane-park``-style skip property was needed. The
   module answered at CCI 0x1A (the only device on the J5 camera bus) with
   CAM_EN (J5 pin 11) untouched -- that bench unit's E1M-EVK carries the J5
   pin-11 pull-up rework (``docs/boards/e1m-evk.md``, originally fitted for
   OV5647), so whether this module self-enables on a STOCK carrier (no
   rework) is NOT established by this result; do not add IMX335 to any
   "self-enables" list on the strength of it. The first post-start frame
   was bad in 3 of the 4 first frames checked (run 329 -- darkened lower
   rows -- and both of run 331's loads -- near-black rows 466-583;
   all-zero rows 759-778; run 330's own first frame was clean); the kept
   second frame was clean every time. This example discards the first
   frame for this sensor rather than rely on a clean run happening again.

   **Run 331 -- PRODUCT-CODE confirmation:** the ``fbf9c8de3`` tree with
   ONLY patch 0004 applied on a pristine Zephyr v4.4.1 checkout, no
   diag-only code. Run 331 applied patch 0004 at sha256
   ``2459cd9511dac95fa682197302b1d33b201dbacd3d4df02c2097e53dbd5278c0``
   (the ``fbf9c8de3`` file); the committed patch file was regenerated
   afterward (sha256
   ``47d7d5ac6dd2293b3ffb46562f031a543867a618b0da7dbe596c83ca609a400d``)
   and differs from what run 331 tested ONLY in comments -- no register
   writes, ordering, or logic changed. The kept (second) frame was clean: 0 IPI/CRC error lines, only one
   ``FRAME_SEQ`` event at start, correct stride, 0 near-black rows, nothing
   written past the buffer, a clean close, and ``CSI_PIXCLK_CTRL`` read
   back ``0x00020001`` (divisor 2 = 200 MHz, matching this shield's
   ``csi-pixclk-hz``). ``csi-halt-en`` was left at its default -- no
   IPI-halt/"nohalt" DT override was needed.

   **NOT bench-verified -- never claim beyond the above:**

   - frame rate/fps (not measured on this path);
   - the module self-enabling on a stock, non-reworked carrier (the J5
     pin-11 rework is a module-enable concern, unrelated to D-PHY lock);
   - D-PHY lock on any unit/carrier other than E1M-AEN803 2026W36-0001 on
     this specific E1M-EVK;
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
