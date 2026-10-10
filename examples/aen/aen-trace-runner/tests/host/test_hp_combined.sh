#!/bin/bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# tests/host/test_hp_combined.sh -- the combined HP image (hp_vision built with -DTR_HP_SOUND=ON: the
# camera + NPU pipeline AND the game sound in one HP_APP), its build gate and its release
# interlocks (a32/release/build-release.sh TR_HP_VISION=ON TR_SND_HP=ON):
#   A. hp_vision/sound_gate.cmake: TR_HP_SOUND only with TR_SND_REWORKED_U46=ON, default OFF;
#   B. hp_vision_check.sh + snd_hp_check.sh over every HP_APP combination: combined +
#      2026W36-0002 allowed, + 2026W36-0009 refused, without REWORKED refused, a plain hp_vision
#      passes WITHOUT the sound check, a sound image never passes without it, two separate HP
#      images refused, the sound buffers outside the HP DTCM refused, an HE without the lease glue
#      refused, the DEV underrun positive control refused (cache or ELF), an HE built for another
#      camera than the HP refused, ...;
#   C. source interlocks: I2S_SELECT is only ever written 0; the i2s3 overlay only in the
#      TR_HP_SOUND branch; the lease is entered/left/returned in the right order;
#   D. end to end: build-release.sh refuses combined + 2026W36-0009 before it builds anything.
set -u
cd "$(dirname "$0")/../.." || exit 1
t=$(mktemp -d "${TMPDIR:-/tmp}/tr-hp-combined.XXXXXX")
trap 'rm -rf "$t"' EXIT
fail=0
FAILS() { echo "FAIL $*"; fail=1; }

# ---- A. the CMake gate -------------------------------------------------------------------
gate() { # want(ON|OFF|FATAL) why cmake-args...
	local want=$1 why=$2 out rc
	shift 2
	out=$(cmake "$@" -P hp_vision/sound_gate.cmake 2>&1)
	rc=$?
	if [ "$want" = FATAL ]; then
		{ [ $rc -ne 0 ] && grep -q 'reworked U46' <<<"$out"; } || FAILS "sound_gate: $why (rc=$rc): $out"
	else
		{ [ $rc -eq 0 ] && grep -q "TR_HP_SOUND=$want\$" <<<"$out"; } || FAILS "sound_gate: $why, want $want (rc=$rc): $out"
	fi
}
if command -v cmake >/dev/null; then
	gate OFF "no flags: a plain hp_vision has no sound"
	gate OFF "REWORKED ON alone: the sound is opt-in" -DTR_SND_REWORKED_U46=ON
	gate ON "REWORKED ON + TR_HP_SOUND ON" -DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=ON
	gate OFF "REWORKED ON, sound off" -DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=OFF
	gate FATAL "sound without REWORKED" -DTR_SND_REWORKED_U46=OFF -DTR_HP_SOUND=ON
	gate FATAL "sound, REWORKED not given" -DTR_HP_SOUND=ON
	gate FATAL "sound =1, REWORKED =0" -DTR_SND_REWORKED_U46=0 -DTR_HP_SOUND=1
else
	FAILS "cmake not found (the sound gate is not tested)"
fi

