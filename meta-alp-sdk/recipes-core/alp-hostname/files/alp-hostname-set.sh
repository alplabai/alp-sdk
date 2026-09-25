#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# Set the hostname from the SoM SKU that U-Boot read out of the identity
# EEPROM and published as /chosen/alp,sku (u-boot patch 0009), so one image
# per family names each board after the module actually fitted. Without the
# property (no validated manifest, older bootloader) the distro default from
# alp.conf stays. Always exits 0: a missing SKU is not a boot failure.

set -u

prop=/proc/device-tree/chosen/alp,sku
[ -r "$prop" ] || exit 0

# "E1M-V2M103" -> "e1m-v2m103": lowercase, [a-z0-9-] only, no stray dashes.
name=$(tr -d '\0' <"$prop" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9-' '-' |
	sed -e 's/--*/-/g' -e 's/^-//' -e 's/-$//')
[ -n "$name" ] || exit 0

hostname "$name"
# Persist too, so tools that read /etc/hostname agree; best effort on a
# read-only rootfs.
echo "$name" >/etc/hostname 2>/dev/null || true
echo "alp-hostname: $name (from /chosen/alp,sku)"
exit 0
