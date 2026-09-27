#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Alp Lab AB
#
# Set the hostname from the SoM SKU that U-Boot read out of the identity
# EEPROM and published as /chosen/alp,sku (u-boot patch 0009), so one image
# per family names each board after the module actually fitted. Without the
# property (no validated manifest, older bootloader) the distro default from
# alp.conf stays. Always exits 0: a missing SKU is not a boot failure.
#
# When U-Boot also publishes the unit serial (/chosen/alp,serial, patch
# 0010), it is appended so units of one SKU get distinct names:
# "e1m-v2m103-2026w38-0001". The whole serial, not just its index: the
# index restarts every ISO week.
#
# ALP_SKU_PROP / ALP_SERIAL_PROP / ALP_HOSTNAME_FILE / ALP_KERNEL_HOSTNAME
# override the paths for tests/scripts/test_alp_hostname_set.py only.

set -u

prop=${ALP_SKU_PROP:-/proc/device-tree/chosen/alp,sku}
sprop=${ALP_SERIAL_PROP:-/proc/device-tree/chosen/alp,serial}
etc=${ALP_HOSTNAME_FILE:-/etc/hostname}
kern=${ALP_KERNEL_HOSTNAME:-/proc/sys/kernel/hostname}
[ -r "$prop" ] || exit 0

# "E1M-V2M103" -> "e1m-v2m103": lowercase, [a-z0-9-] only, no stray dashes.
# This is the whole trust boundary for a raw EEPROM field: tr -c maps every
# other byte to '-' before the value is used anywhere.
sanitise() {
	tr -d '\0' <"$1" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9-' '-' |
		sed -e 's/--*/-/g' -e 's/^-//' -e 's/-$//'
}

name=$(sanitise "$prop")
[ -n "$name" ] || exit 0
if [ -r "$sprop" ]; then
	serial=$(sanitise "$sprop")
	[ -n "$serial" ] && name="$name-$serial"
fi

# Write the kernel hostname directly (no dependency on a hostname binary)
# and log either outcome.
if echo "$name" >"$kern" 2>/dev/null; then
	echo "alp-hostname: $name (from /chosen)"
else
	echo "alp-hostname: WARNING: could not set kernel hostname to $name" >&2
fi

# Persist so tools reading /etc/hostname agree. Only when it changed (no
# flash write every boot) and via rename, so a power cut never leaves an
# empty file.
if [ "$(cat "$etc" 2>/dev/null)" != "$name" ]; then
	trap 'rm -f "$etc.alp-new"' EXIT
	if ! { echo "$name" >"$etc.alp-new" && mv -f "$etc.alp-new" "$etc"; } 2>/dev/null; then
		echo "alp-hostname: WARNING: could not persist $name to $etc (read-only rootfs?)" >&2
	fi
fi
exit 0