# ---- B. the checks, through build-release.sh's own step 0 ------------------------------------
# fake nm: `nm -S` lines (addr size type name) for the combined image's symbols; a dir's NM file
# (next to its zephyr/) overrides the default.
cat >"$t/nm" <<'EOF'
#!/bin/sh
for a in "$@"; do f=$a; done
d=$(dirname "$(dirname "$f")")
if [ -f "$d/NM" ]; then cat "$d/NM"; exit 0; fi
echo "00001234 00000040 T tr_pslot_write"
echo "000031c0 00000200 T tr_audio_render"
echo "00003200 00000040 T tr_bus2_he_frame"
echo "2002b8b4 00000200 b s_mono"
echo "2002b4b4 00000400 b s_stereo"
echo "200350a0 00010000 B kheap__system_heap"
echo "2002e360 00002000 B _k_thread_stack_tr_snd_thread"
EOF
chmod +x "$t/nm"
HP='BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp'
SND_ELF='[snd] 2 I2S_SELECT = 0 (amps) -> %d'
dts() { # file extra_status_for_sound_nodes(okay|none)
	{
		echo "/ {"
		echo "	soc {"
		printf '\t\ti2c1: i2c@49011000 {\n\t\t\tstatus = "okay";\n\t\t};\n'
		printf '\t\tcam: cam@49030000 {\n\t\t\tstatus = "okay";\n\t\t};\n'
		if [ "$2" = okay ]; then
			for n in i2c@49012000 gpio@49005000 i2s@49017000 spi@48104000 gpio@42002000; do
				printf '\t\t%s {\n\t\t\tstatus = "okay";\n\t\t};\n' "$n"
			done
		fi
		echo "	};"
		echo "};"
	} >"$1"
}
mk() { # dir "cache lines" "build.ninja defines" elf-text sound-nodes
	mkdir -p "$1/zephyr"
	printf '%b\n' "$HP\nTR_CAM_ROTATE:STRING=90\nTR_CAM_MIRROR:BOOL=ON\nCMAKE_PROJECT_NAME:STATIC=trace_runner_hp_vision\n$2" >"$1/CMakeCache.txt"
	echo "CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y" >"$1/zephyr/.config"
	echo "FLAGS = $3" >"$1/build.ninja"
	printf '\177ELF fake\0%s\0' "${4:-vision}" >"$1/zephyr/zephyr.elf"
	head -c 4096 /dev/zero >"$1/zephyr/zephyr.bin"
	dts "$1/zephyr/zephyr.dts" "${5:-okay}"
}
SOUND_ON='TR_HP_SOUND:BOOL=ON\nTR_SND_REWORKED_U46:BOOL=ON'
EMB='-DTR_SND_TEST=0 -DTR_SND_EMBED=1'
mk "$t/comb" "$SOUND_ON" "$EMB" "$SND_ELF"
mk "$t/comb-unrew" 'TR_HP_SOUND:BOOL=ON\nTR_SND_REWORKED_U46:BOOL=OFF' "$EMB" "$SND_ELF"
mk "$t/comb-noembed" "$SOUND_ON" "-DTR_SND_TEST=0" "$SND_ELF"
mk "$t/comb-test" "$SOUND_ON" "-DTR_SND_TEST=1 -DTR_SND_EMBED=1" "$SND_ELF"
mk "$t/comb-uart5" "$SOUND_ON" "$EMB" "$SND_ELF"
sed -i 's|^\t};$|\t\tuart@4901d000 {\n\t\t\tstatus = "okay";\n\t\t};\n\t};|' "$t/comb-uart5/zephyr/zephyr.dts"
for v in sram heap-over nostack; do mk "$t/comb-$v" "$SOUND_ON" "$EMB" "$SND_ELF"; done
# the DEV underrun positive control, by its cache entry and by its console text in the ELF
mk "$t/comb-underrun" "$SOUND_ON"'\nTR_SND_UNDERRUN_TEST:BOOL=ON' "$EMB" "$SND_ELF"
mk "$t/comb-underrun-elf" "$SOUND_ON"'\nTR_SND_UNDERRUN_TEST:BOOL=OFF' "$EMB" "$SND_ELF"
printf '[snd] underrun control: I2S3 IRQ held off 2000 us\0' >>"$t/comb-underrun-elf/zephyr/zephyr.elf"
mk "$t/comb-underrun-off" "$SOUND_ON"'\nTR_SND_UNDERRUN_TEST:BOOL=OFF' "$EMB" "$SND_ELF"
printf '%s\n' "00001234 00000040 T tr_pslot_write" "000031c0 00000200 T tr_audio_render" "00003200 00000040 T tr_bus2_he_frame" \
	"02200000 00000200 b s_mono" "2002b4b4 00000400 b s_stereo" "200350a0 00010000 B kheap__system_heap" \
	"2002e360 00002000 B _k_thread_stack_tr_snd_thread" >"$t/comb-sram/NM"
printf '%s\n' "00001234 00000040 T tr_pslot_write" "000031c0 00000200 T tr_audio_render" "00003200 00000040 T tr_bus2_he_frame" \
	"2002b8b4 00000200 b s_mono" "2002b4b4 00000400 b s_stereo" "200ff000 00010000 B kheap__system_heap" \
	"2002e360 00002000 B _k_thread_stack_tr_snd_thread" >"$t/comb-heap-over/NM"
printf '%s\n' "00001234 00000040 T tr_pslot_write" "000031c0 00000200 T tr_audio_render" "00003200 00000040 T tr_bus2_he_frame" \
	"2002b8b4 00000200 b s_mono" "2002b4b4 00000400 b s_stereo" "200350a0 00010000 B kheap__system_heap" >"$t/comb-nostack/NM"
mk "$t/plain" "TR_HP_SOUND:BOOL=OFF" "-DTR_HP_SOUND=0" "vision" none
mk "$t/hidden" "TR_HP_SOUND:BOOL=OFF" "$EMB" "$SND_ELF" none # cache edited after the build: the sound text is in the ELF
# the standalone sound/ GAME image
mkdir -p "$t/snd/zephyr"
printf '%s\n' "TR_SND_REWORKED_U46:BOOL=ON" "CMAKE_PROJECT_NAME:STATIC=trace_runner_sound" >"$t/snd/CMakeCache.txt"
echo "CONFIG_SOC_AE822FA0E5597LS0_RTSS_HP=y" >"$t/snd/zephyr/.config"
echo "FLAGS = -DTR_SND_TEST=0" >"$t/snd/build.ninja"
head -c 4096 /dev/zero >"$t/snd/zephyr/zephyr.bin"
: >"$t/snd/zephyr/zephyr.elf"
head -c 2429520 /dev/zero >"$t/model.bin"

