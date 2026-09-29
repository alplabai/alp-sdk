#!/bin/bash
# tests/host/test_hp_vision_check.sh -- the TR_HP_VISION=ON interlock of
# a32/release/build-release.sh (hp_vision_check.sh): every refusal path
# refuses, the one allowed configuration passes, and the full script refuses
# BEFORE it builds or packages anything -- same shape as
# test_snd_hp_check.sh, and the TR_SND_HP + TR_HP_VISION-together refusal
# that lives in build-release.sh itself, not either check function.
set -u
cd "$(dirname "$0")/../.." || exit 1
t=$(mktemp -d "${TMPDIR:-/tmp}/tr-hp-vision-check.XXXXXX")
trap 'rm -rf "$t"' EXIT
fail=0
# shellcheck source=a32/release/hp_vision_check.sh
source a32/release/hp_vision_check.sh

# Thousands of lines, match near the TOP: a real ELF's nm output is this
# shape, and hp_vision_check.sh's caller (build-release.sh) runs under
# `set -o pipefail` -- a naive `$nm | grep -q` SIGPIPEs $nm on an early
# match and pipefail then reports failure even though grep matched (see
# hp_vision_check.sh's own comment). A one-line fake would never reproduce
# that; this shape does.
{ echo "00001234 T tr_pslot_write"; for i in $(seq 1 5000); do echo "0000$i T pad_sym_$i"; done; } > "$t/nm-body"
printf '#!/bin/sh\ncat "%s/nm-body"\n' "$t" > "$t/nm" && chmod +x "$t/nm"
{ for i in $(seq 1 5000); do echo "0000$i T pad_sym_$i"; done; echo "00001234 T some_other_symbol"; } > "$t/nm-no-pslot-body"
printf '#!/bin/sh\ncat "%s/nm-no-pslot-body"\n' "$t" > "$t/nm-no-pslot" && chmod +x "$t/nm-no-pslot"

