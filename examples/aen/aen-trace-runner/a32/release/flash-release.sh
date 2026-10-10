#!/usr/bin/env bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# a32/release/flash-release.sh -- Flow D for the trace-runner NPU-body
# release (a32/release/build-release.sh TR_HP_VISION=ON output). Reuses the
# SAME proven machinery probe/npu/flash-probe.sh already uses for the NPU
# probe (alp-sdk's scripts/bench/aen/bench-env.sh: alp-sdk#2233's sector-pad
# + fresh-session proof + pre-read/write race check, and the SE-UART ATOC
# guard), generalized from 2 blobs to however many flowd/*.bin the release
# produced (today: bl32, a32_app, atoc, movenet_model). FLASH-RECIPE.md is
# the full ordered procedure this implements -- read that first; this
# script is steps 2-5 of it, not steps 1/6/7 (the read-back capture is its
# own subcommand here, but the cold-cycle + repeat-check + 3-cold-boots
# steps are physical actions FLASH-RECIPE.md spells out by hand).
#
#   flash-release.sh readback OUT.bin                     # step 1
#   flash-release.sh check-a32                            # step 3
#   flash-release.sh write SETOOLS_COPY READBACK.bin [--replace-atoc|--atoc-unqueryable] [--no-reset|--reset]
#                                                          # steps 4-5 (halt once, noreset
#                                                          # loadbins, skip bl32 if unchanged,
#                                                          # fresh-session proof). The session
#                                                          # ends with a warm pin reset into the
#                                                          # new image UNLESS the package carries
#                                                          # the game sound (TR_HP_SOUND): that
#                                                          # image must COLD-boot, so there the
#                                                          # default is --no-reset (the core stays
#                                                          # as the write left it: power-cycle
#                                                          # the board). --no-reset / --reset
#                                                          # force either way.
#   flash-release.sh restore READBACK.bin [--replace-atoc|--atoc-unqueryable]
#                                                          # recovery: put every touched
#                                                          # sector back from a read-back
#
# Needs LG_PLACE, SETOOLS_DIR (any; the ATOC's own address comes from
# SETOOLS_COPY), ALP_SDK_BENCH (default: the alp-sdk checkout this example sits in). Export
# SE_UART for the ATOC guard to query the resident TOC first (see
# FLASH-RECIPE.md's recovery section for the exact SETOOLS command this
# guard runs); pass --atoc-unqueryable only if this bench slot truly has
# none wired.
set -euo pipefail

# shellcheck source=/dev/null
source "${ALP_SDK_BENCH:-$(cd "$(dirname "$0")/../../../../.." && pwd)}/scripts/bench/aen/bench-env.sh"

A32_CHECK_ADDR=0x02401184 # tr_mbox.h TR_MBOX_ADDR (0x02401000) + ctrl_entry offset (+0x184)
A32_EXPECT=0x02500000     # the renderer's SRAM1 entry (a32/renderer/renderer.ld ORIGIN) -- if
                          # ctrl_entry reads anything else, the A32 is not confirmed running
                          # from SRAM1 and MRAM must not be written (it could be fetching from
                          # the very sectors about to be rewritten).
MRAM_LO=0x80000000
MRAM_LEN=0x580000
JLINK_ARGS=(bench_jlink_run)
T="${TMPDIR:-/tmp}/tr-release-flash"
mkdir -p "$T"

die() { echo "flash-release: $*" >&2; exit 1; }

jlink_read() { # <commands...> -- one read-only session, generic device
	{ printf 'si SWD\nspeed %s\ndevice %s\nconnect\n' "$JLINK_SPEED" "$JLINK_DEVICE_READ"; printf '%s\n' "$@"; echo exit; } \
		>"$T/read.jlink"
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$T/read.jlink" >"$T/read.out" 2>&1 || true
	bench_jlink_assert_connected "$T/read.out" "tr-release read" || exit 7
}