# An HE build dir that passes every HE check: only the HP interlock may stop build-release.sh.
mkdir -p "$t/st/build/images" "$t/st/build/config" "$t/he/zephyr" "$t/he-nobus2/zephyr"
printf '#!/bin/sh\ntouch "%s/PACKAGED"\n' "$t" >"$t/st/app-gen-toc" && chmod +x "$t/st/app-gen-toc"
: >"$t/st/build/images/bl32.bin"
: >"$t/st/build/images/m55_stub_hp.bin"
: >"$t/st/build/config/app-device-config.json"
cat >"$t/he-dts" <<'DTS'
/ {
	soc {
		cdc200: cdc200@49031000 {
			width = < 0x2d0 >;
			height = < 0x500 >;
			hsync-len = < 0x6 >;
			hfront-porch = < 0xc >;
			hback-porch = < 0x18 >;
			vsync-len = < 0x2 >;
			vfront-porch = < 0x1c6 >;
			vback-porch = < 0xe >;
			clock-frequency = < 0x2625a00 >;
		};
	};
};
DTS
for d in he he-nobus2; do
	head -c 4096 /dev/zero >"$t/$d/zephyr/zephyr.bin"
	: >"$t/$d/zephyr/zephyr.elf"
	cp "$t/he-dts" "$t/$d/zephyr/zephyr.dts"
	# the camera the HE was built for: the release check compares it with the HP image's
	printf '%s\n' "TR_INPUT_NPU:BOOL=ON" "TR_CAM_ROTATE:STRING=90" "TR_CAM_MIRROR:BOOL=ON" >"$t/$d/CMakeCache.txt"
done
mkdir -p "$t/he-land/zephyr" "$t/he-nonpu/zephyr"
for d in he-land he-nonpu; do
	cp "$t/he/zephyr/zephyr.bin" "$t/he/zephyr/zephyr.elf" "$t/he/zephyr/zephyr.dts" "$t/$d/zephyr/"
done
printf '%s\n' "TR_INPUT_NPU:BOOL=ON" "TR_CAM_ROTATE:STRING=0" "TR_CAM_MIRROR:BOOL=ON" >"$t/he-land/CMakeCache.txt" # landscape HE, portrait HP
printf '%s\n' "TR_INPUT_NPU:BOOL=OFF" "TR_CAM_ROTATE:STRING=90" "TR_CAM_MIRROR:BOOL=ON" >"$t/he-nonpu/CMakeCache.txt"
printf '%s\n' "00003200 00000040 T tr_a32_boot" >"$t/he-nobus2/NM" # an HE without tr_bus2_he_frame
mkdir -p "$t/bin" && printf '#!/bin/sh\ntouch "%s/MADE"\n' "$t" >"$t/bin/make" && chmod +x "$t/bin/make"

