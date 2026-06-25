# SPDX-License-Identifier: Apache-2.0
#
# Bakes the aen-a32-carrier-bringup demo (examples/aen/) into the
# image.  Builds against the staged alp-sdk runtime + chip drivers;
# the rootfs only needs libalp_sdk.so (the chip lib links static).

SUMMARY = "E1M-AEN801 A32 Linux carrier bring-up example"
DESCRIPTION = "User-space bring-up smoke test for the E1M-EVK carrier \
               peripherals over the portable alp_* API (i2c-dev + \
               gpiochip): bus scan, tcal9538 I/O-expander, IMU chip-id, \
               and SoC GPIO."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

# The example sources currently live only on feat/aen-a32-yocto-bringup;
# main has no examples/aen/aen-a32-carrier-bringup/ yet, so pointing at
# branch=main would leave S empty and break the bake.  This pin is
# temporary: once the feature branch merges, repoint to branch=main (matching
# the alp-sdk recipe in recipes-core/alp-sdk/alp-sdk_0.6.bb) so the
# ${AUTOREV} fetch keeps working after the feature branch is deleted.
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=feat/aen-a32-yocto-bringup"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

# Build only the example subdir; it consumes the SDK from the sysroot.
S = "${WORKDIR}/git/examples/aen/aen-a32-carrier-bringup"

DEPENDS = "alp-sdk alp-chips"
RDEPENDS:${PN} = "alp-sdk"

inherit cmake

FILES:${PN} += "${bindir}/aen-a32-carrier-bringup"

# AEN A32 only -- the example is wired to the E1M-EVK on this machine.
COMPATIBLE_MACHINE = "e1m-aen801-a32"
