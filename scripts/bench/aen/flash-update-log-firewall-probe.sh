#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-firewall-probe.sh [--package-only] <he-probe-build-dir>
#
# Build and optionally flash the HE direct-write MRAM firewall probe for
# examples/connectivity/firmware-update-log. The default package is app-only so
# it preserves the board's existing DEVICE/firewall policy. Set
# ALP_AEN_INCLUDE_DEVICE_CONFIG=yes only when intentionally replacing that
# policy; set ALP_AEN_DEVICE_CONFIG_JSON to a config filename under the SETOOLS
# build/config directory when using a board-specific policy. The probe is
# destructive when the firewall is absent: the helper records the first 16 bytes
# of alp_ulog_partition, lets HE try to overwrite them, then reports failure if
# the SWD post-read differs from the baseline.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

PACKAGE_ONLY=0
if [ "${1:-}" = "--package-only" ]; then
	PACKAGE_ONLY=1
	shift
fi

if [ "$#" -ne 1 ]; then
	echo "usage: $0 [--package-only] <he-probe-build-dir>" >&2
	exit 2
fi

HE_BD="$(cd "$1" && pwd)"
HE_BIN="$HE_BD/zephyr/zephyr.bin"
HE_DTS="$HE_BD/zephyr/zephyr.dts"

[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }
grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE=y' "$HE_BD/zephyr/.config" || {
	echo "HE build is not the firewall-probe profile" >&2
	exit 2
}

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
JLINK="$(bench_jlink_exe)" || exit $?
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")

ULOG_OFFSET=$(awk '
	/alp_ulog_partition:/ { in_node = 1 }
	in_node && /reg = </ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^0x[0-9a-fA-F]+$/) {
				print $i
				exit
			}
		}
	}
	in_node && /};/ { in_node = 0 }
' "$HE_DTS")
[ -z "$ULOG_OFFSET" ] && { echo "could not parse alp_ulog_partition offset from $HE_DTS" >&2; exit 2; }
ULOG_ADDR=$(printf '0x%08X' $(($ULOG_OFFSET + 0x80000000)))

read_ulog_words() {
	local out="$1"
	cat > /tmp/firmware-update-log-probe-read-mram.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 $ULOG_ADDR, 0x4
exit
EOF
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-probe-read-mram.jlink \
		2>/tmp/firmware-update-log-probe-read-mram.err \
		>"/tmp/firmware-update-log-probe-read-mram-$out.out" || true
	awk -v addr="${ULOG_ADDR#0x}" 'toupper($1) == toupper(addr) && $2 == "=" {
		print toupper($3" "$4" "$5" "$6)
		exit
	}' "/tmp/firmware-update-log-probe-read-mram-$out.out"
}

rv=$(xxd -e -l 8 "$HE_BIN" | awk '{print $3}')
echo ">>> HE firewall-probe reset vector: 0x$rv" >&2
case "$rv" in
	000*) : ;;
	800*) echo "!! HE probe is MRAM-linked, not ITCM-loadable for the ATOC" >&2; exit 3 ;;
	*) echo "!! HE probe reset vector is unexpected for an ITCM-loaded AEN image" >&2; exit 3 ;;
esac

HE_IMG=firmware-update-log-he-firewall-probe.bin
cp -f "$HE_BIN" "$SET/build/images/$HE_IMG"
DEVICE_CONFIG_JSON="${ALP_AEN_DEVICE_CONFIG_JSON:-app-device-config.json}"

{
	echo "{"
	if [ "${ALP_AEN_INCLUDE_DEVICE_CONFIG:-no}" = "yes" ]; then
		printf '    "DEVICE":   { "disabled": false, "binary": "%s", "version": "0.5.00", "signed": true },\n' \
			"$DEVICE_CONFIG_JSON"
		echo ">>> including DEVICE config in firewall-probe ATOC: $DEVICE_CONFIG_JSON" >&2
	else
		echo ">>> app-only firewall-probe ATOC; preserving existing DEVICE/firewall policy" >&2
	fi
	cat <<JSON
    "HE-PROBE": { "disabled": false, "binary": "$HE_IMG", "version": "1.0.0", "signed": true,
                  "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON
} > "$SET/build/config/firmware-update-log-firewall-probe.json"

cd "$SET"
echo ">>> AEN firmware-update-log HE firewall-probe ATOC" >&2
./app-gen-toc -f build/config/firmware-update-log-firewall-probe.json >/tmp/firmware-update-log-firewall-probe-gentoc.log 2>&1 \
	|| { echo "gen-toc FAILED"; tail -20 /tmp/firmware-update-log-firewall-probe-gentoc.log; exit 1; }

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

BASELINE_WORDS=$(read_ulog_words before)
[ -z "$BASELINE_WORDS" ] && { echo "could not read pre-flash alp_ulog_partition words at $ULOG_ADDR" >&2; exit 2; }
echo "    alp_ulog baseline @ $ULOG_ADDR: $BASELINE_WORDS" >&2

cat > /tmp/firmware-update-log-firewall-probe-write.jlink <<EOF
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

"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-firewall-probe-write.jlink 2>&1 | tee /tmp/firmware-update-log-firewall-probe-write.out | \
	grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" | head -40

if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" \
	/tmp/firmware-update-log-firewall-probe-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

echo "flash complete; reading firewall-probe beacon" >&2
sleep 3
ALP_AEN_FIREWALL_PROBE_BASELINE="$BASELINE_WORDS" \
	"$HERE/read-update-log-proof.sh" --expect-firewall-probe