do_write() { # <tag> <flags...> <file:addr>...
	local tag="$1" replace=0 unq=0 noreset="${FLASH_NO_RESET_DEFAULT:-0}"
	shift
	local -a blobs=()
	for a in "$@"; do
		case "$a" in
		--replace-atoc) replace=1 ;;
		--atoc-unqueryable) unq=1 ;;
		--no-reset) noreset=1 ;;
		--reset) noreset=0 ;;
		*) blobs+=("$a") ;;
		esac
	done
	# The real gettoc row names for this release's ATOC (the JSON keys
	# e1m-aen-evk-trace-runner.json uses), not a made-up "HP-APP" (fix
	# round 2 finding: the guard did exact-string matching against that,
	# so it aborted on every real gettoc capture, which reports these four
	# with underscores).
	bench_flowd_atoc_guard "$replace" "$unq" "$tag" BOOTLOAD A32_APP HP_APP HE_APP || exit $?
	if [ -z "${FLOWD_DRY_RUN:-}" ]; then
		printf 'si SWD\nspeed %s\ndevice %s\nconnect\nexit\n' "$JLINK_SPEED" "$JLINK_DEVICE_READ" >"$T/pre.jlink"
		"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript "$T/pre.jlink" >"$T/pre.out" 2>&1 || true
		bench_jlink_assert_aen_dpidr "$T/pre.out" "MRAM write preflight" || exit 4
	fi
	local scratch
	scratch="$(mktemp -d "$T/flowd-XXXXXX")"
	bench_flowd_prepare_write "$tag" "$scratch" "${blobs[@]}" || exit 9
	local loadbin prewrite
	loadbin="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 1)" || exit 9 # noreset=1: every loadbin, no implicit reset
	[ -n "$loadbin" ] || die "no loadbin lines -- refusing"
	prewrite="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$scratch/prewrite")"
	# NO RESET BEFORE PROGRAMMING (fix round 2 finding, and #1902:
	# scripts/bench/aen/flash-jlink-mramxip.sh's own header, whose bench-measured evidence this
	# follows verbatim): `RSetType 2; r` before the halt was found to DESTROY
	# the debug access the halt needs on this part (AP[3], the M55 debug
	# domain, is present pre-reset 8/8 and absent post-reset 8/8 in that
	# bench log) -- 0 of 8 halts succeeded after a reset. `h` alone, on the
	# LIVE core, halted 12/12. The reset that boots the new image still runs,
	# but only ONCE, at the very end, after every loadbin (RSetType 2; r; g
	# below) -- never before or between writes, so nothing is ever executing
	# from MRAM while this session is rewriting it (the exact hazard the
	# step-3 A32 check exists to catch beforehand).
	# `exec SetSkipProgOnCRCMatch = 0`: force every loadbin to actually
	# reprogram even if J-Link's own CRC check thinks the sector already
	# matches -- do not trust that optimisation on a first-time write to a
	# part profile this release has not flashed before.
	# The boot reset is skipped with --no-reset (the default for a package that carries the game
	# sound): a TR_HP_SOUND image needs a COLD boot (both cores up together, the HE's lease
	# handshake from a clean record), which a warm pin reset into it does not give.
	local bootreset='RSetType 2\nr\ng\n'
	[ "$noreset" -eq 0 ] || bootreset=''
	printf 'si SWD\nspeed %s\ndevice %s\nconnect\nexec SetSkipProgOnCRCMatch = 0\nh\n%s\n%s\n'"$bootreset"'exit\n' \
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
	# CONFIRM THE HALT, SCOPED TO BEFORE PROGRAMMING
	# (scripts/bench/aen/flash-jlink-mramxip.sh:420-446, adopted verbatim): `h` prints a
	# "PC = ........, CycleCnt = ..." register dump ONLY when the core
	# actually halted; on failure it prints "WARNING: CPU could not be
	# halted" and no PC line. The trailing RSetType 2/r/g that BOOTS the
	# image at the end of this SAME session also tries to halt and is
	# EXPECTED to fail on this part (AP[3] is gone after any reset) --
	# scope strictly to the portion BEFORE the first "Downloading file"
	# (where loadbin starts), or that harmless trailing failure reads as
	# this check failing over a write that actually succeeded.
	local preprog="$scratch/write.preprog"
	awk '/Downloading file/{exit} {print}' "$T/write.out" >"$preprog"
	# Fix round 3 ruling: run the SAME fresh-session proof + race check on a
	# halt failure too, BEFORE claiming "NOTHING was written" -- that claim
	# was previously an ASSUMPTION from "noreset means no fallback", never
	# independently verified. If the halt somehow failed but the chip was
	# still written (a prior session, a race, an unexpected recovery), the
	# proof below is what actually knows that, not this script's guess.
	local halted=1
	if ! grep -qE '^PC = [0-9A-F]{8}, CycleCnt = ' "$preprog"; then
		halted=0
		echo "!! HALT FAILED before programming -- the core was RUNNING for the flash." >&2
		grep -iE "Could not find core in Coresight setup|CPU could not be halted|Failed to halt CPU|Failed to preserve target RAM" "$preprog" | head -5 >&2
	else
		echo "halt: core halted before programming ($(grep -oE '^PC = [0-9A-F]{8}' "$preprog" | head -1))"
	fi
	local proof=0 race=0
	bench_flowd_proof "$tag" "$FLOWD_MANIFEST" "$scratch/postread" || proof=1
	bench_flowd_check_race "$tag" "$FLOWD_SECTORS_FILE" "$scratch/sectors" "$scratch/prewrite" "$T/write.out" || race=1
	echo "flowd scratch (pre-write sectors kept here): $scratch"
	if [ "$halted" -eq 0 ]; then
		if [ "$proof" -eq 0 ] && [ "$race" -eq 0 ]; then
			echo "!! HALT FAILED, but the fresh-session proof PASSED -- something DID land." >&2
			echo "   Do not assume 'noreset means nothing happened': treat the board as" >&2
			echo "   WRITTEN and investigate the halt failure separately." >&2
			exit 6
		fi
		echo "   With 'loadbin ..., noreset' there is no fallback reset, and the fresh-session" >&2
		echo "   proof/race check above did NOT confirm a write either -- NOTHING was written" >&2
		echo "   -- MRAM still holds its previous contents. This is NOT corruption." >&2
		exit 5
	fi
	[ "$race" -eq 0 ] || { echo "!! RACE -- restore from $scratch/sectors + $scratch/prewrite"; exit 11; }
	[ "$proof" -eq 0 ] || { echo "!! READ-BACK PROOF FAILED -- do not treat the board as flashed, do not resume (g)"; exit 3; }
	echo "verify: read-back proof OK (fresh-session, NOT a cold-cycle persistence proof -- FLASH-RECIPE.md steps 6-7)"
	if [ "$noreset" -eq 0 ]; then
		echo "the write session's own trailing RSetType 2/r/g already resumed the target -- FLASH-RECIPE.md step 6 (cold cycle) is the persistence proof, not this run"
	else
		echo "NO boot reset was issued (--no-reset): the target is still halted/running its OLD image. POWER-CYCLE the board (cold boot) -- FLASH-RECIPE.md step 6"
	fi
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
check-a32)
	jlink_read "mem32 $A32_CHECK_ADDR, 1"
	got=$(grep -oiE "^0*${A32_CHECK_ADDR#0x} = [0-9A-Fa-f]+" "$T/read.out" | awk '{print $3}')
	[ -n "$got" ] || die "could not parse the mem32 read of $A32_CHECK_ADDR from $T/read.out -- read it by eye before proceeding"
	if [ "$((16#${got}))" -ne "$((A32_EXPECT))" ]; then
		die "ctrl_entry=0x$got, want $A32_EXPECT -- the A32 is NOT confirmed running from SRAM1. DO NOT WRITE. Halt it (a32/renderer/halt.jlink) and re-check."
	fi
	echo "flash-release: A32 check OK -- ctrl_entry=0x$got (running from SRAM1, not MRAM)"
	;;
