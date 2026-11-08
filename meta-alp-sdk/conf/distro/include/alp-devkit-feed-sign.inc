# SPDX-License-Identifier: Apache-2.0
#
# Dev-kit opkg package feed -- SIGNING side (alp-sdk #2841). Opt-in, for the
# release builder ONLY (the internal bitbake runner); customers never need it.
#
# Signs every Packages index that `bitbake package-index` writes, with the
# dedicated Alp feed key. The private half is NOT in any repo checkout: the
# runner imports it into a scratch GNUPGHOME and points GPG_PATH at it:
#
#     require conf/distro/include/alp-devkit-feed-sign.inc
#     GPG_PATH = "/path/to/scratch/gnupghome"
#
# Signatures are binary (Packages.sig), the name opkg looks for.
# These per-arch signatures are NOT what is published and nothing reads them:
# scripts/package_devkit_feed.sh merges the allowlisted stanzas into one index,
# signs that with its own --gnupghome/--key and verifies it against the committed
# public key. The release flow does not need this fragment: without it
# `bitbake package-index` writes unsigned per-arch indexes and the published
# feed is signed all the same. It remains for a builder that wants signed
# per-arch indexes in its own deploy dir.

INHERIT += "sign_package_feed"
# Fingerprint of keys/alp_devkit_feed_signing.pub.asc.
PACKAGE_FEED_GPG_NAME ?= "2B7807E20B75B471BF88FD7AC9AB032782D3143B"
# The feed key is passphrase-less by design (internal runner secret); the class
# insists on a passphrase file, so point it at a committed one-line empty file.
PACKAGE_FEED_GPG_PASSPHRASE_FILE ?= "${ALP_SDK_LAYERDIR}/conf/distro/include/alp-devkit-feed.passphrase"
PACKAGE_FEED_GPG_SIGNATURE_TYPE ?= "BIN"
