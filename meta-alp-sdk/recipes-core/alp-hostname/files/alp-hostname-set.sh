#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# alp-hostname-set.sh -- derive the Linux hostname from the SoM SKU
# U-Boot published to the device tree.
#
# WHY: meta-alp-sdk/conf/distro/alp.conf pins one static `hostname ?=
# "alp-e1m"` for every SKU baked into a given image, so a board running
# an image built for a different SKU boots with the WRONG hostname
# (bench-observed: a V2M103 board running an old v2n101 image reported
# itself as e1m-v2n101-a55). U-Boot's board_late_init() already
# identifies the SoM from the on-module identity EEPROM (see the 0001
# rzv2n-dev patch) and, once it does, writes the SKU string it printed
# ("ALP: SoM family ... sku ...") into /chosen as `alp,sku` (and
# `alp,serial` when the manifest carries a serial number) so Linux can
# read the SAME fact instead of re-deriving or hardcoding it -- one
# machine source for "which SoM is this", per this repo's simplification
# principle.
#
# `alp,sku` is OPTIONAL: older firmware, a non-ALP board, or a manifest
# that failed validation simply never sets the property, and this script
# leaves the image's distro-default hostname (alp.conf's "alp-e1m")
# completely untouched -- no error, no log noise on the common case of
# firmware predating this feature.
#
# Exit code is always 0: a missing/malformed property is an expected,
# non-fatal case, not a boot failure worth failing this unit over.

set -u

CHOSEN_SKU=/proc/device-tree/chosen/alp,sku
CHOSEN_SERIAL=/proc/device-tree/chosen/alp,serial

[ -r "$CHOSEN_SKU" ] || exit 0

# DT string properties are NUL-terminated; strip the trailing NUL byte
# `tr -d` leaves behind, then sanitise to a valid hostname label
# ([a-z0-9-]) -- SKU strings are shown to humans with mixed case/spaces
# by U-Boot's printf but a hostname label may not contain either.
sku=$(tr -d '\0' <"$CHOSEN_SKU" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9-' '-')
# Collapse repeated '-' left by the sanitise pass and trim leading/
# trailing ones, so "e1m--v2m103" (a space in the source SKU) reads
# "e1m-v2m103", not with a doubled or dangling dash.
sku=$(echo "$sku" | sed -e 's/-\{2,\}/-/g' -e 's/^-//' -e 's/-$//')

[ -n "$sku" ] || exit 0

new_hostname="$sku"

if [ -r "$CHOSEN_SERIAL" ]; then
    serial=$(tr -d '\0' <"$CHOSEN_SERIAL" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9' '-')
    # Last 4 characters identify the unit without the full serial
    # dominating a hostname meant to stay short and typeable.
    serial_suffix=$(echo "$serial" | tail -c 5)
    [ -n "$serial_suffix" ] && new_hostname="${sku}-${serial_suffix}"
fi

echo "alp-hostname: setting hostname to \"$new_hostname\" (from /chosen/alp,sku)"

if command -v hostnamectl >/dev/null 2>&1; then
    hostnamectl set-hostname "$new_hostname"
else
    echo "$new_hostname" >/etc/hostname
    hostname "$new_hostname"
fi

exit 0