write)
	st="$(realpath "${1:?SETOOLS_COPY}")"
	rb="$(realpath "${2:?READBACK.bin, from the readback step and build-release.sh 3rd arg}")"
	shift 2
	[ "$(stat -c %s "$rb")" -eq $((MRAM_LEN)) ] || die "$rb is not a full $MRAM_LEN B read-back"
	fd="$st/build/flowd"
	[ -d "$fd" ] || die "$fd missing -- run build-release.sh SETOOLS_COPY HE_BUILD \"$rb\" first (3rd arg: sector-merged blobs)"
	# This flow is for the HP_VISION release specifically -- refuse a package
	# that was not built with TR_HP_VISION=ON (fix round 3 ruling): without
	# the model item, HP_APP would be whatever build-release.sh's m55_stub_hp
	# default is, silently writable through this exact same command.
	grep -q movenet_model "$fd/recipe.txt" 2>/dev/null || \
		die "$fd/recipe.txt has no movenet_model item -- this is not a TR_HP_VISION=ON package"
	blobs=()
	for f in "$fd"/*.bin; do
		[ -e "$f" ] || continue
		name=$(basename "$f" .bin)
		addr="0x${name##*-0x}"
		# Skip ANY blob that is byte-identical to the live read-back at that
		# address (fix round 2: this used to be bl32-only; the ruling is
		# every blob) -- one fewer sector rewritten for no reason, and one
		# fewer sector this write session's own halt/noreset risk touches.
		sz=$(stat -c %s "$f")
		off=$(( (16#${addr#0x}) - (16#${MRAM_LO#0x}) ))
		if cmp -s <(dd if="$rb" bs=1 skip="$off" count="$sz" status=none) "$f"; then
			echo "flash-release: $name unchanged vs the read-back -- skipping" >&2
			continue
		fi
		# bl32 (the bootloader/TF-A) is the one item this release is not in
		# the business of updating -- refuse rather than write a changed one
		# (fix round 3 ruling): this recipe's whole point is hp_vision + the
		# model, and a bl32 mismatch is far more likely a stale/wrong
		# read-back or SETOOLS_COPY than an intended bootloader update this
		# flow should just carry along.
		if [[ "$name" == bl32-* ]]; then
			die "$name differs from the live read-back -- refusing to write a changed bl32 (this release does not update the bootloader)"
		fi
		blobs+=("$f:$addr")
	done
	[ "${#blobs[@]}" -gt 0 ] || die "nothing to write (everything matched the read-back?)"
	# A package that carries the game sound cold-boots: no warm reset at the end of the session.
	if grep -aq 'I2S_SELECT = 0 (amps)' "$st/build/images/trace_runner_hp_vision.bin" 2>/dev/null; then
		export FLASH_NO_RESET_DEFAULT=1
		echo "flash-release: the HP image carries the game sound -> --no-reset (cold-boot it; pass --reset to override)" >&2
	fi
	do_write tr-release "$@" "${blobs[@]}"
	;;
restore)
	rb="$(realpath "${1:?READBACK.bin}")"
	shift
	[ "$(stat -c %s "$rb")" -eq $((MRAM_LEN)) ] || die "$rb is not a full $MRAM_LEN B read-back"
	# Restores the four ranges this release ever touches. If a future
	# release adds a fifth item, extend this list (or read it from a saved
	# flowd/recipe.txt instead of hardcoding it here).
	blobs=()
	for r in 0x80000000:0x8000C000 0x80020000:0x800A0000 0x80100000:0x80354000 0x80510000:0x80580000; do
		lo=${r%:*} hi=${r#*:}
		f="$T/restore-$lo.bin"
		dd if="$rb" of="$f" bs=16384 skip=$(((lo - MRAM_LO) / 16384)) count=$(((hi - lo) / 16384)) status=none
		blobs+=("$f:$lo")
	done
	do_write tr-release-restore "$@" "${blobs[@]}"
	;;
*)
	sed -n '2,20p' "$0"
	exit 2
	;;
esac
