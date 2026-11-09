# SPDX-License-Identifier: Apache-2.0
SUMMARY = "Example product image on the Alp SDK base image"

# alp-image-base is the composition primitive: Alp SDK runtime, the silicon's
# NPU runtime, OTA client, watchdog. Do NOT extend core-image-* from here: the
# vendor core-image-%.bbappend files do not reach alp-image-* recipes.
require recipes-images/alp-image-base.bb

# Product packages go here.
IMAGE_INSTALL:append = " nano"

# Opt in to the multimedia packagegroup once it is in meta-alp-sdk
# (set ALP_PRODUCT_MULTIMEDIA = "1" in local.conf). Its commercial-flagged
# members also need LICENSE_FLAGS_ACCEPTED, see docs/v2n-custom-carrier.md.
ALP_PRODUCT_MULTIMEDIA ??= "0"
IMAGE_INSTALL:append = "${@' packagegroup-alp-multimedia' if d.getVar('ALP_PRODUCT_MULTIMEDIA') == '1' else ''}"
