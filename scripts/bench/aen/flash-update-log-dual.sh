#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-dual.sh [--package-only] <hp-build-dir> <he-build-dir>
#
# Build the dual-entry ATOC package for examples/connectivity/firmware-update-log
# on E1M-AEN801 / Alif E8:
#   - HP owner:  M55_HP, loadAddress 0x50000000, flags ["load", "boot"]
#   - HE client: M55_HE, loadAddress 0x58000000, flags ["load"]
#
# The default package is app-only so it preserves the board's existing
# DEVICE/firewall policy. Set ALP_AEN_INCLUDE_DEVICE_CONFIG=yes only when
# intentionally replacing that policy. The package is written to MRAM only when
# ALP_CONFIRM_DESTRUCTIVE_FLASH=yes is present. Use --package-only to validate
# the SETOOLS package without touching the board.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

PACKAGE_ONLY=0
if [ "${1:-}" = "--package-only" ]; then
	PACKAGE_ONLY=1
	shift
fi

if [ "$#" -ne 2 ]; then
	echo "usage: $0 [--package-only] <hp-build-dir> <he-build-dir>" >&2
	exit 2
fi

HP_BD="$(cd "$1" && pwd)"
HE_BD="$(cd "$2" && pwd)"
HP_BIN="$HP_BD/zephyr/zephyr.bin"
HE_BIN="$HE_BD/zephyr/zephyr.bin"

[ -f "$HP_BIN" ] || { echo "missing HP zephyr.bin: $HP_BIN" >&2; exit 2; }
[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
JLINK="$(bench_jlink_exe)" || exit $?
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")

check_itcm_vector() {
	local role="$1"
	local bin="$2"
	local rv

	rv=$(xxd -e -l 8 "$bin" | awk '{print $3}')
	echo ">>> $role reset vector: 0x$rv" >&2
	case "$rv" in
		000*) : ;;
		800*) echo "!! $role is MRAM-linked, not ITCM-loadable for the dual ATOC" >&2; exit 3 ;;
		*) echo "!! $role reset vector is unexpected for an ITCM-loaded AEN image" >&2; exit 3 ;;
	esac
}

check_itcm_vector "HP" "$HP_BIN"
check_itcm_vector "HE" "$HE_BIN"

HP_IMG=firmware-update-log-hp.bin
HE_IMG=firmware-update-log-he.bin
cp -f "$HP_BIN" "$SET/build/images/$HP_IMG"
cp -f "$HE_BIN" "$SET/build/images/$HE_IMG"

{
	echo "{"
	if [ "${ALP_AEN_INCLUDE_DEVICE_CONFIG:-no}" = "yes" ]; then
		echo '    "DEVICE":   { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },'
		echo ">>> including DEVICE config in update-log dual ATOC (ALP_AEN_INCLUDE_DEVICE_CONFIG=yes)" >&2
	else
		echo ">>> app-only update-log dual ATOC; preserving existing DEVICE/firewall policy" >&2
	fi
	cat <<JSON
    "HP-OWNER": { "disabled": false, "binary": "$HP_IMG", "version": "1.0.0", "signed": true,
                  "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] },
    "HE-CLIENT": { "disabled": false, "binary": "$HE_IMG", "version": "1.0.0", "signed": true,
                   "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load"] }
}
JSON
} > "$SET/build/config/firmware-update-log-dual.json"

cd "$SET"
echo ">>> AEN firmware-update-log dual-entry ATOC" >&2
./app-gen-toc -f build/config/firmware-update-log-dual.json >/tmp/firmware-update-log-dual-gentoc.log 2>&1 \
	|| { echo "gen-toc FAILED"; tail -20 /tmp/firmware-update-log-dual-gentoc.log; exit 1; }

PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address" >&2; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ATOC_ADDR" >&2

if [ "$PACKAGE_ONLY" -eq 1 ]; then
	echo "package-only: not flashing MRAM" >&2
	exit 0
fi

if [ "${ALP_CONFIRM_DESTRUCTIVE_FLASH:-}" != "yes" ]; then
	echo "refusing destructive MRAM flash: set ALP_CONFIRM_DESTRUCTIVE_FLASH=yes for this run" >&2
	exit 4
fi

cat > /tmp/firmware-update-log-dual-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
loadbin $PKG $ATOC_ADDR
verifybin $PKG $ATOC_ADDR
RSetType 2
r
g
exit
EOF

"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-write.jlink 2>&1 | tee /tmp/firmware-update-log-dual-write.out | \
	grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" | head -40

if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" \
	/tmp/firmware-update-log-dual-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

echo "flash complete; capture labgrid console for HP owner + HE client output" >&2
sleep 3
READBACK_ARGS=()
if grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROVEN=y' "$HE_BD/zephyr/.config" \
	2>/dev/null; then
	READBACK_ARGS=(--expect-hw)
fi
"$HERE/read-update-log-proof.sh" "${READBACK_ARGS[@]}"