rel() { # want(0|3) why [VAR=value ...]  -- the whole build-release.sh, up to the first thing it builds
	local want=$1 why=$2 out rc
	shift 2
	rm -f "$t/MADE" "$t/PACKAGED"
	out=$(
		unset TR_SND_HP TR_SND_HP_BUILD TR_SND_CARRIER_SERIAL TR_HP_VISION TR_HP_VISION_BUILD TR_HP_VISION_MODEL
		for kv in "$@"; do export "${kv?}"; done
		PATH="$t/bin:$PATH" NM="$t/nm" bash a32/release/build-release.sh "$t/st" "${HE_DIR:-$t/he}" 2>&1
	)
	rc=$?
	LAST_OUT=$out
	if [ "$want" = 0 ]; then
		# past every interlock: it reached `make` (and may die later on the fake HE's autolaunch id)
		if [ ! -e "$t/MADE" ] || grep -q REFUSED <<<"$out"; then
			FAILS "build-release.sh: $why was refused or never got to the build (rc=$rc): $out"
		fi
	else
		if [ "$rc" -ne "$want" ] || ! grep -q "REFUSED" <<<"$out" || [ -e "$t/MADE" ] || [ -e "$t/PACKAGED" ]; then
			FAILS "build-release.sh: $why was not refused up front with exit $want (rc=$rc): $out"
		fi
	fi
}
V="TR_HP_VISION=ON TR_HP_VISION_MODEL=$t/model.bin"
S="TR_SND_HP=ON"
# shellcheck disable=SC2086
{
	rel 0 "combined + 2026W36-0002 (reworked U46)" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'TR_SND_HP=ON (combined image) allowed for unit 2026W36-0002' <<<"$LAST_OUT" ||
		FAILS "combined: snd_hp_check (combined) did not run: $LAST_OUT"
	grep -q 'TR_HP_VISION=ON allowed' <<<"$LAST_OUT" || FAILS "combined: hp_vision_check did not run: $LAST_OUT"
	rel 3 "combined + 2026W36-0009 (stock U46)" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0009
	grep -q "is 'deny'" <<<"$LAST_OUT" || FAILS "combined + 0009: not refused by the carrier list: $LAST_OUT"
	rel 3 "combined, no carrier serial" $V $S TR_HP_VISION_BUILD="$t/comb"
	rel 3 "combined, unknown unit" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0042
	rel 3 "combined without TR_SND_REWORKED_U46=ON" $V $S TR_HP_VISION_BUILD="$t/comb-unrew" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'TR_SND_REWORKED_U46=ON' <<<"$LAST_OUT" || FAILS "combined unreworked: wrong refusal: $LAST_OUT"
	rel 3 "combined, not the embedded GAME build" $V $S TR_HP_VISION_BUILD="$t/comb-noembed" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined, a TEST build" $V $S TR_HP_VISION_BUILD="$t/comb-test" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined, uart5 left enabled (the audit stays on)" $V $S TR_HP_VISION_BUILD="$t/comb-uart5" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 0 "combined with the DEV underrun control explicitly OFF" $V $S TR_HP_VISION_BUILD="$t/comb-underrun-off" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined with the DEV underrun positive control (cache)" $V $S TR_HP_VISION_BUILD="$t/comb-underrun" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'underrun positive control' <<<"$LAST_OUT" || FAILS "underrun control (cache): wrong refusal: $LAST_OUT"
	rel 3 "combined with the DEV underrun control's text in the ELF" $V $S TR_HP_VISION_BUILD="$t/comb-underrun-elf" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'underrun positive control' <<<"$LAST_OUT" || FAILS "underrun control (ELF): wrong refusal: $LAST_OUT"
	rel 3 "combined with an HE built for a landscape camera (HP is portrait)" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002 HE_DIR="$t/he-land"
	grep -q 'landscape (0) and portrait' <<<"$LAST_OUT" || FAILS "HE rotation mismatch: wrong refusal: $LAST_OUT"
	rel 3 "combined with an HE built without TR_INPUT_NPU" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002 HE_DIR="$t/he-nonpu"
	grep -q 'TR_INPUT_NPU' <<<"$LAST_OUT" || FAILS "HE without TR_INPUT_NPU: wrong refusal: $LAST_OUT"
	rel 3 "combined, the sound buffers in SRAM0" $V $S TR_HP_VISION_BUILD="$t/comb-sram" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined, the heap runs past the DTCM" $V $S TR_HP_VISION_BUILD="$t/comb-heap-over" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined, no sound thread stack in the ELF" $V $S TR_HP_VISION_BUILD="$t/comb-nostack" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "TR_SND_HP=ON + TR_HP_VISION=ON but the hp_vision image has no sound" $V $S TR_HP_VISION_BUILD="$t/plain" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'needs the combined image' <<<"$LAST_OUT" || FAILS "no-sound image as combined: wrong refusal: $LAST_OUT"
	rel 3 "two separate HP images (standalone sound + hp_vision)" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_HP_BUILD="$t/snd" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'two separate HP images' <<<"$LAST_OUT" || FAILS "two images: wrong refusal: $LAST_OUT"
	rel 0 "TR_SND_HP_BUILD naming the same combined dir" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_HP_BUILD="$t/comb/." TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "combined with an HE that has no lease glue" $V $S TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002 HE_DIR="$t/he-nobus2"
	grep -q 'tr_bus2_he_frame' <<<"$LAST_OUT" || FAILS "HE without the lease: wrong refusal: $LAST_OUT"
	rel 0 "a plain hp_vision, TR_HP_VISION alone: no sound check at all" $V TR_HP_VISION_BUILD="$t/plain"
	! grep -q 'TR_SND_HP=ON' <<<"$LAST_OUT" || FAILS "plain hp_vision went through the sound check: $LAST_OUT"
	rel 3 "combined image with TR_HP_VISION alone (the sound would skip the carrier list)" $V TR_HP_VISION_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'carries the game sound' <<<"$LAST_OUT" || FAILS "combined alone: wrong refusal: $LAST_OUT"
	rel 3 "sound text in the ELF though the cache says TR_HP_SOUND=OFF" $V TR_HP_VISION_BUILD="$t/hidden"
	rel 3 "the combined hp_vision dir as TR_SND_HP_BUILD alone" $S TR_SND_HP_BUILD="$t/comb" TR_SND_CARRIER_SERIAL=2026W36-0002
	grep -q 'combined hp_vision image' <<<"$LAST_OUT" || FAILS "combined dir as the standalone sound: wrong refusal: $LAST_OUT"
	rel 0 "the standalone sound/ GAME image alone (unchanged path)" $S TR_SND_HP_BUILD="$t/snd" TR_SND_CARRIER_SERIAL=2026W36-0002
	rel 3 "the standalone sound/ image on 2026W36-0009" $S TR_SND_HP_BUILD="$t/snd" TR_SND_CARRIER_SERIAL=2026W36-0009
	rel 0 "nothing: the parked stub"
}
# a bad ON/OFF value is a usage error (die, exit 1), not an interlock
out=$(
	unset TR_HP_VISION_BUILD
	PATH="$t/bin:$PATH" NM="$t/nm" TR_HP_VISION=yes bash a32/release/build-release.sh "$t/st" "$t/he" 2>&1
)
[ $? -eq 1 ] || FAILS "TR_HP_VISION=yes was not a usage error: $out"
out=$(PATH="$t/bin:$PATH" NM="$t/nm" TR_SND_HP=yes bash a32/release/build-release.sh "$t/st" "$t/he" 2>&1)
[ $? -eq 1 ] || FAILS "TR_SND_HP=yes was not a usage error: $out"

