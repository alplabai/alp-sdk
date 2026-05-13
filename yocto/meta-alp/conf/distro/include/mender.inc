# SPDX-License-Identifier: Apache-2.0
#
# Mender OTA distro config for meta-alp targets.
#
# Opt-in.  Pull in from a machine .conf via:
#
#     require conf/distro/include/mender.inc
#
# Or from local.conf for one-off builds:
#
#     INHERIT += "mender-full"
#     require conf/distro/include/mender.inc
#
# Requires the `meta-mender-core` layer in bblayers.conf:
#
#     git clone -b scarthgap https://github.com/mendersoftware/meta-mender
#     bitbake-layers add-layer ../meta-mender/meta-mender-core
#
# Status: v0.4 prep scaffolding.  HIL roundtrip (mock-update +
# rollback simulation) parked behind docs/ci/HW-IN-LOOP.md.

# Mender's core image-generation class.  Adds the .mender artefact
# next to the standard image outputs and configures the rootfs for
# A/B updates.
INHERIT += "mender-full"

# Storage layout.  meta-mender-core supports several patterns; the
# v0.4 reference path uses block-device-backed A/B rootfs
# partitions.  Override in machine .conf or local.conf when the SoM
# uses raw NAND, eMMC RPMB, or partition-table-free SD media.
MENDER_STORAGE_DEVICE_BASE ?= "/dev/mmcblk0p"
MENDER_BOOT_PART ?= "${MENDER_STORAGE_DEVICE_BASE}1"
MENDER_ROOTFS_PART_A ?= "${MENDER_STORAGE_DEVICE_BASE}2"
MENDER_ROOTFS_PART_B ?= "${MENDER_STORAGE_DEVICE_BASE}3"
MENDER_DATA_PART ?= "${MENDER_STORAGE_DEVICE_BASE}4"

# Rootfs sizing.  256 MiB per A/B slot covers the v0.4 reference
# image (alp-sdk-runtime + alp-chips + alp-edgeai + a TensorFlow
# Lite Micro runtime + a representative model).  Bump in
# local.conf for fatter app payloads.
MENDER_STORAGE_TOTAL_SIZE_MB ?= "1024"
MENDER_BOOT_PART_SIZE_MB ?= "32"
MENDER_DATA_PART_SIZE_MB ?= "256"

# Server URL placeholder.  Override in local.conf for production
# fleets.  The default points at Mender's hosted demo so
# image builds don't fail on missing required vars; production
# fleets always override.
MENDER_SERVER_URL ?= "https://hosted.mender.io"
MENDER_TENANT_TOKEN ?= ""

# Artefact naming.  Mender uses ARTIFACTNAME to detect upgrades vs
# downgrades; bake the SDK version into the default so OTA history
# tells you what shipped.
MENDER_ARTIFACT_NAME ?= "alp-${DISTRO_VERSION}-${MACHINE}-${PV}"

# U-Boot is the standard bootloader integration point for Mender
# on i.MX + Renesas.  Each machine .conf picks the matching
# `u-boot-mender` provider when this include is used.
PREFERRED_PROVIDER_virtual/bootloader ?= "u-boot"

# Image features.  Mender requires the rootfs include the mender
# client + ssh (for first-boot enrollment); apps can opt out of
# ssh once their bootstrap flow is in place.
IMAGE_FEATURES:append = " ssh-server-openssh"
IMAGE_INSTALL:append = " mender-client mender-connect"
