# shellcheck shell=bash
# a32/release/hp_vision_check.sh -- sourced by build-release.sh (and
# tests/host/test_hp_vision_check.sh). hp_vision_check HP_BUILD_DIR
# MODEL_FILE NM: 0 when the hp_vision image + Vela'd model may be packaged as
# HP_APP + the MRAM model blob, else prints a loud refusal and returns 1.
#
# Same shape as snd_hp_check.sh (the OTHER thing that can occupy HP_APP) --
# deliberately not merged with it: sound and vision are different silicon
# risks (I2S mux contention vs "does the HP app even link the pipeline"),
# and TR_SND_HP + TR_HP_VISION are refused together below (build-release.sh),
# not inside this function, since that refusal needs both env vars in view
# at once.

hp_vision_refuse() {
	{
		echo "######################################################################"
		echo "build-release: REFUSED TR_HP_VISION=ON -- $*"
		echo "######################################################################"
	} >&2
}

hp_vision_check() {
	local hd=$1 model=$2 nm=$3
	if [ ! -f "$hd/CMakeCache.txt" ]; then
		hp_vision_refuse "$hd is not a Zephyr build dir (no CMakeCache.txt)"
		return 1
	fi
	if ! grep -q '^BOARD:STRING=alp_e1m_aen803_m55_hp/' "$hd/CMakeCache.txt"; then
		hp_vision_refuse "$hd is not an M55-HP build"
		return 1
	fi
	# The camera orientation this image applies (src/vision/cam_rot.h),
	# printed so whoever packages it sees what goes on the board, and only
	# the three rotations tr_cam_rot_src() implements. An EMPTY cache entry
	# is refused too: it means "cam_rot.h's default when this was built",
	# and that default has changed (270 -> 90, the bench-verified mount).
	local rot mir
	rot=$(sed -n 's/^TR_CAM_ROTATE:[A-Z]*=//p' "$hd/CMakeCache.txt")
	mir=$(sed -n 's/^TR_CAM_MIRROR:[A-Z]*=//p' "$hd/CMakeCache.txt")
	echo "build-release: hp_vision camera: TR_CAM_ROTATE=${rot:-<unset>} TR_CAM_MIRROR=${mir:-<unset>}" >&2
	case "$rot" in
	0 | 90 | 270) ;;
	*)
		hp_vision_refuse "TR_CAM_ROTATE='$rot' in $hd/CMakeCache.txt -- must be set explicitly to 0, 90 or 270 (90: the 2026W36-0009 bench mount, see FLASH-RECIPE.md)"
		return 1
		;;
	esac
	if [ ! -f "$hd/zephyr/zephyr.bin" ] || [ "$(stat -c %s "$hd/zephyr/zephyr.bin")" -gt 262144 ]; then
		hp_vision_refuse "$hd/zephyr/zephyr.bin is missing or > 256 KiB HP ITCM"
		return 1
	fi
	# Captured, not piped straight into `grep -q`: under this script's
	# (and its caller's) `set -o pipefail`, grep -q closes its stdin the
	# instant it finds a match, SIGPIPEs $nm mid-write on any real ELF
	# (thousands of symbol lines) and pipefail then reports the WHOLE
	# pipeline as failed even though the match was real -- reproduced live
	# against hp_vision's actual zephyr.elf (a bare `$nm | grep -q` refused
	# every real build; only the tiny one-line fake $nm in
	# test_hp_vision_check.sh finishes before grep ever needs to SIGPIPE
	# it, so that test alone never caught this).
	local syms
	syms=$("$nm" "$hd/zephyr/zephyr.elf" 2>/dev/null) || true
	if ! grep -q ' T tr_pslot_write$' <<<"$syms"; then
		hp_vision_refuse "$hd/zephyr/zephyr.elf does not link tr_pslot_write -- not an hp_vision build"
		return 1
	fi
	# Peripheral-ownership audit (fix round 6, silicon root cause; fix
	# round 12, review: widened from an i2c-only check -- it missed &uart5
	# left enabled and fighting the HE for its own console/shell, a real
	# silicon-found sibling of the same class of bug). ALLOW is every
	# /soc peripheral hp_vision actually needs: i2c1/csi_i2c (0x49011000,
	# camera SCCB), gpio12 (0x4900c000, camera control lines), cam/
	# csi_capture_port (0x49030000) + d-phy/csi (0x49033000, the CSI
	# receiver), ethosu55 (0x400e1000, the HP-paired NPU) -- the app's OWN
	# real peripherals -- plus the non-peripheral structural nodes every
	# board carries regardless of app (fix round 13, review: these were
	# invisible to an earlier version of this scan for a DIFFERENT reason,
	# below, not because they were ever excluded on purpose): memory/ospi
	# region maps (0x1a000000, 0x83000000), the clock controller
	# (0x1a602000), the pin controller (0x1a603000), SRAM0/1 (0x2000000,
	# 0x2400000), MRAM/flash (0x80000000), and the ARM core's own NVIC +
	# SysTick (0xe000e100, 0xe000e010 -- always present on any Cortex-M,
	# not board- or app-specific). lptimer0 (0x42001000) is allow-listed
	# too, for a concrete reason checked at the SAME time as the scan
	# below (its own comment there) rather than blanket-trusted.
	#
	# fix round 13 (review): a /soc node with NO status property AT ALL is
	# "okay" by plain devicetree spec (status defaults to "okay" when
	# absent) -- every node in the "structural" list above has exactly
	# this shape in the generated zephyr.dts (never an explicit `status =
	# "okay"` line, just no status property at all), so the ORIGINAL
	# version of this scan, which only ever flagged a node when it saw a
	# LITERAL `status = "okay"` line, silently treated all of them as "not
	# okay" and never looked at them -- not because they were vetted safe,
	# because the scan had a blind spot for the devicetree's own default.
	# Fixed: no status property recorded for a popped node now defaults to
	# 'okay', matching real Zephyr semantics, and the allow-list above
	# grew to match every genuinely-safe structural node that default now
	# surfaces (it was already effectively "allowed" by the blind spot;
	# this makes that explicit and checked, not accidental). Anything else
	# status=okay (explicit or by omission) under /soc is refused --
	# labelled or not (the original i2c-only regex required an `i2cN:`
	# label prefix; a bare, unlabelled `@addr {` node matches too now).
	# Checked against the GENERATED zephyr.dts (the actual node status the
	# build will link), not `nm`: a driver's ISR symbol match can't tell
	# WHICH instance(s) it is wired to (one shared ISR function, instance
	# selected by DEVICE_DT_DEFINE's device pointer), while the DTS status
	# is the definitive, per-instance source of truth for what gets built.
	# Stack-based nesting (same as the original): a child device node's
	# OWN status (e.g. an arx3a0 camera sensor node inside its i2c bus)
	# must never be mistaken for its PARENT /soc peripheral's status --
	# test_hp_vision_check.sh's own fixture exercises exactly this. Direct
	# /soc children only: soc_depth anchors the scan so a deeply-nested
	# child's address never gets compared against ALLOW in its own right.
	local dts="$hd/zephyr/zephyr.dts" bad
	if [ ! -f "$dts" ]; then
		hp_vision_refuse "$hd/zephyr/zephyr.dts missing -- can't audit peripheral ownership"
		return 1
	fi
	bad=$(python3 - "$dts" <<'PY'
import re, sys
ALLOW = {
	'49011000', '4900c000', '49030000', '49033000', '400e1000',  # this app's own peripherals
	'1a000000', '83000000', '1a602000', '1a603000',              # memory/ospi maps, clock/pin controllers
	'2000000', '2400000', '80000000',                            # SRAM0, SRAM1, MRAM/flash
	'e000e100', 'e000e010',                                      # ARM core: NVIC, SysTick
	'42001000',                                                  # lptimer0 -- see the scan below's own check
}
depth = 0
soc_seen = False
in_soc_depth = None  # the stack depth /soc's OWN frame sits at, while still open
stack = []
bad = []
for line in open(sys.argv[1]):
	if not soc_seen and re.match(r'\s*(?:[A-Za-z0-9_]+:\s*)*soc\s*\{', line):
		# fix round 13 (review, second pass): a bare depth counter here is
		# STICKY -- it is set once and never reset, so a ROOT-level sibling
		# of /soc that textually appears AFTER /soc's own closing brace (e.g.
		# /cpus/cpu@0) can land at the same numeric nesting depth /soc's real
		# children sit at and get misread as one. A stack-pushed 'SOC' marker
		# fixes this: in_soc_depth is only ever cleared when THIS SAME marker
		# is popped, i.e. when /soc's own subtree genuinely closes, not by
		# coincidental depth arithmetic.
		soc_seen = True
		stack.append([depth, 'SOC', None, False])
		in_soc_depth = depth
		depth += 1
		continue
	# fix round 13 (review, second pass): the label group was `(?:label:)?`
	# -- ONE optional label. A generated node can carry two stacked labels
	# (e.g. `i2c1: csi_i2c: i2c@49011000 {`), which that anchor never
	# matched at all; the line fell through to the generic-brace branch
	# below and got pushed as an untracked, unaddressed node (direct=False,
	# inst=None) -- invisible to the ALLOW check no matter its status. `*`
	# instead of `?` accepts any number of stacked labels.
	m = re.match(r'\s*(?:[A-Za-z0-9_]+:\s*)*[A-Za-z0-9_,+\-]+@([0-9a-fA-F]+)\s*\{', line)
	if m:
		direct = in_soc_depth is not None and depth == in_soc_depth + 1
		stack.append([depth, m.group(1), None, direct])
		depth += 1
		continue
	if re.search(r'\{\s*$', line):
		stack.append([depth, None, None, False])
		depth += 1
		continue
	if re.match(r'\s*\};', line):
		if stack and stack[-1][0] == depth - 1:
			_, inst, status, direct = stack.pop()
			if inst == 'SOC':
				in_soc_depth = None
			else:
				status = status or 'okay'  # no status property: "okay" by devicetree spec, not "not okay"
				if inst is not None and direct and status == 'okay' and inst not in ALLOW:
					bad.append(inst)
		depth -= 1
		continue
	if stack and stack[-1][0] == depth - 1:
		m2 = re.search(r'status = "(okay|disabled)"', line)
		if m2:
			stack[-1][2] = m2.group(1)
print(' '.join(sorted(set(bad))))
PY
	)
	if [ -n "$bad" ]; then
		hp_vision_refuse "/soc peripheral(s) 0x$bad are status=okay in $dts but not in this app's own allow-list (i2c1, gpio12, cam, csi/d-phy, ethosu55) -- the HP owns only what it drives; a stray enabled peripheral can contend with the HE for a shared resource (silicon-proven twice: i2c2's ISR, fix round 6; uart5's console, fix round 12) -- disable it in hp_vision's board overlay"
		return 1
	fi
	# fix round 13: lptimer0 (0x42001000, above) is allow-listed on a
	# CHECKED reason, not just a documented one -- its own Zephyr driver
	# (drivers/counter/counter_alif_lptimer.c) only builds, and only then
	# installs an ISR on this core's own NVIC, when CONFIG_COUNTER is set;
	# the generated .config says it is not (this app never asked for a
	# counter), so lptimer0 being DT-"okay" by omission (no explicit
	# status line at all -- see the scan's own header comment) never
	# actually attaches anything to this core. If that ever changes --
	# some future dependency selects CONFIG_COUNTER on -- this must refuse
	# again until lptimer0's own ownership gets the SAME real audit i2c2
	# and uart5 already got, not stay allow-listed on a stale assumption.
	local cfg="$hd/zephyr/.config"
	if [ ! -f "$cfg" ]; then
		hp_vision_refuse "$hd/zephyr/.config missing -- can't confirm lptimer0 (0x42001000) has no driver"
		return 1
	fi
	if grep -q '^CONFIG_COUNTER=y' "$cfg"; then
		hp_vision_refuse "CONFIG_COUNTER=y in $cfg -- lptimer0 (0x42001000) is allow-listed ONLY because no counter driver was built; a counter driver now is, so it needs a real ownership audit (i2c2/uart5's own precedent, fix round 6/12) before this stays allowed"
		return 1
	fi
	if [ ! -f "$model" ]; then
		hp_vision_refuse "model file '$model' not found -- see src/vision/movenet_mram.h; tools fetch/cut it, none is committed"
		return 1
	fi
	# TR_MOVENET_MRAM_SIZE (src/vision/movenet_mram.h): the design doc's cut
	# model table figure. An exact-size check, not a bound: a Vela re-run
	# with different flags (or the wrong model entirely) produces a
	# different byte count, and movenet_mram.h's address math (this file's
	# caller) assumes exactly this many bytes.
	local want=2429520 got
	got=$(stat -c %s "$model")
	if [ "$got" != "$want" ]; then
		hp_vision_refuse "model file is $got B, want $want B (src/vision/movenet_mram.h TR_MOVENET_MRAM_SIZE) -- wrong model or a different Vela run"
		return 1
	fi
	echo "build-release: TR_HP_VISION=ON allowed: $hd (TR_CAM_ROTATE=$rot TR_CAM_MIRROR=${mir:-<unset>}), model $model ($got B)" >&2
	return 0
}