# ---- C. source interlocks ------------------------------------------------------------------
n_sel=$(grep -c 'alp_gpio_write(s\.mux_sel' sound/src/main.c)
n_sel0=$(grep -c 'alp_gpio_write(s\.mux_sel, false)' sound/src/main.c)
if [ "$n_sel" -lt 1 ] || [ "$n_sel" != "$n_sel0" ] || grep -q 'mux_sel, *\(true\|1\)' sound/src/main.c; then
	FAILS "I2S_SELECT (s.mux_sel, E1M IO13) is written something other than 0 in sound/src/main.c ($n_sel writes, $n_sel0 of them false)"
fi
# the embedded sound thread is cooperative: a failed write must be followed by a sleep, never a spin
awk '/^\t\t\tfails\+\+;$/ { f = 1 } f && /^\t\t\tk_msleep\(BLOCK \* 1000u \/ RATE\);$/ { ok = 1 } f && /\} else \{/ { f = 0 }
	END { exit !ok }' sound/src/main.c || FAILS "sound/src/main.c: the embedded stream's failed-write path does not sleep (a cooperative thread would starve the vision)"
# the lease (src/ipc/tr_bus2.h) as the firmware wires it. Every step that touches I2C2 / GPIO5 is
# entered with the Dekker entry (SND_BUS / tr_snd_bus_enter): step 4, 5, 5b, 6, 7, 11 in main.c, the
# proxy attach (EEPROM @0x50) in the bridge, the I2C2 device_init in lease_acquire(); the I2S3
# steps leave the bus phase first
n_bus=$(grep -cE '^[[:space:]]+SND_BUS\(\);' sound/src/main.c)
[ "$n_bus" = 6 ] || FAILS "sound/src/main.c: $n_bus SND_BUS() entries, want 6 (step 4, 5, 5b, 6, 7, 11)"
awk '/^\tSND_RELEASE\(\); \/\* steps 8-10/ { nb = NR } /s.spk = alp_audio_out_open\(/ { if (nb && NR == nb + 1) ok = 1 } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: the I2S3 steps do not return the lease (SND_RELEASE before step 9)"
# the lease is returned across step 1 too: the bridge releases it right after the EEPROM read, before the reset
awk '/alp_gpio_cc3501e_attach\(fw\)/ { a = NR } /tr_snd_bus_release\(\);/ { if (a && NR - a <= 3) r = NR } /return cc3501e_reset\(fw\);/ { if (r && r < NR) ok = 1 } END { exit !ok }' sound/src/cc3501e_bridge.c ||
	FAILS "sound/src/cc3501e_bridge.c: the lease is not returned between the EEPROM read and the CC3501E reset"
# step 11: the lease is waited for with the bit clock fed, and returned at the end of the bring-up
awk '/SND_IDLE_SET\(keepalive_block\);/ { k = NR } /rc = tas2563_resume\(/ { if (k && !r) r = NR } /^\tSND_RELEASE\(\);$/ { if (r && NR > r) ok = 1 } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: step 11 does not feed the clock while it waits for the lease, or does not return it"
awk '/if \(!tr_snd_bus_enter\(\)\) \{/ { e = NR } /alp_gpio_cc3501e_attach\(fw\)/ { if (e && NR - e <= 4) ok = 1 } END { exit !ok }' sound/src/cc3501e_bridge.c ||
	FAILS "sound/src/cc3501e_bridge.c: the proxy attach (identity-EEPROM read on I2C2) is not a bus step"
awk '/^static void lease_acquire\(void\)$/ { f = 1 } f && /tr_bus2_hp_enter\(&s_lease/ { e = NR } f && /tr_i2c2_quiesce\(i2c2/ { q = NR } f && /device_init\(i2c2\)/ { if (e && q && e < q && q < NR) ok = 1; f = 0 } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: the HP's I2C2 is not quiesced and device_init()ed inside an entered bus step (Dekker entry)"
# this core's I2C2 IRQ: armed ONLY by an entry that passed (one irq_enable, in snd_irq_on; its callers)
[ "$(grep -c 'irq_enable(TR_I2C2_IRQN)' sound/src/main.c)" = 1 ] ||
	FAILS "sound/src/main.c: the I2C2 IRQ is armed somewhere other than snd_irq_on()"
awk '/^bool tr_snd_bus_enter\(void\)$/ { f = 1 } f && /^}$/ { f = 0 } f && /snd_irq_on\(\);/ { n++ } f && /tr_bus2_hp_enter\(&s_lease/ { e = 1 } END { exit !(n == 2 && e) }' sound/src/main.c ||
	FAILS "sound/src/main.c: tr_snd_bus_enter() does not arm the IRQ after the entry / the acquire"
