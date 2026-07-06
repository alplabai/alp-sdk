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
if [ "${1:-}" = "--expect-hw" ]; then
	EXPECT_HW=1
	shift
fi

if [ "$#" -ne 0 ]; then
	echo "usage: $0 [--expect-hw]" >&2
	exit 2
fi

JLINK="$(bench_jlink_exe)" || exit $?

cat > /tmp/firmware-update-log-dual-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 0x02000060, 0x10
Sleep 400
mem32 0x0200006C, 0x4
mem32 0x02001060, 0x10
exit
EOF

$JLINK -nogui 1 -CommanderScript /tmp/firmware-update-log-dual-read.jlink \
	2>/tmp/firmware-update-log-dual-read.err \
	>/tmp/firmware-update-log-dual-read.out || true

hp_line=$(awk 'toupper($1) == "02000060" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)
he_line=$(awk 'toupper($1) == "02001060" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)

echo "----- update-log SRAM0 proof beacons -----"
echo "HP @0x02000060: [magic, last_status, last_op, served_count]"
echo "HE @0x02001060: [magic, last_op, last_seq, last_status]"
[ -n "$hp_line" ] && echo "$hp_line"
[ -n "$he_line" ] && echo "$he_line"
echo "------------------------------------------"

if [ -z "$hp_line" ] || [ -z "$he_line" ]; then
	echo "missing one or more proof beacon lines; see /tmp/firmware-update-log-dual-read.out" >&2
	exit 1
fi

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
