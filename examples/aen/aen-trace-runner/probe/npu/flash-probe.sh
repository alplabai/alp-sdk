#!/usr/bin/env bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# probe/npu/flash-probe.sh -- Flow D for the M55-HP NPU probe (README.md).
#
#   flash-probe.sh readback OUT.bin                      # 1. save live MRAM 0x80000000..0x8057FFFF (read-only)
#   flash-probe.sh write SETOOLS_COPY PAYLOAD [FLAGS]    # 2. payload @0x80100000 + probe ATOC, reset, prove, read results
#   flash-probe.sh results                               # 3. re-read the result block (read-only)
#   flash-probe.sh restore OUT.bin [FLAGS]               # 4. put back every sector step 2 touched, from step 1's file
#
# FLAGS: --atoc-unqueryable (a place with no SE-UART, e.g. the E1M-AEN803 2026W36-0009 EVK) or
# --replace-atoc -- passed to alp-sdk's bench_flowd_atoc_guard unchanged.
# FLOWD_DRY_RUN=1: plans, pads and prints the write session, touches no probe.
#
# Everything board-facing is alp-sdk's Flow D machinery
# (scripts/bench/aen/bench-env.sh, the #2233 sector pad + read-back proof +
# race check), exactly as flash-jlink-hp.sh drives it; this only adds the
# second blob. Needs LG_PLACE, SETOOLS_DIR (any; the ATOC comes from
# SETOOLS_COPY), and ALP_SDK_BENCH (default: the alp-sdk checkout this example sits in).
set -euo pipefail

# shellcheck source=/dev/null
source "${ALP_SDK_BENCH:-$(cd "$(dirname "$0")/../../../../.." && pwd)}/scripts/bench/aen/bench-env.sh"

PAYLOAD_ADDR=0x80100000
RESULT_ADDR=0x0237F200
MRAM_LO=0x80000000
MRAM_LEN=0x580000
JLINK_ARGS=(bench_jlink_run)
T="${TMPDIR:-/tmp}/tr-npu-flash"
mkdir -p "$T"

die() { echo "flash-probe: $*" >&2; exit 1; }

jlink_read() { # <commands...> -- one read-only session, generic device
	{ printf 'si SWD\nspeed %s\ndevice %s\nconnect\n' "$JLINK_SPEED" "$JLINK_DEVICE_READ"; printf '%s\n' "$@"; echo exit; } \
		>"$T/read.jlink"
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$T/read.jlink" >"$T/read.out" 2>&1 || true
	bench_jlink_assert_connected "$T/read.out" "tr-npu read" || exit 7
}

results() {
	jlink_read "mem32 $RESULT_ADDR, 0x61" "Sleep 2000" "mem32 $RESULT_ADDR, 0x8"
	echo "----- result block @$RESULT_ADDR: magic 4E505552, CPUID, VTOR, heartbeat, stage, status, MHz, verdict(1 PASS 2 FAIL) -----"
	grep -E '^0237F[23]' "$T/read.out"
	echo "(the second read's heartbeat must differ from the first: the HP is running)"
}

