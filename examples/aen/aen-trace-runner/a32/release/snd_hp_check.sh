# shellcheck shell=bash
# a32/release/snd_hp_check.sh -- sourced by build-release.sh (and
# tests/host/test_snd_hp_check.sh). snd_hp_check SOUND_BUILD_DIR SERIAL
# ALLOWLIST NM [MODE]: 0 when the P10 sound image may go into HP_APP for that unit,
# else prints a loud refusal and returns 1. See sound-carriers.txt for why.
#   MODE sound    (default) the standalone sound/ GAME image (TR_SND_HP=ON alone);
#                 the combined hp_vision build is refused here (it is packaged with
#                 TR_HP_VISION=ON TR_SND_HP=ON).
#   MODE combined hp_vision built with -DTR_HP_SOUND=ON (TR_HP_VISION=ON TR_SND_HP=ON): the
#                 same carrier / rework / HP / synth / size checks, plus the embedded GAME
#                 build and, on the linked ELF, the sound's buffers, the heap its I2S queue
#                 comes from and its thread stack in the HP DTCM 0x20000000..0x200FFFFF.

snd_hp_refuse() {
	{
		echo "######################################################################"
		echo "build-release: REFUSED TR_SND_HP=ON -- $*"
		echo "  The HP sound image drives I2S3 (P9_3/4/5) at HP boot. On a stock"
		echo "  74LVC157 U46 carrier that contends. Nothing was packaged."
		echo "######################################################################"
	} >&2
}

snd_hp_check() {
	local sd=$1 serial=$2 list=$3 nm=$4 mode=${5:-sound} line verdict proj syms sym addr size
	if [ -z "$serial" ]; then
		snd_hp_refuse "set TR_SND_CARRIER_SERIAL=<unit serial> (one line of $list)"
		return 1
	fi
	line=$(grep -E "^${serial}[[:space:]]" "$list" | head -1)
	if [ -z "$line" ]; then
		snd_hp_refuse "unit $serial is not in $list"
		return 1
	fi
	verdict=$(echo "$line" | awk '{print $2}')
	if [ "$verdict" != allow ]; then
		snd_hp_refuse "unit $serial is '$verdict' in $list: $line"
		return 1
	fi
	if [ ! -f "$sd/CMakeCache.txt" ] || ! tr -d '\r' < "$sd/CMakeCache.txt" | grep -q '^TR_SND_REWORKED_U46:BOOL=ON$'; then
		snd_hp_refuse "$sd was not configured with TR_SND_REWORKED_U46=ON"
		return 1
	fi
	if ! grep -q '^CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y' "$sd/zephyr/.config" 2>/dev/null; then
		snd_hp_refuse "$sd is not an M55-HP build"
		return 1
	fi
	if ! grep -q 'TR_SND_TEST=0' "$sd/build.ninja" 2>/dev/null; then
		snd_hp_refuse "$sd is not a TR_SND_MODE=GAME build"
		return 1
	fi
	proj=$(tr -d '\r' < "$sd/CMakeCache.txt" | sed -n 's/^CMAKE_PROJECT_NAME:[A-Za-z]*=//p' | head -1)
	case "$mode" in
	sound)
		if [ "$proj" = trace_runner_hp_vision ]; then
			snd_hp_refuse "$sd is the combined hp_vision image: package it with TR_HP_VISION=ON TR_SND_HP=ON (hp_vision_check.sh + this check)"
			return 1
		fi
		;;
	combined)
		if [ "$proj" != trace_runner_hp_vision ]; then
			snd_hp_refuse "$sd is not an hp_vision build (project '$proj')"
			return 1
		fi
		if ! tr -d '\r' < "$sd/CMakeCache.txt" | grep -qiE '^TR_HP_SOUND:[A-Za-z]*=(ON|1|YES|TRUE|Y)$' ||
			! grep -q 'TR_SND_EMBED=1' "$sd/build.ninja" 2>/dev/null; then
			snd_hp_refuse "$sd was not configured with TR_HP_SOUND=ON (no embedded game sound)"
			return 1
		fi
		;;
	*)
		snd_hp_refuse "unknown mode '$mode'"
		return 1
		;;
	esac
	# Captured, not piped straight into `grep -q`: under this script's
	# caller's `set -o pipefail`, grep -q closes its stdin on the first
	# match and SIGPIPEs $nm mid-write on a real ELF, which pipefail then
	# reports as the whole pipeline failing (see hp_vision_check.sh's
	# identical fix for the reproduction).
	if ! grep -q ' tr_audio_render$' <<<"$("$nm" "$sd/zephyr/zephyr.elf" 2>/dev/null)"; then
		snd_hp_refuse "$sd/zephyr/zephyr.elf lacks the synth"
		return 1
	fi
	if [ "$mode" = combined ]; then
		# The sound path's memory is HP-local DTCM: a link-time fact, so checked here on the ELF.
		# Every one of these must exist and lie in 0x20000000..0x200FFFFF. `nm -S`: address, size,
		# type, name. kheap__system_heap is the system heap's storage (alp-sdk's i2s backend
		# k_malloc()s its 2-block slab from it).
		syms=$("$nm" -S "$sd/zephyr/zephyr.elf" 2>/dev/null)
		for sym in s_mono s_stereo kheap__system_heap _k_thread_stack_tr_snd_thread; do
			read -r addr size <<<"$(awk -v s="$sym" 'NF == 4 && $4 == s { print $1, $2; exit }' <<<"$syms")"
			if [ -z "$addr" ] || [ -z "$size" ]; then
				snd_hp_refuse "$sd/zephyr/zephyr.elf has no '$sym' (the embedded sound's buffers / heap / stack)"
				return 1
			fi
			if [ $((16#$addr)) -lt $((0x20000000)) ] || [ $((16#$addr + 16#$size)) -gt $((0x20100000)) ]; then
				snd_hp_refuse "$sym is at 0x$addr (+0x$size), outside the HP DTCM 0x20000000..0x200FFFFF"
				return 1
			fi
		done
	fi
	if [ "$(stat -c %s "$sd/zephyr/zephyr.bin" 2>/dev/null || echo 999999999)" -gt 262144 ]; then
		snd_hp_refuse "$sd/zephyr/zephyr.bin is missing or > 256 KiB HP ITCM"
		return 1
	fi
	echo "build-release: TR_SND_HP=ON ($mode image) allowed for unit $serial: $line" >&2
	return 0
}
