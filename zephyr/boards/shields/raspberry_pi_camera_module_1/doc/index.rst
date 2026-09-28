.. _raspberry_pi_camera_module_1_shield:

Raspberry Pi Camera Module 1 (OV5647)
######################################

Overview
********

The Raspberry Pi Camera Module 1 carries an OmniVision OV5647 5 Mpx raw
Bayer MIPI CSI-2 image sensor on the standard 15-pin RPi camera connector.
This shield wires it in the same board-agnostic shape as upstream's
``raspberry_pi_camera_module_2`` shield: a ``fixed-clock`` at 25 MHz feeds
the sensor's XVCLK, and the sensor's endpoint is left to be paired with
whichever carrier connector shield defines the ``csi_interface`` /
``csi_ep_in`` / ``csi_i2c`` / ``csi_capture_port`` labels.

On alp-sdk that carrier connector shield is the E1M-EVK's
``e1m_evk_rpi_csi`` shield.

Requirements
************

A board that provides a carrier connector shield defining ``csi_interface``,
``csi_ep_in``, ``csi_i2c`` and ``csi_capture_port``.

Programming
***********

Set ``-DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_1"`` (carrier
shield first, camera shield second) when building for a board that provides
the carrier connector shield.
