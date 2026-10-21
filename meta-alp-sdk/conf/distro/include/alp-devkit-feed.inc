# SPDX-License-Identifier: Apache-2.0
#
# Dev-kit opkg package feed -- CLIENT side (alp-sdk #2841). Opt-in.
#
# Pull in from local.conf / a machine conf of a dev-kit image build:
#
#     require conf/distro/include/alp-devkit-feed.inc
#
# Effect: the alp-image-edge image ships opkg with GPG + HTTPS support, the feed config
# /etc/opkg/alp-devkit-feed.conf (signature checking ON) and the Alp feed
# public key, so `opkg update && opkg upgrade alp-sdk` works against the
# signed feed published for this image's release line.
#
# NOT a field-update channel: field devices stay on image-based A/B OTA
# (Mender). Customers who do not require this file get none of the above.
#
# Signing is a separate, builder-only fragment: alp-devkit-feed-sign.inc.

ALP_DEVKIT_FEED = "1"

# The feed is ipk/opkg. The poky default is package_rpm, so a dev-kit image is
# built with the ipk packager (rootfs package manager = opkg, which is also what
# the feed config below drives).
PACKAGE_CLASSES = "package_ipk"

# The feed line is the SDK major.minor (0.17.0~rc2 -> 0.17); one signed feed per
# line and per MACHINE (alp-sdk is PACKAGE_ARCH = MACHINE_ARCH). Override
# ALP_DEVKIT_FEED_URL to host the feed elsewhere (e.g. a file:// mirror).
ALP_DEVKIT_FEED_LINE ?= "${@'.'.join(d.getVar('DISTRO_VERSION').split('~')[0].split('.')[:2])}"
ALP_DEVKIT_FEED_TAG  ?= "feed-v${ALP_DEVKIT_FEED_LINE}-${MACHINE}"
ALP_DEVKIT_FEED_URL  ?= "https://github.com/alplabai/alp-sdk/releases/download/${ALP_DEVKIT_FEED_TAG}"

# opkg verifies signatures through gpgme and must follow GitHub's redirect to
# the CDN for release assets; its default wget fetcher does neither reliably.
# sha256: ipks are bound only by the checksum in the signed index, so it must be
# SHA-256 (the scarthgap opkg PACKAGECONFIG name is unverified without a build).
PACKAGECONFIG:append:pn-opkg = " gpg curl ssl-curl sha256"