# A minimal slice of the shape zephyr.dts actually has (nested child device
# node, comment-trailer on the status line, tab indent) -- the ownership
# audit (hp_vision_check.sh) must see past the arx3a0 CHILD node's own
# "status" property to the i2c@ node's own, not the last one textually seen
# (fix round 6 caught exactly this: a naive scan matched the child's
# "disabled" instead of the parent's "okay" and missed the real i2c2 leak).
dts_i2c_node() { # instance status
	cat <<DTS
		i2cX: i2c@$1 {
			compatible = "snps,designware-i2c";
			status = "$2";
			arx3a0: arx3a0@36 {
				compatible = "onnn,arx3a0";
				status = "disabled";
			};
		};
DTS
}
# fix round 12 (review: the ownership audit widened past i2c-only): a plain,
# unlabelled node -- the audit must catch these too, not just a `foo:`-
# labelled one (the original i2c-only regex required a label prefix).
dts_plain_node() { # nodename addr status(okay|disabled|NONE -- NONE omits the property entirely)
	cat <<DTS
		$1@$2 {
			$( [ "$3" != "NONE" ] && echo "status = \"$3\";" )
		};
DTS
}
# fix round 13 (review, second pass): the real zephyr.dts labels some nodes
# with TWO stacked labels (`i2c1: csi_i2c: i2c@49011000 {`) -- the scan's
# label regex only ever matched one, so a multi-labelled node fell through
# unaddressed and unchecked no matter its status. This fixture is a BAD
# (not-in-ALLOW) node with two labels, to prove the scan still catches it.
dts_multilabel_node() { # nodename addr status
	cat <<DTS
		labelA: labelB: $1@$2 {
			status = "$3";
		};
DTS
}
mk_build() { # dir board_line extra_i2c_status(okay|disabled) uart5_status lpgpio_status counter(yes|) omitted_status_bad_addr multilabel_bad_addr
	mkdir -p "$1/zephyr"
	# ROT (env): the TR_CAM_ROTATE cache value, 90 when unset; ROT= writes it empty.
	printf '%s\nTR_CAM_ROTATE:STRING=%s\nTR_CAM_MIRROR:BOOL=ON\n' "$2" "${ROT-90}" > "$1/CMakeCache.txt"
	head -c 4096 /dev/zero > "$1/zephyr/zephyr.bin"
	: > "$1/zephyr/zephyr.elf"
	if [ "${6:-}" = yes ]; then
		echo "CONFIG_COUNTER=y" > "$1/zephyr/.config"
	else
		echo "# CONFIG_COUNTER is not set" > "$1/zephyr/.config"
	fi
	{
		echo "/ {"
		echo "	soc {"
		dts_i2c_node 49011000 okay # i2c1, the camera bus this app owns
		[ -n "${3:-}" ] && dts_i2c_node 49012000 "$3" # i2c2
		[ -n "${4:-}" ] && dts_plain_node uart 4901d000 "$4" # uart5, unlabelled
		[ -n "${5:-}" ] && dts_plain_node gpio 42002000 "$5" # lpgpio, unlabelled
		# fix round 13: the non-peripheral structural nodes every board
		# carries, none with an explicit status line (Zephyr's own real
		# generated dts shape) -- must stay allowed, proving the "no
		# status property = okay by devicetree spec" fix doesn't itself
		# start refusing every board's own memory/clock/pin/NVIC/lptimer
		# nodes now that they are actually looked at.
		dts_plain_node memory 1a000000 NONE
		dts_plain_node clock-controller 1a602000 NONE
		dts_plain_node lptimer 42001000 NONE
		[ -n "${7:-}" ] && dts_plain_node unowned "$7" NONE # bad: omitted status still defaults to okay
		[ -n "${8:-}" ] && dts_multilabel_node unowned "$8" okay # bad: two stacked labels must still be scanned
		echo "	};"
		echo "};"
	} > "$1/zephyr/zephyr.dts"
}
mk_build "$t/good" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled
mk_build "$t/bad-i2c2" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' okay disabled disabled
# fix round 12: the two peripherals a real silicon boot found left enabled
# and contending with the HE (uart5's console/shell, lpgpio unused).
mk_build "$t/bad-uart5" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled okay disabled
mk_build "$t/bad-lpgpio" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled okay
mk_build "$t/bad-counter" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled yes
mk_build "$t/bad-omitted-status" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled '' 42003000
mk_build "$t/bad-multilabel" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled '' '' 42004000
mk_build "$t/he" 'BOARD:STRING=alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he'
# The camera rotation: only 0/90/270, and never the empty "header default".
ROT=180 mk_build "$t/bad-rot180" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled
ROT= mk_build "$t/bad-rot-empty" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled
ROT=270 mk_build "$t/rot270" 'BOARD:STRING=alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp' disabled disabled disabled

# A model file of exactly the right size, and a wrong-size one.
head -c 2429520 /dev/zero > "$t/model.bin"
head -c 100 /dev/zero > "$t/model-wrong.bin"

expect() { # want(0|1) why build model nm
	local want=$1 why=$2 out rc
	out=$(hp_vision_check "$3" "$4" "$5" 2>&1)
	rc=$?
	if { [ "$want" = 0 ] && [ $rc -ne 0 ]; } || { [ "$want" = 1 ] && { [ $rc -eq 0 ] || ! grep -q REFUSED <<<"$out"; }; }; then
		echo "FAIL hp_vision_check: $why (rc=$rc): $out"
		fail=1
	fi
}
expect 1 "no build dir" "$t/missing" "$t/model.bin" "$t/nm"
expect 1 "an HE build" "$t/he" "$t/model.bin" "$t/nm"
expect 1 "elf lacks tr_pslot_write" "$t/good" "$t/model.bin" "$t/nm-no-pslot"
expect 1 "no model file" "$t/good" "$t/missing-model.bin" "$t/nm"
expect 1 "wrong-size model" "$t/good" "$t/model-wrong.bin" "$t/nm"
expect 0 "HP vision build + right-size model" "$t/good" "$t/model.bin" "$t/nm"
expect 1 "TR_CAM_ROTATE=180" "$t/bad-rot180" "$t/model.bin" "$t/nm"
expect 1 "TR_CAM_ROTATE empty (the header default at build time)" "$t/bad-rot-empty" "$t/model.bin" "$t/nm"
expect 0 "TR_CAM_ROTATE=270" "$t/rot270" "$t/model.bin" "$t/nm"
# The values it packages are printed.
if ! hp_vision_check "$t/good" "$t/model.bin" "$t/nm" 2>&1 | grep -q 'TR_CAM_ROTATE=90 TR_CAM_MIRROR=ON'; then
	echo "FAIL hp_vision_check: does not print TR_CAM_ROTATE=90 TR_CAM_MIRROR=ON"
	fail=1
