# SPDX-License-Identifier: Apache-2.0
#
# RZ/V2N (and the rest of the E1M SoM family) has no Raspberry Pi PiSP,
# Intel IPU3, or Rockchip RkISP1 imaging hardware.  libcamera's default
# `pipelines=auto` builds the rpi/pisp handler, which pulls `libpisp`
# via a meson wrap subproject -- wrap downloading is disabled in Yocto,
# so do_configure fails ("Automatic wrap-based subproject downloading is
# disabled").  Restrict libcamera to the pipelines that are relevant and
# dependency-light on this SoC family:
#   - simple   : generic media-controller SoC cameras (RZ/V CRU + CSI-2)
#   - uvcvideo : USB webcams
#   - vimc     : virtual test pipeline (HW-less bring-up)
# meson array option -> comma-separated, no spaces (the recipe substitutes
# this verbatim into -Dpipelines=, so spaces would become stray meson args).
LIBCAMERA_PIPELINES = "simple,uvcvideo,vimc"
