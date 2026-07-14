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

TOOLBOX="${TOOLBOX:?stage + set: SimpleLink Wi-Fi Toolbox launcher dir (the simplelink-wifi-toolbox executable)}"
PUBLIC_KEY="${PUBLIC_KEY:?stage + set: Alp validation public key (PEM)}"
SIGNING_MODULE="${SIGNING_MODULE:?stage + set: sign.py shim (keys wired to the validation keypair)}"
CONF_BIN="${CONF_BIN:?stage + set: cc35xx-conf.bin}"
TOOL_SETTINGS="${TOOL_SETTINGS:?stage + set: tool_settings.json}"
XDS_SERIAL="${XDS_SERIAL:-L50015YR}"     # CC3501E XDS110 on this bench

# GPE image/flash version = the CC35 vendor-RoT gate.  It is NOT the app SemVer
# (that lives in firmware-version.txt and is reported via GET_DIAG_INFO.fw_version)
# and NOT the wire ALP_CC3501E_PROTOCOL_VERSION.
#
# The four fields a.b.c.d are BYTE-SIZED (each 0..255).  TWO hard constraints,
# both bench-proven on the E1M-AEN801 (2026-07-05):
#   1. Each field must be <=255.  A human date like 1.<yy>.<mmdd>.<hhmm> is INVALID
#      (mmdd=0705/hhmm=1531 overflow a byte and corrupt the version).
#   2. The MAJOR field (a) MUST be 0.  A vendor image whose major >= 1 FAILS the
#      SES/BL2 secure-boot AUTHENTICATION (boot report @0x28000104 sets AUTH_ERROR
#      0x80) and the app core never launches -- host reads get_version=-5, the CC35
#      never services the bridge.  Proven: byte-identical firmware authenticated at
#      0.0.1.0 but AUTH_ERROR'd at 1.0.0.0 and at 104.x.y.z.  (The OLD scheme here --
#      the big-endian 4 bytes of `date +%s`, high byte ~0x68=104 in the major slot --
#      is exactly this failure and silently bricked EVERY V3 image this session.)
#
# Correct scheme: MAJOR=0, then the low 3 bytes of the Unix epoch in b.c.d.  Each
# field is a byte by construction; monotonic per-second (the 24-bit window wraps
# only every ~194 days -- fine within a release cycle).  Override with VERSION=a.b.c.d
# (all <=255, a=0, higher than the unit) to force one.
_e=$(date +%s)
VERSION="${VERSION:-0.$(( (_e >> 16) & 255 )).$(( (_e >> 8) & 255 )).$(( _e & 255 ))}"

# Output root is derived from this script's location (mirrors build_ti.sh's
# out="$fw/build/ti"), so the script targets THIS checkout, not a fixed path.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$(cd "$HERE/.." && pwd)" # firmware/cc3501e
OUT="$fw/build/ti"
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
prog_log="$PKG/program.log"
"$TOOLBOX" "${progargs[@]}" >"$prog_log" 2>&1 || {
  echo "program returned nonzero -- retry once (intermittent -1141 SECAP reject, per BRINGUP_STATUS)"
  "$TOOLBOX" "${progargs[@]}" >"$prog_log" 2>&1
}
cat "$prog_log"

# The programmer returns 0 even when it SKIPS the vendor image: if the staged
# tool_settings' programming_instructions/action_request are stale or image-coupled
# to a different build, the vendor-image write is silently no-op'd -- it streams only
# the ~1.3 KB programming-instructions and leaves the OLD resident image (this is #712,
# which cost a full bench session on #708).  A nonzero exit is NOT sufficient.
#
# Verify the FULL vendor image actually streamed.  NB: programming_report.txt's
# primary_vendor_image_done bit is NOT a usable signal on this bench -- it reads 0 on
# EVERY report, including the known-good REF_SET that brought Wi-Fi/BLE up.  The reliable
# discriminator is the streamed byte count vs the vendor image size: full stream (~1.09 MB)
# = written, ~1.3 KB = skipped.
vsize=$(stat -c%s "$PKG/primary_vendor_image.sign.bin" 2>/dev/null || echo 0)
streamed=$(grep -oiE 'Writing binary size of[[:space:]]+[0-9]+' "$prog_log" | grep -oE '[0-9]+' | sort -rn | head -1)
streamed=${streamed:-0}
if [ "$vsize" -gt 0 ] && [ "$streamed" -ge $(( vsize / 2 )) ]; then
  echo "== CC3501E warm-flashed + vendor image streamed ($streamed B of $vsize B). =="
else
  echo "ERROR: programmer exited 0 but did NOT stream the vendor image (streamed=$streamed B, expected ~$vsize B)." >&2
  echo "       -> stale / image-coupled programming manifest: only the instructions were written and the" >&2
  echo "       OLD resident image survives (#712).  deploy_validate.sh re-signs only the vendor_image half;" >&2
  echo "       programming_instructions + action_request are image-coupled and are NOT regenerated here." >&2
  echo "       To deliver a fresh image, use the matched-full-set pipeline instead:" >&2
  echo "         firmware/cc3501e/ti/regen_flashset.sh" >&2
  exit 5
fi
