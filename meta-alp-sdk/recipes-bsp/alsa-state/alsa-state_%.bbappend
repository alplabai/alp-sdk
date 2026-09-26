# E1M-X-EVK TAS2563 mixer defaults (ASI1 Sel Left/Right, lowest amp
# level). Carrier-specific like tas2563-audio.cfg in the kernel bbappend,
# so it is scoped to the E1M-X-EVK machines rather than every build.
FILESEXTRAPATHS:prepend:e1m-v2n101 := "${THISDIR}/files:"
FILESEXTRAPATHS:prepend:e1m-v2m101 := "${THISDIR}/files:"