[ "$(grep -c 'snd_irq_on();' sound/src/main.c)" = 2 ] ||
	FAILS "sound/src/main.c: snd_irq_on() is called outside tr_snd_bus_enter()"
awk '/^void tr_snd_bus_leave\(void\)$/ { f = 1 } f && /snd_irq_off\(\);/ { i = NR } f && /tr_bus2_hp_leave\(/ { if (i && i < NR) ok = 1; f = 0 } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: tr_snd_bus_leave() does not turn the I2C2 IRQ off before HELD"
# the HP quiesces the controller the same way the HE does (see tr_i2c2_quiesce below)
grep -q 'SCB->CCR & SCB_CCR_DC_Msk' sound/src/main.c || FAILS "sound/src/main.c: the HP claims a lease with the D-cache on"
grep -q 'SCB->CCR & SCB_CCR_DC_Msk' src/platform/bus2_he.c || FAILS "src/platform/bus2_he.c: the HE offers a lease with the D-cache on"
awk '/^bool tr_bus2_hp_enter\(/ { f = 1 } f && /hp_set\(r, TR_BUS2_HP_BUS, hp->token\);/ { w = NR } f && /barrier\(\);/ { if (w) b = NR } f && /r->he_state;/ { if (w && b && w < b && b < NR) ok = 1; f = 0 } END { exit !ok }' src/ipc/tr_bus2.c ||
	FAILS "src/ipc/tr_bus2.c: the HP entry is not write-state, fence, read-offer"
awk '/^bool tr_bus2_he_boot\(/ { f = 1 } f && /r->he_state = tr_bus2_word\(TR_BUS2_HE_OWNS, 0u\);/ { w = NR } f && /o->barrier\(\);/ { if (w) b = NR } f && /hp_st\(r\) != TR_BUS2_HP_BUS/ { if (w && b && w < b && b < NR) ok = 1; f = 0 } END { exit !ok }' src/ipc/tr_bus2.c ||
	FAILS "src/ipc/tr_bus2.c: the HE boot is not void, fence, read-HP-phase"
# the HE stops its I2C2 BEFORE it publishes the offer; the HP turns its line off BEFORE it returns it
awk '/^void tr_bus2_he_tick\(/ { f = 1 } f && /o->give\(o->ctx\);/ { g = NR } f && /r->he_state = tr_bus2_word\(TR_BUS2_HE_OFFER, tok\);/ { if (g && !p) { p = NR; ok = (g < p) } } END { exit !ok }' src/ipc/tr_bus2.c ||
	FAILS "src/ipc/tr_bus2.c: the HE publishes the offer before it stopped its I2C2"
awk '/^void tr_snd_bus_release\(void\)$/ { f = 1 } f && /tr_snd_bus_leave\(\);/ { i = NR } f && /tr_bus2_hp_return\(/ { if (i) ok = 1; f = 0 } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: the HP returns the bus before it turned its I2C2 IRQ off"
awk '/^static void b2_give\(void \*ctx\)$/ { f = 1 } f && /irq_disable\(/ { i = NR } f && /tr_i2c2_stop\(\);/ { if (i && NR > i) ok = 1; f = 0 } END { exit !ok }' src/platform/bus2_he.c ||
	FAILS "src/platform/bus2_he.c: the HE does not mask its I2C2 line before it stops the controller"
# re-arming a controller the other core used: IC_ENABLE = 0 + wait, INTR_MASK = 0, clear the pending
# IRQ, reset the driver's semaphore, bus-recover -- in this order -- and only THEN configure + enable
awk '/^static inline bool tr_i2c2_quiesce\(/ { f = 1 } f && /tr_i2c2_stop\(\);/ { a = NR } f && /TR_I2C2_IC_INTR_MASK\);/ { b = NR } f && /NVIC_ClearPendingIRQ/ { c = NR } f && /k_sem_reset/ { d = NR } f && /tr_i2c2_bus_clear/ { e = NR; f = 0 } END { exit !(a && b && c && d && e && a < b && b < c && c < d && d < e) }' src/platform/tr_i2c2_rearm.h ||
	FAILS "src/platform/tr_i2c2_rearm.h: tr_i2c2_quiesce() is not stop, INTR_MASK=0, clear pending, sem reset, bus-recover"
awk '/^static void b2_take\(void \*ctx\)$/ { f = 1 } f && /tr_i2c2_quiesce\(/ { q = NR } f && /i2c_configure\(/ { c = NR } f && /irq_enable\(/ { if (q && c && q < c && c < NR) ok = 1; f = 0 } END { exit !ok }' src/platform/bus2_he.c ||
	FAILS "src/platform/bus2_he.c: the HE re-arms I2C2 without quiescing it first"
awk '/^static void b2_reclaim\(void \*ctx\)$/ { f = 1 } f && /irq_disable\(/ { i = NR } f && /tr_i2c2_stop\(\);/ { s = NR } f && /tr_i2c2_bus_clear\(true\)/ { if (i && s && i < s && s < NR) ok = 1; f = 0 } END { exit !ok }' src/platform/bus2_he.c ||
	FAILS "src/platform/bus2_he.c: the reclaim is not IRQ off, IC_ENABLE = 0, SCL bus-clear"
