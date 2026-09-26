.. _e1m_evk_rpi_csi_shield:

E1M-EVK Raspberry Pi camera connector (J5)
##########################################

Overview
********

J5 on the E1M-EVK is the Raspberry Pi 15-pin MIPI CSI-2 camera connector.
This shield is the board half of Zephyr's Raspberry Pi camera-shield
contract: it defines the ``csi_interface`` / ``csi_ep_in`` / ``csi_i2c`` /
``csi_capture_port`` labels that a sensor shield such as upstream's
``raspberry_pi_camera_module_2`` consumes, and points the ``alp-camera0``
alias at the capture device.

J5 sits on input A of the carrier's PI3WVR626 2:1 CSI mux, whose SEL line is
E1M ``IO2``; the shield hogs it low to select J5. The connector carries two
data lanes, no reset line and no sensor clock (Raspberry Pi modules carry
their own oscillator). Its control I2C is E1M ``I2C1``, shared with the DSI
connector's touch controller.

The labels map onto SoC nodes and pins, so each supported SoM target has its
own overlay under ``boards/``. The E1M-AEN HE targets share
``boards/e1m_aen.dtsi``: the CSI-2 host on the E8's dedicated receive D-PHY,
SoC ``I2C1`` for the sensor bus, and the CPI as the capture device.

Requirements
************

An E1M-EVK carrying a SoM target this shield has a ``boards/`` overlay for:

- ``alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he``
- ``alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he``

Keep the carrier's ``CAM_EN`` expander output at 0 (its reset default):
driving it to 1 pulls J5 pin 11 low and powers the module down.

E1M-AEN hw_rev r2 (2626-R2)'s camera connector wiring swaps the P and N wires of all
three MIPI CSI-2 differential pairs (clock lane and both data lanes)
relative to the EVK. A camera plugged straight into J5 answers its I2C
chip-ID probe but never synchronizes (D-PHY leaves Stop-state, no frame
arrives). Build a short adapter that crosses camera-connector pins 2<->3,
5<->6 and 8<->9 (every other pin stays straight); match lane lengths given
the 800 Mbit/s/lane rate. See ``docs/camera-shields.md``.

Programming
***********

Set ``-DSHIELD="e1m_evk_rpi_csi innomaker_cam_ov9281"`` (carrier shield
first, camera shield second). Any sensor shield that follows the same
label contract works in place of ``innomaker_cam_ov9281``, e.g. upstream's
``raspberry_pi_camera_module_2``.
