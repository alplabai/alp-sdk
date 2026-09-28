.. _raspberry_pi_global_shutter_camera_shield:

Raspberry Pi Global Shutter Camera (IMX296)
############################################

Overview
********

The Raspberry Pi Global Shutter Camera (and the compatible InnoMaker
CAM-IMX296RAW-TRIGGER module) carries a Sony IMX296LQR-C colour
global-shutter MIPI CSI-2 image sensor on the standard 15-pin RPi camera
connector. This shield wires it in the same board-agnostic shape as
upstream's ``raspberry_pi_camera_module_2`` shield: a ``fixed-clock`` at
54 MHz feeds the sensor's INCK, and the sensor's endpoint is left to be
paired with whichever carrier connector shield defines the
``csi_interface`` / ``csi_ep_in`` / ``csi_i2c`` / ``csi_capture_port``
labels.

Unlike the sibling ``raspberry_pi_camera_module_1`` (OV5647) and
``innomaker_cam_ov9281`` shields, which are both 2-lane parts, the IMX296
silicon has a single MIPI CSI-2 data lane -- this shield sets
``data-lanes = <1>`` on both the sensor endpoint and the ``&csi_ep_in``
override.

On alp-sdk that carrier connector shield is the E1M-EVK's
``e1m_evk_rpi_csi`` shield.

The driver offers the sensor's one All-pixel scan mode: 1456x1088 RAW10
(``SRGGB10P``), the frame the sensor actually transmits. That is the
1440x1080 recommended recording area plus the 8-column / 4-row
colour-processing margin around it, which the sensor sends rather than crops.
A 1440x1080 image is a crop the application takes from that frame.

.. important::
   The 54 MHz ``fixed-clock`` in this shield's overlay must match the actual
   crystal on the camera module in hand. The Sony IMX296 accepts
   37.125 MHz, 54 MHz or 74.25 MHz INCK (see
   ``zephyr/drivers/video/imx296.c``); the Raspberry Pi Global Shutter
   Camera module ships a 54 MHz oscillator, which is what this shield
   assumes. A different InnoMaker GS camera batch or a hand-built module
   with a different crystal requires changing ``clock-frequency`` in this
   overlay to match -- the driver rejects any other INCK frequency at
   runtime (``-ENOTSUP``) rather than silently mis-clocking the sensor.

.. note::
   Bench status (issue #2287, an E1M-AEN803 on the E1M-EVK): the sensor's
   I2C identity is silicon-verified (bench run 229) -- a real INNO-MAKER
   CAM-IMX296RAW-TRIGGER module answers at CCI address 0x1A and the
   driver's SENSOR_INFO probe reads back the expected colour-IMX296LQR-C
   signature. On a later bench pass, on the same silicon, the sensor also
   streams: the CSI-2 host's frame counter advances once master-mode
   free-run starts. **No frame has been captured yet -- the Alif IPI emits
   no line.** This sensor has no known register to park its CSI-2 CLOCK
   lane at LP-11 short of full streaming (unlike OV5647), so the CSI-2
   host's D-PHY Stop-state wait skips only that one lane's check for this
   sensor via a new DT property, ``no-lp11-clock-lane-park`` -- an
   inference from the datasheet, not a bench measurement of the D-PHY's
   Stop-state register. See ``docs/camera-shields.md``.

Requirements
************

A board that provides a carrier connector shield defining ``csi_interface``,
``csi_ep_in``, ``csi_i2c`` and ``csi_capture_port``.

Programming
***********

Set ``-DSHIELD="e1m_evk_rpi_csi raspberry_pi_global_shutter_camera"``
(carrier shield first, camera shield second) when building for a board that
provides the carrier connector shield.
