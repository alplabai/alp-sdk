#!/usr/bin/env bash
# scripts/bench/aen/read-update-log-proof.sh
#
# Non-destructive readback for the AEN firmware-update-log proof beacons.
# Hold the labgrid reservation for the target board before running this script.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

EXPECT_HW=0
EXPECT_FIREWALL_PROBE=0
while [ "$#" -gt 0 ]; do
	case "$1" in
		--expect-hw)
			EXPECT_HW=1
			shift
			;;
		--expect-firewall-probe)
			EXPECT_FIREWALL_PROBE=1
			shift
			;;
		*)
			break
			;;
	esac
done

if [ "$#" -ne 0 ]; then
	echo "usage: $0 [--expect-hw] [--expect-firewall-probe]" >&2
	exit 2
fi

JLINK="$(bench_jlink_exe)" || exit $?
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")

cat > /tmp/firmware-update-log-dual-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 0x02000060, 0x10
Sleep 400
mem32 0x0200006C, 0x4
mem32 0x02001060, 0x10
mem32 0x02001080, 0x10
exit
EOF

"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-read.jlink \
	2>/tmp/firmware-update-log-dual-read.err \
	>/tmp/firmware-update-log-dual-read.out || true

hp_line=$(awk 'toupper($1) == "02000060" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)
he_line=$(awk 'toupper($1) == "02001060" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)
fw_line=$(awk 'toupper($1) == "02001080" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)

echo "----- update-log SRAM0 proof beacons -----"
echo "HP @0x02000060: [magic, last_status, last_op, served_count]"
echo "HE @0x02001060: [magic, last_op, last_seq, last_status]"
echo "FW @0x02001080: [magic, result, stage, detail, pc, cfsr, bfar, hfsr, offset]"
[ -n "$hp_line" ] && echo "$hp_line"
[ -n "$he_line" ] && echo "$he_line"
[ -n "$fw_line" ] && echo "$fw_line"
echo "------------------------------------------"

if [ "$EXPECT_FIREWALL_PROBE" -eq 0 ] && { [ -z "$hp_line" ] || [ -z "$he_line" ]; }; then
	echo "missing one or more proof beacon lines; see /tmp/firmware-update-log-dual-read.out" >&2
	exit 1
fi

if [ "$EXPECT_FIREWALL_PROBE" -eq 0 ]; then
	hp_magic=$(printf '%s\n' "$hp_line" | awk '{ print toupper($3) }')
	he_magic=$(printf '%s\n' "$he_line" | awk '{ print toupper($3) }')
	he_status=$(printf '%s\n' "$he_line" | awk '{ print toupper($6) }')

	if [ "$hp_magic" != "554C4F90" ] || [ "$he_magic" != "554C4FE0" ]; then
		echo "proof beacon magic mismatch; HP=$hp_magic HE=$he_magic" >&2
		exit 1
	fi

	if [ "$EXPECT_HW" -eq 1 ] && [ "$he_status" != "00000000" ]; then
		echo "HE client did not finish with ALP_OK; status=0x$he_status" >&2
		exit 1
	fi
fi

if [ "$EXPECT_FIREWALL_PROBE" -eq 1 ]; then
	if [ -z "$fw_line" ]; then
		echo "missing firewall probe beacon; see /tmp/firmware-update-log-dual-read.out" >&2
		exit 1
	fi
	fw_magic=$(printf '%s\n' "$fw_line" | awk '{ print toupper($3) }')
	fw_result=$(printf '%s\n' "$fw_line" | awk '{ print toupper($4) }')
	if [ "$fw_magic" != "46575052" ]; then
		echo "firewall probe magic mismatch; FW=$fw_magic" >&2
		exit 1
	fi
	case "$fw_result" in
		00000002|00000003)
			;;
		*)
			echo "firewall probe did not prove HE write rejection; result=0x$fw_result" >&2
			exit 1
			;;
	esac
fi