# <tag> <flags...> <file:addr>... -- the flash-jlink-hp.sh write sequence, N blobs.
flowd_write() {
	local tag="$1" replace=0 unq=0
	shift
	local -a blobs=()
	for a in "$@"; do
		case "$a" in
		--replace-atoc) replace=1 ;;
		--atoc-unqueryable) unq=1 ;;
		*) blobs+=("$a") ;;
		esac
	done
	bench_flowd_atoc_guard "$replace" "$unq" "$tag" HP-APP || exit $?
	if [ -z "${FLOWD_DRY_RUN:-}" ]; then
		printf 'si SWD\nspeed %s\ndevice %s\nconnect\nexit\n' "$JLINK_SPEED" "$JLINK_DEVICE_READ" >"$T/pre.jlink"
		"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$T/pre.jlink" >"$T/pre.out" 2>&1 || true
		bench_jlink_assert_aen_dpidr "$T/pre.out" "MRAM write preflight" || exit 4
	fi
	local scratch
	scratch="$(mktemp -d "$T/flowd-XXXXXX")"
	bench_flowd_prepare_write "$tag" "$scratch" "${blobs[@]}" || exit 9
	local loadbin prewrite
	loadbin="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)" || exit 9
	[ -n "$loadbin" ] || die "no loadbin lines -- refusing"
	prewrite="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$scratch/prewrite")"
	printf 'si SWD\nspeed %s\ndevice %s\nconnect\n%s\n%s\nRSetType 2\nr\ng\nexit\n' \
		"$JLINK_SPEED" "$JLINK_DEVICE_FLASH" "$prewrite" "$loadbin" >"$T/write.jlink"
	if [ -n "${FLOWD_DRY_RUN:-}" ]; then
		echo "--- DRY RUN: nothing written; manifest $FLOWD_MANIFEST; write session: ---"
		cat "$T/write.jlink"
		exit 10
	fi
	local rc=0
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$T/write.jlink" >"$T/write.out" 2>&1 || rc=$?
	[ "$rc" -ne 13 ] || die "bench_jlink_run refused the write session (rc=13)"
	grep -qi "Could not connect to the target device" "$T/write.out" && die "$JLINK_DEVICE_FLASH did not connect"
	local proof=0 race=0
	bench_flowd_proof "$tag" "$FLOWD_MANIFEST" "$scratch/postread" || proof=1
	bench_flowd_check_race "$tag" "$FLOWD_SECTORS_FILE" "$scratch/sectors" "$scratch/prewrite" "$T/write.out" || race=1
	echo "flowd scratch (pre-write sectors kept here): $scratch"
	[ "$race" -eq 0 ] || { echo "!! RACE -- restore from $scratch/sectors + $scratch/prewrite"; exit 11; }
	[ "$proof" -eq 0 ] || { echo "!! READ-BACK PROOF FAILED -- do not treat the board as flashed"; exit 3; }
	echo "verify: read-back proof OK (not a cold-cycle persistence proof)"
}

cmd="${1:-}"
shift || true
case "$cmd" in
readback)
	out="$(realpath -m "${1:?OUT.bin}")"
	jlink_read "savebin $(bench_flowd_jlink_path "$out") $MRAM_LO $MRAM_LEN"
	[ "$(stat -c %s "$out")" -eq $((MRAM_LEN)) ] || die "short read-back: $out"
	echo "MRAM $MRAM_LO..+$MRAM_LEN -> $out  md5 $(md5sum <"$out" | cut -d' ' -f1)"
	;;
write)
	st="$(realpath "${1:?SETOOLS_COPY}")"
	pl="$(realpath "${2:?PAYLOAD}")"
	shift 2
	pkg="$st/build/AppTocPackage.bin"
	addr=$(grep -a "APP Package Start Address:" "$st/build/app-package-map.txt" | awk "{print \$NF}" | tail -1)
	[ -f "$pkg" ] && [ -n "$addr" ] || die "no ATOC package in $st/build (run app-gen-toc, README.md)"
	grep -aq tr_npu_probe_hp.bin "$st/build/app-package-map.txt" || die "$st's package is not the NPU probe's"
	# restore below puts back 0x80100000..0x8040FFFF and 0x80550000..0x8057FFFF only.
	[ $((PAYLOAD_ADDR + $(stat -c %s "$pl"))) -le $((0x80410000)) ] || die "payload past 0x80410000: widen restore"
	[ $((addr)) -ge $((0x80550000)) ] || die "ATOC package starts below 0x80550000: widen restore"
	flowd_write tr-npu-probe "$@" "$pl:$PAYLOAD_ADDR" "$pkg:$addr"
	sleep 5 # SE boot + the probe's two passes (~2 s of sustained loop each)
	results
	;;
results)
	results
	;;
restore)
	rb="$(realpath "${1:?OUT.bin from readback}")"
	shift
	[ "$(stat -c %s "$rb")" -eq $((MRAM_LEN)) ] || die "$rb is not a full $MRAM_LEN B read-back"
	# The sectors a probe write touches: the payload's and the probe ATOC's
	# (whole 16 KiB sectors, from the package map; README.md step 4).
	blobs=()
	for r in 0x80100000:0x80410000 0x80550000:0x80580000; do
		lo=${r%:*} hi=${r#*:}
		f="$T/restore-$lo.bin"
		dd if="$rb" of="$f" bs=16384 skip=$(((lo - MRAM_LO) / 16384)) count=$(((hi - lo) / 16384)) status=none
		blobs+=("$f:$lo")
	done
	flowd_write tr-npu-restore "$@" "${blobs[@]}"
	;;
*)
	sed -n '2,9p' "$0"
	exit 2
	;;
esac
