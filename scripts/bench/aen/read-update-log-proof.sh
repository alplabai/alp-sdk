#!/usr/bin/env bash
# scripts/bench/aen/read-update-log-proof.sh
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe over the labgrid-held AEN bench, and SETOOLS only when
# decoding the firewall probe's MRAM proof window). Runs under WSL2 on
# Windows. See docs/aen-bench-bringup.md.
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
fw_fault_line=$(awk 'toupper($1) == "02001090" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)
fw_offset_line=$(awk 'toupper($1) == "020010A0" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)
fw_pattern_tail_line=$(awk 'toupper($1) == "020010B0" && $2 == "=" { print; exit }' /tmp/firmware-update-log-dual-read.out)

echo "----- update-log SRAM0 proof beacons -----"
echo "HP @0x02000060: [magic, last_status, last_op, served_count]"
echo "HE @0x02001060: [magic, last_op, last_seq, last_status]"
echo "FW @0x02001080: [magic, result, stage, detail, pc, cfsr, bfar, hfsr, offset, attempted pattern]"
[ -n "$hp_line" ] && echo "$hp_line"
[ -n "$he_line" ] && echo "$he_line"
[ -n "$fw_line" ] && echo "$fw_line"
[ -n "$fw_fault_line" ] && echo "$fw_fault_line"
[ -n "$fw_offset_line" ] && echo "$fw_offset_line"
[ -n "$fw_pattern_tail_line" ] && echo "$fw_pattern_tail_line"
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
	fw_stage=$(printf '%s\n' "$fw_line" | awk '{ print toupper($5) }')
	fw_detail=$(printf '%s\n' "$fw_line" | awk '{ print toupper($6) }')
	fw_offset=$(printf '%s\n' "$fw_offset_line" | awk '{ print toupper($3) }')
	[ -z "$fw_offset" ] && fw_offset="????????"
	fw_pattern=$(printf '%s\n%s\n' "$fw_offset_line" "$fw_pattern_tail_line" | awk '
		NR == 1 { printf "%s %s %s", toupper($4), toupper($5), toupper($6) }
		NR == 2 { printf " %s", toupper($3) }
	')
	mram_line=""
	mram_words=""
	if printf '%s\n' "$fw_offset" | grep -Eq '^[0-9A-F]{8}$' && [ "$fw_offset" != "FFFFFFFF" ]; then
		# Read the log window the AUTHORITATIVE way: via the Secure Enclave
		# (getmramdata, bus master id 0), which is not subject to HE's master-side
		# firewall (FC8). A proven HW-enforced policy denies HE's own master id the
		# same window, and that denial also blocks the HE debug AP -- so a J-Link/SWD
		# read of the window is blocked or returns garbage and CANNOT be trusted for
		# the verdict (it produced a false FAIL before this path existed). SWD is only
		# a fallback for when SETOOLS/SE-UART are not wired.
		off_hex=$(printf '%s' "$fw_offset" | sed 's/^0*//')
		[ -z "$off_hex" ] && off_hex=0
		if [ -n "${SETOOLS_DIR:-}" ] && [ -n "${SE_UART:-}" ] && [ -x "$SETOOLS_DIR/maintenance" ]; then
			( cd "$SETOOLS_DIR" && ./maintenance -b "${SE_UART_BAUD:-57600}" -c "$SE_UART" \
				-opt getmramdata "0x$off_hex" ) \
				>/tmp/firmware-update-log-mram-se.out 2>&1 || true
			# Strip ANSI colour codes SETOOLS emits, then take the four 0xXXXXXXXX
			# words from the "[0x<off>] 0x.. 0x.. 0x.. 0x.." data line.
			mram_words=$(sed 's/\x1b\[[0-9;]*m//g' /tmp/firmware-update-log-mram-se.out | awk '
				/\[0x/ { n = 0; for (i = 1; i <= NF; i++) if ($i ~ /^0x[0-9A-Fa-f]{8}$/) w[n++] = toupper(substr($i, 3)) }
				END { for (j = 0; j < n && j < 4; j++) printf "%s%s", (j ? " " : ""), w[j] }')
			[ -n "$mram_words" ] && mram_line="MRAM(SE) @0x$fw_offset = $mram_words"
		fi
		if [ -z "$mram_words" ]; then
			# SWD fallback -- UNRELIABLE when the window denies the HE debug AP.
			mram_addr=$(printf '0x%08X' $((0x80000000 + 0x$fw_offset)))
			cat > /tmp/firmware-update-log-mram-read.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 $mram_addr, 0x4
exit
EOF
			"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-mram-read.jlink \
				2>/tmp/firmware-update-log-mram-read.err \
				>/tmp/firmware-update-log-mram-read.out || true
			mram_line=$(awk -v addr="${mram_addr#0x}" \
				'toupper($1) == toupper(addr) && $2 == "=" { print; exit }' \
				/tmp/firmware-update-log-mram-read.out)
			mram_words=$(printf '%s\n' "$mram_line" | awk '{ print toupper($3" "$4" "$5" "$6) }')
		fi
	fi
	baseline_words=$(printf '%s\n' "${ALP_AEN_FIREWALL_PROBE_BASELINE:-}" | tr ',:' '  ' | awk '
		{ for (i = 1; i <= NF; i++) printf "%s%s", (i == 1 ? "" : " "), toupper($i) }
	')
	pattern_landed=0
	[ -n "$mram_words" ] && [ "$mram_words" = "$fw_pattern" ] && pattern_landed=1
	baseline_changed=unknown
	if [ -n "$baseline_words" ] && [ -n "$mram_words" ]; then
		if [ "$mram_words" = "$baseline_words" ]; then
			baseline_changed=0
		else
			baseline_changed=1
		fi
	fi
	if [ "$fw_magic" != "46575052" ]; then
		echo "firewall probe magic mismatch; FW=$fw_magic" >&2
		exit 1
	fi
	[ -n "$mram_line" ] && echo "MRAM @0x$fw_offset: $mram_words"
	case "$fw_result" in
		00000002|00000003|00000006)
			if [ "$baseline_changed" = "1" ]; then
				echo "firewall verdict: FAIL - MRAM changed from the pre-flash baseline at offset 0x$fw_offset" >&2
				echo "firewall detail: before=$baseline_words after=$mram_words attempted=$fw_pattern" >&2
				exit 1
			fi
			if [ "$pattern_landed" -eq 1 ]; then
				echo "firewall verdict: FAIL - HE installed its attempted pattern at offset 0x$fw_offset" >&2
				echo "firewall detail: attempted=$fw_pattern after=$mram_words" >&2
				exit 1
			fi
			if [ "$fw_result" = "00000006" ] && [ -z "$mram_words" ]; then
				echo "firewall verdict: ERROR - HE could not read back; SWD MRAM compare is missing" >&2
				exit 1
			fi
			echo "firewall verdict: PASS - HE could not modify alp_ulog_partition"
			;;
		00000004)
			echo "firewall verdict: FAIL - HE changed alp_ulog_partition at offset 0x$fw_offset" >&2
			echo "firewall detail: write stage=0x$fw_stage flash_write status=0x$fw_detail" >&2
			echo "firewall probe did not prove HE write rejection; result=0x$fw_result" >&2
			exit 1
			;;
		00000005)
			echo "firewall verdict: ERROR - probe setup/read failed; stage=0x$fw_stage detail=0x$fw_detail" >&2
			echo "firewall probe did not prove HE write rejection; result=0x$fw_result" >&2
			exit 1
			;;
		*)
			echo "firewall verdict: ERROR - unexpected probe result 0x$fw_result" >&2
			echo "firewall probe did not prove HE write rejection; result=0x$fw_result" >&2
			exit 1
			;;
	esac
fi