grep -q '^SYS_INIT(snd_bus2_boot, PRE_KERNEL_1, 0);$' sound/src/main.c || FAILS "sound/src/main.c: the HP does not forget the lease record at PRE_KERNEL_1"
grep -q '^SYS_INIT(bus2_he_boot, POST_KERNEL, 0);$' src/platform/bus2_he.c || FAILS "src/platform/bus2_he.c: the HE does not claim at POST_KERNEL 0 (before the i2c_dw instance)"
# the HE's only I2C2 users skip their transfer while the HP holds the bus
grep -q 'tr_bus2_he_owns()' src/platform/rail5v_power.c || FAILS "src/platform/rail5v_power.c: the +5V poll does not check the lease"
grep -q 'tr_bus2_he_owns()' src/platform/imu.c || FAILS "src/platform/imu.c: the IMU read does not check the lease"
awk '/tr_bus2_he_frame\(\);/ { f = NR } /tr_rail5v_poll\(\);/ { if (f && NR == f + 1) ok = 1 } END { exit !ok }' src/main.c ||
	FAILS "src/main.c: the HE's main loop does not tick the lease right before the +5V poll"
# The HE offers I2C2 only AFTER its own I2C2 users (BMI323, INA236) are open: ui_present() ticks the lease
# (so does the first flip, before the opens), therefore the tick is inert until tr_bus2_he_arm(), which
# main() calls after tr_imu_open() and tr_rail5v_open().
awk '/^int main\(void\)/ { m = 1 } m && /tr_imu_open\(\)/ { i = NR } m && /tr_rail5v_open\(\)/ { r = NR } m && /tr_bus2_he_arm\(\);/ { a = NR }
	END { exit !(i && r && a && a > i && a > r) }' src/main.c ||
	FAILS "src/main.c: tr_bus2_he_arm() is not called after tr_imu_open() and tr_rail5v_open()"
[ "$(grep -c 'tr_bus2_he_arm();' src/main.c)" = 1 ] || FAILS "src/main.c: tr_bus2_he_arm() must be called exactly once"
awk '/^void tr_bus2_he_frame\(void\)$/ { f = 1 } f && /if \(!g_armed\)/ { g = NR } f && /tr_bus2_he_tick\(/ { if (g && g < NR) ok = 1; f = 0 } END { exit !ok }' src/platform/bus2_he.c ||
	FAILS "src/platform/bus2_he.c: tr_bus2_he_frame() can offer the bus before tr_bus2_he_arm()"
[ "$(grep -c 'g_armed = true;' src/platform/bus2_he.c)" = 1 ] || FAILS "src/platform/bus2_he.c: g_armed is set somewhere other than tr_bus2_he_arm()"
# flash-release.sh write: a package that carries the game sound cold-boots -- no warm reset at the end
grep -q -- '--no-reset) noreset=1' a32/release/flash-release.sh && grep -q 'FLASH_NO_RESET_DEFAULT=1' a32/release/flash-release.sh &&
	grep -q "bootreset=''" a32/release/flash-release.sh || FAILS "a32/release/flash-release.sh: no --no-reset / sound-package default"
bash -n a32/release/flash-release.sh || FAILS "a32/release/flash-release.sh: syntax"
# the SCL bus-clear sets DR / DDR before the pads leave the I2C function (no driven glitch)
awk '/^static inline bool tr_i2c2_bus_clear/ { f = 1 } f && /tr_gpio5_ddr\(0u, TR_I2C2_SCL \| TR_I2C2_SDA\);/ { d = NR } f && /pinctrl_configure_pins\(tr_i2c2_gpio_pads/ { if (d && d < NR) ok = 1; f = 0 } END { exit !ok }' src/platform/tr_i2c2_rearm.h ||
	FAILS "src/platform/tr_i2c2_rearm.h: the bus-clear switches the pads to GPIO before clearing DDR bits 6/7"
# the amp settle + ACK poll: the settle after SD_N is TR_SND_AMP_SETTLE_US, every amp is polled for its ACK
# before tas2563_init, and an amp that never ACKs fails the bring-up
grep -q '^	k_usleep(TR_SND_AMP_SETTLE_US);$' sound/src/main.c && ! grep -q 'k_usleep(TAS2563_RESET_SETTLE_US)' sound/src/main.c ||
	FAILS "sound/src/main.c: the settle after SD_N is not TR_SND_AMP_SETTLE_US"
awk '/tr_amp_wait_ack\(&amp_io,/ { p = NR } /rc = tas2563_init\(/ { if (!i) i = NR } END { exit !(p && i && p < i) }' sound/src/main.c ||
	FAILS "sound/src/main.c: the amps are not polled for their ACK before tas2563_init"
