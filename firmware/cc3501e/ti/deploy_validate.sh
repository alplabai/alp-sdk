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
# The four fields a.b.c.d are BYTE-SIZED (each 0..255) and compared field-by-field.
# So a human date like 1.<yy>.<mmdd>.<hhmm> is INVALID -- mmdd=0705 and hhmm=1531
# overflow a byte and corrupt the programmed version (this bricked a bench unit).
#
# Correct scheme: the big-endian 4 bytes of the Unix epoch seconds. Each field is
# a byte (<=255) by construction, and because it is the big-endian split of a
# monotonically-increasing 32-bit counter, the field-by-field version compare IS
# the epoch compare -> strictly monotonic every second, no state file needed. The
# high byte is ~0x68 today, so this also cleanly beats any poisoned low value.
# Override with VERSION=a.b.c.d (all <=255, higher than the unit) to force one.
_e=$(date +%s)
VERSION="${VERSION:-$(( (_e >> 24) & 255 )).$(( (_e >> 16) & 255 )).$(( (_e >> 8) & 255 )).$(( _e & 255 ))}"

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
