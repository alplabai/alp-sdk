# Unversioned: poky ships alsa-state.bb (no _<version>), which a
# `alsa-state_%.bbappend` would not match (dangling append).
# E1M-X-EVK TAS2563 mixer defaults (ASI1 Sel Left/Right, lowest amp
# level). Carrier-specific like tas2563-audio.cfg in the kernel bbappend,
# so it is scoped to the E1M-X-EVK machines rather than every build.
# e1m-v2n101 is carried by every V2N-family machine, V2M included.
FILESEXTRAPATHS:prepend:e1m-v2n101 := "${THISDIR}/files:"
