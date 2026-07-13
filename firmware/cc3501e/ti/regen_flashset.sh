#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a COMPLETE, MATCHED CC3501E flash-set for the current firmware .out and
# stage a WARM (vendor-image-only, no-fuse) programmer manifest for it.
#
# WHY THIS EXISTS: deploy_validate.sh only rebuilds the vendor_image; it reuses a
# stale programmer manifest.  Two couplings make that insufficient (both cost a
# wasted bench flash to discover):
#   1. programming_instructions is built from --version + the flash-discovery
#      configs -- it must be regenerated at the SAME --version as the vendor_image
#      or the programmer's rollback/version gate rejects the write.
#   2. the programming action_request carries a `prog_req_content` BITMAP of which
#      components to program.  If `primary_vendor_image` is false the programmer
#      streams only the ~1.3 KB instructions and SKIPS the image -- the CC35 then
#      boots with no valid app (GET_VERSION = -5).  This script builds a WARM
#      action_request with primary_vendor_image=true, boot_sector=false (no fuse).
#
# Output: a self-consistent set + `tool_settings.warm.json` under DIR_OUT, ready
# for:  simplelink-wifi-toolbox programmer -i XDS110 -param1 <sn> programming \
#         --tool_settings <DIR_OUT>/tool_settings.warm.json
#
# The build/sign steps are HOST-side + non-destructive; only the programmer run
# touches the board.  Requires the bench signing assets (NOT in the repo).
#
# Usage:
#   TOOLBOX=... PUBLIC_KEY=... SIGNING_MODULE=... CONF_BIN=... REF_SET=... \
#     [VERSION=0.b.c.d] [ACTION_PARAMS=...] firmware/cc3501e/ti/regen_flashset.sh
set -euo pipefail

TOOLBOX="${TOOLBOX:?stage + set: simplelink-wifi-toolbox executable}"
PUBLIC_KEY="${PUBLIC_KEY:?stage + set: Alp validation public key (PEM)}"
SIGNING_MODULE="${SIGNING_MODULE:?stage + set: sign.py shim (validation keypair)}"
CONF_BIN="${CONF_BIN:?stage + set: cc35xx-conf.bin}"
# A prior COMPLETE matched set (e.g. gen-out-conn45/toolbox) -- the source of the
# version-independent inputs (fuses/flash-disc JSONs) + the reused debug_action_request.
REF_SET="${REF_SET:?stage + set: dir of a prior complete matched set (fuses + flash_disc JSONs)}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw="$(cd "$HERE/.." && pwd)"                         # firmware/cc3501e
VOUT="$fw/build/ti/cc3501e-bridge.out"               # the firmware built by build_ti.sh
DIR_OUT="${DIR_OUT:-$fw/build/ti/flashset}"
[ -f "$VOUT" ] || { echo "missing $VOUT -- run firmware/cc3501e/ti/build_ti.sh --wifi --ble"; exit 1; }

# GPE version: MAJOR=0, low 3 bytes of the epoch (monotonic; each byte <=255).
# Same scheme as deploy_validate.sh -- guaranteed higher than a prior same-day set.
_e=$(date +%s)
VERSION="${VERSION:-0.$(((_e>>16)&255)).$(((_e>>8)&255)).$((_e&255))}"

# WARM action_params: primary_vendor_image=true, everything else (incl. boot_sector)
# false.  Regenerate it from REF_SET's full-set params unless one is provided.
ACTION_PARAMS="${ACTION_PARAMS:-$DIR_OUT/action_params.warm.json}"

FUSES="$REF_SET/fuse_prog_inst_param.CC35XXE.mod.json"
FD_OTFDE="$REF_SET/temp/flash_disc_param_otfde.json"
FD_EXT="$REF_SET/temp/flash_disc_param_ext_mem.json"
FD_XSPI="$REF_SET/temp/flash_disc_param_xspi.json"
for f in "$FUSES" "$FD_OTFDE" "$FD_EXT" "$FD_XSPI" "$REF_SET/debug_action_request.sign.bin"; do
  [ -f "$f" ] || { echo "REF_SET missing $f"; exit 3; }
done

rm -rf "$DIR_OUT"; mkdir -p "$DIR_OUT"
echo "== regen flash-set v$VERSION from $VOUT =="

