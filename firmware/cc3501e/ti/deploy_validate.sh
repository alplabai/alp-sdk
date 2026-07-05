#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Linux port of validate_gpio_bench.ps1 step [2/5] -- WARM-flash the CC3501E:
# FIB build -> sign with the Alp VALIDATION key -> program over the XDS110.
# (The absent deploy_validate.ps1 named in package_cc3501e_prod.ps1 is this, on Linux.)
#
# WARM path: no cold POR forced here; the Alif host app drives the reset
# (WIFI_EN + nRESET) when it brings the link up, skipping the fuse-gated vendor SBL.
#
# Requires the bench signing assets (NOT in the repo -- stage from the bench dir
# that ran the 2026-06-18 flash; they are a matched set):
#   PUBLIC_KEY      -- Alp VALIDATION public key (PEM); the fresh unit's vendor RoT
#   SIGNING_MODULE  -- sign.py shim with PRIVATE_KEY/PUBLIC_KEY set to the validation keypair
#   CONF_BIN        -- cc35xx-conf.bin (SoM flash/memory config; must match this SoM)
#   TOOL_SETTINGS   -- tool_settings.json (device/key-specific programmer manifest)
#
# Usage:
#   PUBLIC_KEY=... SIGNING_MODULE=... CONF_BIN=... TOOL_SETTINGS=... ./deploy_validate.sh
set -euo pipefail

TOOLBOX="${TOOLBOX:-/home/caner/ti/simplelink_wifi_toolbox_4_2_4/simplelink_wifi_toolbox_lin_4_2_4/simplelink-wifi-toolbox}"
PUBLIC_KEY="${PUBLIC_KEY:?stage + set: Alp validation public key (PEM)}"
SIGNING_MODULE="${SIGNING_MODULE:?stage + set: sign.py shim (keys wired to the validation keypair)}"
CONF_BIN="${CONF_BIN:?stage + set: cc35xx-conf.bin}"
TOOL_SETTINGS="${TOOL_SETTINGS:?stage + set: tool_settings.json}"
XDS_SERIAL="${XDS_SERIAL:-L50015YR}"     # CC3501E XDS110 on this bench

# GPE image/flash version = the CC35 vendor-RoT ANTI-ROLLBACK gate: the unit
# rejects a flash whose version <= what is already programmed.  It is NOT the
# app SemVer (that lives in firmware-version.txt and is reported via
# GET_DIAG_INFO.fw_version) and NOT the wire ALP_CC3501E_PROTOCOL_VERSION.
#
# Scheme = date-derived + major>=1 so it is always monotonic and beats whatever
# is on the unit: major.<yy>.<mmdd>.<hhmm>  e.g. 1.26.0705.1432.  The bench unit
# was poisoned to 0.9.0.7 by ad-hoc bumps, so major MUST stay >= 1 going forward
# (1.x > 0.9).  Override with VERSION=... only to force a specific higher value.
VERSION="${VERSION:-1.$(date +%y).$(date +%m%d).$(date +%H%M)}"

OUT=/home/caner/alp-sdk/firmware/cc3501e/build/ti
VOUT="$OUT/cc3501e-bridge.out"
PKG="$OUT/bench"
[ -f "$VOUT" ] || { echo "missing $VOUT -- run: firmware/cc3501e/ti/build_ti.sh --wifi"; exit 1; }
for f in "$PUBLIC_KEY" "$SIGNING_MODULE" "$CONF_BIN" "$TOOL_SETTINGS"; do
  [ -f "$f" ] || { echo "MISSING signing asset: $f"; exit 3; }
done
mkdir -p "$PKG"

echo "== FIB build vendor_image v$VERSION =="
"$TOOLBOX" flash-images-builder build vendor_image --version "$VERSION" \
    --public_key "$PUBLIC_KEY" --vendor_out_file "$VOUT" --conf_bin_file "$CONF_BIN" --dir_out_path "$PKG"

echo "== sign vendor_image (Alp VALIDATION key -- bench/staging) =="
"$TOOLBOX" flash-images-builder sign vendor_image \
    --unsign_image "$PKG/vendor_image.unsign.bin" --activation_type vendor_key \
    --signing_module "$SIGNING_MODULE" --public_key "$PUBLIC_KEY" --dir_out_path "$PKG"
# sign gotcha (BRINGUP_STATUS): output is named after the input base -> rename to
# the name tool_settings.json references before programming.
cp -f "$PKG/vendor_image.sign.bin" "$PKG/primary_vendor_image.sign.bin"

echo "== program over XDS110 ($XDS_SERIAL) =="
progargs=(programmer -i XDS110)
[ -n "$XDS_SERIAL" ] && progargs+=(-param1 "$XDS_SERIAL")
progargs+=(programming --tool_settings "$TOOL_SETTINGS")
"$TOOLBOX" "${progargs[@]}" || {
  echo "program returned nonzero -- retry once (intermittent -1141 SECAP reject, per BRINGUP_STATUS)"
  "$TOOLBOX" "${progargs[@]}"
}
echo "== CC3501E warm-flashed. =="
