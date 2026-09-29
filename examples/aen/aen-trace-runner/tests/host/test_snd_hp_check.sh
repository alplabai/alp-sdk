#!/bin/bash
# tests/host/test_snd_hp_check.sh -- the TR_SND_HP=ON interlock of
# a32/release/build-release.sh (snd_hp_check.sh + sound-carriers.txt): every
# refusal path refuses, the one allowed configuration passes, and the full
# script refuses BEFORE it builds or packages anything.
set -u
cd "$(dirname "$0")/../.." || exit 1
t=$(mktemp -d "${TMPDIR:-/tmp}/tr-snd-hp-check.XXXXXX")
trap 'rm -rf "$t"' EXIT
fail=0
# shellcheck source=a32/release/snd_hp_check.sh
source a32/release/snd_hp_check.sh
list=a32/release/sound-carriers.txt

# A fake nm and a fake HP GAME build dir that passes every check.
printf '#!/bin/sh\necho "00001234 T tr_audio_render"\n' > "$t/nm" && chmod +x "$t/nm"
mk_build() { # dir cache_line soc_line test_define
	mkdir -p "$1/zephyr"
	echo "$2" > "$1/CMakeCache.txt"
	echo "$3" > "$1/zephyr/.config"
	echo "FLAGS = -D$4" > "$1/build.ninja"
	head -c 4096 /dev/zero > "$1/zephyr/zephyr.bin"
	: > "$1/zephyr/zephyr.elf"
}
mk_build "$t/good" TR_SND_REWORKED_U46:BOOL=ON CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y TR_SND_TEST=0
mk_build "$t/unreworked" TR_SND_REWORKED_U46:BOOL=OFF CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y TR_SND_TEST=0
mk_build "$t/he" TR_SND_REWORKED_U46:BOOL=ON CONFIG_SOC_AE822FA0E5597LS0_RTSS_HE=y TR_SND_TEST=0
mk_build "$t/test" TR_SND_REWORKED_U46:BOOL=ON CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y TR_SND_TEST=1

expect() { # want(0|1) why build serial
	local want=$1 why=$2 out rc
	out=$(snd_hp_check "$3" "$4" "$list" "$t/nm" 2>&1)
	rc=$?
	if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && { [ $rc -eq 0 ] || ! grep -q REFUSED <<<"$out"; }; }; then
		echo "FAIL snd_hp_check: $why (rc=$rc): $out"
		fail=1
	fi
}
expect 1 "no serial" "$t/good" ""
expect 1 "2026W36-0009 unit is denied" "$t/good" 2026W36-0009
expect 1 "unknown unit" "$t/good" 2026W36-0042
expect 1 "not configured TR_SND_REWORKED_U46=ON" "$t/unreworked" 2026W36-0002
expect 1 "an HE build" "$t/he" 2026W36-0002
expect 1 "a TEST build" "$t/test" 2026W36-0002
expect 1 "no build dir" "$t/missing" 2026W36-0002
expect 0 "reworked 2026W36-0002 unit + GAME HP build" "$t/good" 2026W36-0002

# End to end: build-release.sh must stop at the interlock (exit 3) before it
# runs make or app-gen-toc (both would leave a marker).
mkdir -p "$t/st/build/images" "$t/st/build/config" "$t/he/zephyr"
printf '#!/bin/sh\ntouch "%s/PACKAGED"\n' "$t" > "$t/st/app-gen-toc" && chmod +x "$t/st/app-gen-toc"
: > "$t/st/build/images/bl32.bin"; : > "$t/st/build/images/m55_stub_hp.bin"
: > "$t/st/build/config/app-device-config.json"
head -c 4096 /dev/zero > "$t/he/zephyr/zephyr.bin"; : > "$t/he/zephyr/zephyr.elf"
mkdir -p "$t/bin" && printf '#!/bin/sh\ntouch "%s/MADE"\n' "$t" > "$t/bin/make" && chmod +x "$t/bin/make"
out=$(PATH="$t/bin:$PATH" NM="$t/nm" TR_SND_HP=ON TR_SND_HP_BUILD="$t/good" TR_SND_CARRIER_SERIAL=2026W36-0009 \
	bash a32/release/build-release.sh "$t/st" "$t/he" 2>&1)
rc=$?
if [ $rc -ne 3 ] || ! grep -q "REFUSED" <<<"$out" || [ -e "$t/MADE" ] || [ -e "$t/PACKAGED" ]; then
	echo "FAIL build-release.sh did not refuse 2026W36-0009 up front (rc=$rc): $out"
	fail=1
fi

[ $fail = 0 ] && echo "test_snd_hp_check: ok"
exit $fail