echo "-- 1/4 vendor_image --"
"$TOOLBOX" flash-images-builder build vendor_image --version "$VERSION" \
    --public_key "$PUBLIC_KEY" --vendor_out_file "$VOUT" --conf_bin_file "$CONF_BIN" --dir_out_path "$DIR_OUT"
"$TOOLBOX" flash-images-builder sign vendor_image \
    --unsign_image "$DIR_OUT/vendor_image.unsign.bin" --activation_type vendor_key \
    --signing_module "$SIGNING_MODULE" --public_key "$PUBLIC_KEY" --dir_out_path "$DIR_OUT"
cp -f "$DIR_OUT/vendor_image.sign.bin" "$DIR_OUT/primary_vendor_image.sign.bin"

echo "-- 2/4 programming_instructions (matched v$VERSION) --"
"$TOOLBOX" flash-images-builder build programming_image --version "$VERSION" \
    --public_key "$PUBLIC_KEY" --fuses_programming_instructions "$FUSES" \
    --flash_discovery_config_otfde "$FD_OTFDE" --flash_discovery_config_ext_mem "$FD_EXT" \
    --flash_discovery_config_xspi "$FD_XSPI" --dir_out_path "$DIR_OUT" \
    --activation_type vendor_key --signing_module "$SIGNING_MODULE"
"$TOOLBOX" flash-images-builder sign programming_image \
    --unsign_image "$DIR_OUT/programming_instructions_image.unsign.bin" --activation_type vendor_key \
    --signing_module "$SIGNING_MODULE" --public_key "$PUBLIC_KEY" --dir_out_path "$DIR_OUT"

echo "-- 3/4 WARM action_request (primary_vendor_image=true, boot_sector=false) --"
if [ ! -f "$ACTION_PARAMS" ]; then
  python3 - "$REF_SET" "$ACTION_PARAMS" <<'PY'
import json,sys,glob,os
ref,dst=sys.argv[1],sys.argv[2]
# find a full-set programming action_params in the signing tree to copy the shape
cands=glob.glob(os.path.join(ref,"..","..","gen-syscfg","action_params.json"))
cands+=glob.glob(os.path.join(ref,"..","..","**","action_params.json"),recursive=True)
srcp=next((c for c in cands if os.path.exists(c)),None)
d=json.load(open(srcp))
prc=d["programming"]["payload_param"]["payload"]["prog_req_content"]
for k in prc:
    if isinstance(prc[k],bool): prc[k]=False
prc["primary_vendor_image"]=True     # WARM: write ONLY the vendor image, no fuse
os.makedirs(os.path.dirname(dst),exist_ok=True)
json.dump(d,open(dst,"w"),indent=2)
print("  authored",dst,"from",srcp)
PY
fi
"$TOOLBOX" flash-images-builder build action_request --type programming \
    --params_json "$ACTION_PARAMS" --dir_out_path "$DIR_OUT"
"$TOOLBOX" flash-images-builder sign action_request \
    --unsign_request "$DIR_OUT/programming_action_request.unsign.bin" --activation_type vendor_key \
    --signing_module "$SIGNING_MODULE" --public_key "$PUBLIC_KEY" --dir_out_path "$DIR_OUT"

echo "-- 4/4 WARM tool_settings --"
python3 - "$DIR_OUT" "$REF_SET" <<'PY'
import json,sys,os
out,ref=sys.argv[1],sys.argv[2]
c={
  "tbl_container_programming": None,
  "programming_instructions": os.path.join(out,"programming_instructions_image.sign.bin"),
  "actions_req_paths": {
    "programming": os.path.join(out,"programming_action_request.sign.bin"),
    "debug": os.path.join(ref,"debug_action_request.sign.bin"),
  },
  "primary_vendor_image": os.path.join(out,"primary_vendor_image.sign.bin"),
  "secondary_vendor_image": "",
  "boot_sector": None, "primary_tbl": None, "secondary_tbl": "",
  "primary_ti_wsoc": None, "secondary_ti_wsoc": "", "protected_storage": "",
}
json.dump({"programming_debug_and_ota_signed_components":c},
          open(os.path.join(out,"tool_settings.warm.json"),"w"),indent=2)
print("  wrote",os.path.join(out,"tool_settings.warm.json"))
PY

echo "== DONE. Matched WARM flash-set at $DIR_OUT (v$VERSION) =="
ls -la "$DIR_OUT"/*.sign.bin "$DIR_OUT"/tool_settings.warm.json