awk 'p && /^\t\t\treturn 6;$/ { ok = 1 } { p = /\(unsigned\)tries\);$/ } END { exit !ok }' sound/src/main.c ||
	FAILS "sound/src/main.c: an amp that never ACKs does not fail the bring-up"
grep -q 'tr_amp_ready.c' sound/CMakeLists.txt || FAILS "sound/CMakeLists.txt: the standalone image lacks the amp settle (tr_amp_ready.c)"
# the i2s3 overlay and the sound Kconfig only inside `if(TR_HP_SOUND)`, which the gate ties to REWORKED
awk '/^if\(TR_HP_SOUND\)$/ { d = 1; next } d && /^(endif|else)\(\)$/ { d = 0 } /sound\.overlay|sound\.conf|sound_hp\.overlay/ && !d { bad = 1 }
	END { exit bad }' hp_vision/CMakeLists.txt || FAILS "hp_vision/CMakeLists.txt applies a sound overlay/conf outside if(TR_HP_SOUND)"
grep -q '^include(${CMAKE_CURRENT_LIST_DIR}/sound_gate.cmake)$' hp_vision/CMakeLists.txt || FAILS "hp_vision/CMakeLists.txt does not include the sound gate"
grep -q 'zephyr,deferred-init;' hp_vision/sound_hp.overlay || FAILS "the HP's I2C2 is not deferred (hp_vision/sound_hp.overlay)"
# the HE: TR_HP_SOUND needs TR_INPUT_NPU, and a build that uses GPIO5 is refused at compile time
grep -q 'TR_HP_SOUND AND NOT TR_INPUT_NPU' CMakeLists.txt || FAILS "CMakeLists.txt: TR_HP_SOUND without TR_INPUT_NPU is not refused"
grep -q 'DT_NODE_HAS_STATUS(DT_NODELABEL(gpio5), okay)' src/platform/bus2_he.c || FAILS "src/platform/bus2_he.c: no compile-time refusal of an HE that uses GPIO5"
# the I2S3 FIFO deadline: I2S3 above the camera, CSI and U55 in the combined image's overlay
prio() { awk -v n="&$1 {" '$0 == n { f = 1 } f && /interrupts = </ { gsub(/[<>;]/, ""); print $4; exit }' hp_vision/sound_hp.overlay; }
p_i2s=$(prio i2s3)
for l in ethosu55 cam csi; do
	p_o=$(prio "$l")
	{ [ -n "$p_i2s" ] && [ -n "$p_o" ] && [ "$p_i2s" -lt "$p_o" ]; } || FAILS "hp_vision/sound_hp.overlay: I2S3 priority '$p_i2s' not above $l '$p_o'"
done
# the lease record collides with nothing: it is NOT the I2C1 handover words
a_b2=$(sed -n '/^#define TR_MEM_BUS2 /{n;s/^[[:space:]]*\(0x[0-9A-Fa-f]*\)u.*/\1/p}' src/ipc/tr_memmap.h | head -1)
a_i2c1=$(sed -n '/^#define TR_MEM_I2C1_HANDOVER /{n;s/^[[:space:]]*\(0x[0-9A-Fa-f]*\)u.*/\1/p}' src/ipc/tr_memmap.h | head -1)
{ [ -n "$a_b2" ] && [ -n "$a_i2c1" ] && [ "$a_b2" != "$a_i2c1" ]; } || FAILS "TR_MEM_BUS2 ($a_b2) is the I2C1 handover address ($a_i2c1)"

# ---- real builds, when given (the gw build dirs) ---------------------------------------------
NM=${NM:-$(command -v arm-zephyr-eabi-nm || true)}
if [ -n "${TR_HP_COMBINED_BUILD:-}" ] && [ -x "${NM:-}" ] && [ -f "$TR_HP_COMBINED_BUILD/zephyr/zephyr.elf" ]; then
	syms=$("$NM" "$TR_HP_COMBINED_BUILD/zephyr/zephyr.elf")
	for s in tr_audio_render tas2563_init tas2563_resume cc3501e_bridge_bringup i2s_dw_initialize tr_bus2_hp_enter \
		tr_bus2_hp_poll tr_snd_bus_enter abort_bringup snd_bus2_boot tr_pslot_write; do
		grep -qE " $s\$" <<<"$syms" || FAILS "$TR_HP_COMBINED_BUILD (combined) lacks $s"
	done
fi
if [ -n "${TR_HP_PLAIN_BUILD:-}" ] && [ -x "${NM:-}" ] && [ -f "$TR_HP_PLAIN_BUILD/zephyr/zephyr.elf" ]; then
	if grep -qE ' (tr_audio_render|tas2563_init|cc3501e_bridge_bringup|i2s_dw_initialize|tr_bus2_hp_enter)$' <<<"$("$NM" "$TR_HP_PLAIN_BUILD/zephyr/zephyr.elf")"; then
		FAILS "$TR_HP_PLAIN_BUILD (TR_HP_SOUND=OFF) links sound code"
	fi
fi

[ $fail = 0 ] && echo "test_hp_combined: ok"
exit $fail
