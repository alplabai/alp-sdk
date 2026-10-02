# SPDX-License-Identifier: Apache-2.0
"""Probe-based power measurement (alp-sdk#405).

Drives the vendor commands of the on-board RP2040 CMSIS-DAP probe to stream
power-monitor readings alongside a target GPIO marker, then turns the stream
into per-rail energy-per-inference numbers.  Layers: transport (USB + wire
encoding), monitors (part decoders), capture (device -> records), analysis
(pure maths), cli.
"""
