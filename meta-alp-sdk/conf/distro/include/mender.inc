# SPDX-License-Identifier: Apache-2.0
#
# Mender OTA distro config for meta-alp-sdk targets.
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
# Status: v0.6 scaffolding.  HIL roundtrip (mock-update +
# rollback simulation) parked behind docs/ci/HW-IN-LOOP.md.

# Mender's core image-generation class.  Adds the .mender artefact
# next to the standard image outputs and configures the rootfs for
# A/B updates.
INHERIT += "mender-full"

# Storage layout.  meta-mender-core supports several patterns; the
# v0.6 reference path uses block-device-backed A/B rootfs
# partitions.  Override in machine .conf or local.conf when the SoM
# uses raw NAND, eMMC RPMB, or partition-table-free SD media.
MENDER_STORAGE_DEVICE_BASE ?= "/dev/mmcblk0p"
MENDER_BOOT_PART ?= "${MENDER_STORAGE_DEVICE_BASE}1"
MENDER_ROOTFS_PART_A ?= "${MENDER_STORAGE_DEVICE_BASE}2"
MENDER_ROOTFS_PART_B ?= "${MENDER_STORAGE_DEVICE_BASE}3"
MENDER_DATA_PART ?= "${MENDER_STORAGE_DEVICE_BASE}4"

# Rootfs sizing.  alp-image-edge (ROS 2 + alp-sdk + libcamera +
# GStreamer + DEEPX runtime where applicable) targets ~1 GiB
# slot ceiling on V2N101 / V2M101.  Bump in local.conf for fatter
# app payloads.
MENDER_STORAGE_TOTAL_SIZE_MB ?= "4096"
MENDER_BOOT_PART_SIZE_MB ?= "64"
MENDER_DATA_PART_SIZE_MB ?= "512"

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
# on Renesas.  Each machine .conf picks the matching
# `u-boot-mender` provider when this include is used.
PREFERRED_PROVIDER_virtual/bootloader ?= "u-boot"

# Image features.  Mender requires the rootfs include the mender
# client + ssh (for first-boot enrollment); apps can opt out of
# ssh once their bootstrap flow is in place.
IMAGE_FEATURES:append = " ssh-server-openssh"
IMAGE_INSTALL:append = " mender-client mender-connect"

# U-Boot keeps its environment as a redundant pair in eMMC boot partition 2
# (the SoM preset's on_module.emmc_boot.uboot_env; uboot-env-emmc.cfg has
# CONFIG_SYS_MMC_ENV_DEV=0, CONFIG_SYS_MMC_ENV_PART=2;
# tests/scripts/test_uboot_env_layout.py holds all of them equal to the
# preset).  meta-mender builds U-Boot's CONFIG_ENV_* from these variables and
# writes /etc/fw_env.config from the same ones, so setting them is what makes
# fw_setenv (the Mender client's upgrade_available / mender_boot_part writes)
# and U-Boot use ONE store:
#   MENDER_UBOOT_CONFIG_SYS_MMC_ENV_PART = "2"  -> device is
#       ${MENDER_STORAGE_DEVICE}boot1 = /dev/mmcblk0boot1 (the default "0"
#       is the user area /dev/mmcblk0, which U-Boot never reads),
#   MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_1/_2 -> the two copies,
#   BOOTENV_SIZE = 0x10000 = CONFIG_ENV_SIZE (meta-mender checks they match).
# mender-full generates /etc/fw_env.config itself; alp-uboot-env (installed by
# alp-image-common.inc) ships one too and would conflict, so it is removed.
MENDER_UBOOT_CONFIG_SYS_MMC_ENV_PART ?= "2"
MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_1 ?= "0x220000"
MENDER_UBOOT_ENV_STORAGE_DEVICE_OFFSET_2 ?= "0x230000"
BOOTENV_SIZE ?= "0x10000"
# The environment is outside the rootfs user area (boot partition 2), so no
# space is reserved in front of the first partition for it.
MENDER_RESERVED_SPACE_BOOTLOADER_DATA ?= "0"
IMAGE_INSTALL:remove = "alp-uboot-env"

# U-Boot is the bootloader (the default meta-mender feature set is GRUB/UEFI,
# which this board does not have): turns on meta-mender's U-Boot integration
# (its generic boot-code patches, the env_mender.h variables, and the
# mender-uboot override u-boot_%.bbappend keys mender-boot.cfg on).
MENDER_FEATURES_ENABLE:append = " mender-uboot"
MENDER_FEATURES_DISABLE:append = " mender-grub mender-image-uefi"

# U-Boot integration.  Mender's auto-configure would patch the U-Boot sources
# to replace CONFIG_BOOTCOMMAND with its own generic one, which cannot boot
# this board (dtb choice from the EEPROM manifest, microSD, the OpenCV/Codec
# blobs).  The ALP U-Boot patch 0018 instead reads Mender's variables
# (mender_boot_part / mender_boot_part_hex, upgrade_available, bootcount /
# bootlimit, altbootcmd) from meta-mender's env_mender.h, which Mender's
# generic boot-code patches still provide; u-boot_%.bbappend adds
# mender-boot.cfg (CONFIG_ALP_E1M_MENDER) when mender-uboot is on.
MENDER_UBOOT_AUTO_CONFIGURE ?= "0"