fi
expect 1 "i2c2 left status=okay (shared HE bus, fix round 6)" "$t/bad-i2c2" "$t/model.bin" "$t/nm"
expect 1 "uart5 left status=okay (shared HE console/shell, fix round 12)" "$t/bad-uart5" "$t/model.bin" "$t/nm"
expect 1 "lpgpio left status=okay (unused, fix round 12)" "$t/bad-lpgpio" "$t/model.bin" "$t/nm"
expect 1 "CONFIG_COUNTER=y invalidates lptimer0's allow-list reason (fix round 13)" "$t/bad-counter" "$t/model.bin" "$t/nm"
expect 1 "peripheral with NO status property defaults to okay by devicetree spec (fix round 13)" "$t/bad-omitted-status" "$t/model.bin" "$t/nm"
expect 1 "peripheral with two stacked labels must still be scanned (fix round 13)" "$t/bad-multilabel" "$t/model.bin" "$t/nm"

# End to end: build-release.sh must stop at the interlock (exit 3) before it
# runs make or app-gen-toc, AND must refuse TR_SND_HP + TR_HP_VISION together
# (exit 1, die()) before touching either interlock.
mkdir -p "$t/st/build/images" "$t/st/build/config" "$t/he/zephyr"
printf '#!/bin/sh\ntouch "%s/PACKAGED"\n' "$t" > "$t/st/app-gen-toc" && chmod +x "$t/st/app-gen-toc"
: > "$t/st/build/images/bl32.bin"; : > "$t/st/build/images/m55_stub_hp.bin"
: > "$t/st/build/config/app-device-config.json"
head -c 4096 /dev/zero > "$t/he/zephyr/zephyr.bin"; : > "$t/he/zephyr/zephyr.elf"
# fix round 15: build-release.sh now also refuses an HE whose CMakeCache
# lacks TR_PANEL_HZ:STRING=30 (panel_hz_check.sh, tested on its own in
# tests/host/test_panel_hz_check.sh) -- append it here so these two
# end-to-end checks keep testing what THEY test (the HP interlocks), not
# get short-circuited by an unrelated refusal earlier in the script.
echo 'TR_PANEL_HZ:STRING=30' >> "$t/he/CMakeCache.txt"
mkdir -p "$t/bin" && printf '#!/bin/sh\ntouch "%s/MADE"\n' "$t" > "$t/bin/make" && chmod +x "$t/bin/make"

out=$(PATH="$t/bin:$PATH" NM="$t/nm" TR_HP_VISION=ON TR_HP_VISION_BUILD="$t/he" TR_HP_VISION_MODEL="$t/model.bin" \
	bash a32/release/build-release.sh "$t/st" "$t/he" 2>&1)
rc=$?
if [ $rc -ne 3 ] || ! grep -q "REFUSED" <<<"$out" || [ -e "$t/MADE" ] || [ -e "$t/PACKAGED" ]; then
	echo "FAIL build-release.sh did not refuse an HE build as TR_HP_VISION_BUILD (rc=$rc): $out"
	fail=1
fi

out=$(PATH="$t/bin:$PATH" NM="$t/nm" TR_SND_HP=ON TR_SND_HP_BUILD="$t/he" TR_SND_CARRIER_SERIAL=2026W36-0002 \
	TR_HP_VISION=ON TR_HP_VISION_BUILD="$t/good" TR_HP_VISION_MODEL="$t/model.bin" \
	bash a32/release/build-release.sh "$t/st" "$t/he" 2>&1)
rc=$?
if [ $rc -eq 0 ] || [ -e "$t/MADE" ] || [ -e "$t/PACKAGED" ]; then
	echo "FAIL build-release.sh did not refuse TR_SND_HP=ON + TR_HP_VISION=ON together (rc=$rc): $out"
	fail=1
fi

[ $fail = 0 ] && echo "test_hp_vision_check: ok"
exit $fail
