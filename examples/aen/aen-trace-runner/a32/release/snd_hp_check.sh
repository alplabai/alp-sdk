# shellcheck shell=bash
# a32/release/snd_hp_check.sh -- sourced by build-release.sh (and
# tests/host/test_snd_hp_check.sh). snd_hp_check SOUND_BUILD_DIR SERIAL
# ALLOWLIST NM: 0 when the P10 sound image may go into HP_APP for that unit,
# else prints a loud refusal and returns 1. See sound-carriers.txt for why.

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
	local sd=$1 serial=$2 list=$3 nm=$4 line verdict
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
	if [ ! -f "$sd/CMakeCache.txt" ] || ! grep -q '^TR_SND_REWORKED_U46:BOOL=ON$' "$sd/CMakeCache.txt"; then
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
	# Captured, not piped straight into `grep -q`: under this script's
	# caller's `set -o pipefail`, grep -q closes its stdin on the first
	# match and SIGPIPEs $nm mid-write on a real ELF, which pipefail then
	# reports as the whole pipeline failing (see hp_vision_check.sh's
	# identical fix for the reproduction).
	if ! grep -q ' tr_audio_render$' <<<"$("$nm" "$sd/zephyr/zephyr.elf" 2>/dev/null)"; then
		snd_hp_refuse "$sd/zephyr/zephyr.elf lacks the synth"
		return 1
	fi
	if [ "$(stat -c %s "$sd/zephyr/zephyr.bin" 2>/dev/null || echo 999999999)" -gt 262144 ]; then
		snd_hp_refuse "$sd/zephyr/zephyr.bin is missing or > 256 KiB HP ITCM"
		return 1
	fi
	echo "build-release: TR_SND_HP=ON allowed for unit $serial: $line" >&2
	return 0
}
