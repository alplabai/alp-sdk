#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-firewall-probe.sh [--package-only] <he-probe-build-dir>
#
# Build and optionally flash the HE direct-write MRAM firewall probe for
# examples/connectivity/firmware-update-log. The probe is destructive when the
# firewall is absent: it intentionally tries to change the first 16 bytes of
# alp_ulog_partition from HE, then reports failure if the bytes changed.
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

[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }
grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE=y' "$HE_BD/zephyr/.config" || {
	echo "HE build is not the firewall-probe profile" >&2
	exit 2
}

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
JLINK="$(bench_jlink_exe)" || exit $?

rv=$(xxd -e -l 8 "$HE_BIN" | awk '{print $3}')
echo ">>> HE firewall-probe reset vector: 0x$rv" >&2
case "$rv" in
	000*) : ;;
	800*) echo "!! HE probe is MRAM-linked, not ITCM-loadable for the ATOC" >&2; exit 3 ;;
	*) echo "!! HE probe reset vector is unexpected for an ITCM-loaded AEN image" >&2; exit 3 ;;
esac

HE_IMG=firmware-update-log-he-firewall-probe.bin
cp -f "$HE_BIN" "$SET/build/images/$HE_IMG"

cat > "$SET/build/config/firmware-update-log-firewall-probe.json" <<JSON
{
    "DEVICE":   { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "HE-PROBE": { "disabled": false, "binary": "$HE_IMG", "version": "1.0.0", "signed": true,
                  "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON

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

$JLINK -nogui 1 -CommanderScript /tmp/firmware-update-log-firewall-probe-write.jlink 2>&1 | tee /tmp/firmware-update-log-firewall-probe-write.out | \
	grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" | head -40

if grep -qi "Could not connect to the target device" /tmp/firmware-update-log-firewall-probe-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

echo "flash complete; reading firewall-probe beacon" >&2
sleep 3
"$HERE/read-update-log-proof.sh" --expect-firewall-probe
