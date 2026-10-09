#!/bin/bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# tests/host/runner.sh -- builds and runs every host test; exits non-zero if any test failed.
set -u
cd "$(dirname "$0")/../.." || exit 1
rc=0
# Per-run scratch for the stages below that need a fixed file name.
RUN_TMP=$(mktemp -d "${TMPDIR:-/tmp}/tr-run.XXXXXX") || exit 1
trap 'rm -rf "$RUN_TMP"' EXIT
# TR_RASTER_CHECKS: r3d_raster.c's invariant asserts (never in the A32 image).
CHECKS="-DTR_RASTER_CHECKS"
for t in tests/host/test_*.c; do
	out="$RUN_TMP/tr-$(basename "$t" .c)"
	extra_cflags=""
	# test_track_cam_orientation.c needs TR_CAM_FLIP_Y compiled in as 1
	# (track.h #ifndef-guards it) to exercise the `1` state of the
	# camera-orientation constant against the real track.c / pose.c -- see
	# that file's header comment.
	if [ "$(basename "$t")" = "test_track_cam_orientation.c" ]; then
		extra_cflags="-DTR_CAM_FLIP_Y=1"
	fi
	# test_tilt_takeover.c re-runs test_tilt.c with the bench/dev takeover
	# compiled in (tilt.h's TR_TILT_TAKEOVER, default OFF).
	if [ "$(basename "$t")" = "test_tilt_takeover.c" ]; then
		extra_cflags="-DTR_TILT_TAKEOVER=1"
	fi
	# test_a32_turned.c compares a 720-wide panel's frame with the centre crop of the 800-wide one,
	# which only holds with the focal length pinned (r3d_scene.h tr_scene_f_px: it follows fw).
	if [ "$(basename "$t")" = "test_a32_turned.c" ]; then
		extra_cflags="-DTR_SCENE_F_PX_FIXED=1"
	fi
	# test_r3d_zones.c reads the raster's TR_PROF_* counters for its per-zone
	# cost estimate (the A32 qemu stage below builds it without: goldens only).
	if [ "$(basename "$t")" = "test_r3d_zones.c" ]; then
		extra_cflags="-DTR_RASTER_PROF=1"
	fi
	# -ffp-contract=off: no fused multiply-add, so the float front-end
	# (r3d_math.c) computes the same bits here as on the A32/M55 -- every
	# target build must pass it too (see r3d_math.c).
	if ! cc -std=c11 -Wall -Wextra -Werror -ffp-contract=off -g $CHECKS $extra_cflags -o "$out" "$t" \
		$(ls src/game/*.c src/vision/*.c src/hud/*.c src/render/sprite.c src/render/proj.c \
		     src/render/r3d_math.c src/render/r3d_raster.c src/render/r3d_scene.c src/render/r3d_rig.c \
		     src/render/cam_pip.c src/ipc/*.c src/audio/*.c 2>/dev/null | grep -v main.c) -lm; then
		echo "BUILD FAIL: $t"; rc=1; continue
	fi
	if "$out"; then echo "PASS: $t"; else echo "FAIL: $t"; rc=1; fi
done
# The arm controls with the sensor NOT mirrored (cam_rot.h TR_CAM_MIRROR=0: a
# camera that sees the player face to face): the same physical player's LEFT
# arm must still be the left lane, at every rotation (the default build above
# is the release's selfie mirror).
for t in test_arms test_cam_mirror; do
	out="$RUN_TMP/tr-$t-nomirror"
	if cc -std=c11 -Wall -Wextra -Werror -ffp-contract=off -g -DTR_CAM_MIRROR=0 -o "$out" "tests/host/$t.c" \
		$(ls src/game/*.c src/vision/*.c src/ipc/*.c 2>/dev/null | grep -v main.c) -lm && "$out" >/dev/null; then
		echo "PASS: tests/host/$t.c -DTR_CAM_MIRROR=0"
	else
		echo "FAIL: tests/host/$t.c -DTR_CAM_MIRROR=0"; rc=1
	fi
done
# The 30 Hz panel build (P11a,-DTR_PANEL_HZ=30): the frame-counted game
# logic re-run with its constants scaled -- test_panel_hz.c checks the real
# times, the others that the logic holds at the 30 Hz counts.
for t in test_panel_hz test_step test_pace test_tilt test_tilt_takeover test_attract test_mbox test_hud test_score test_react \
	test_hiscore test_ramp; do
	out="$RUN_TMP/tr-$t-30hz"
	extra_cflags="-DTR_PANEL_HZ=30"
	[ "$t" = test_tilt_takeover ] && extra_cflags="$extra_cflags -DTR_TILT_TAKEOVER=1"
	if cc -std=c11 -Wall -Wextra -Werror -ffp-contract=off -g $extra_cflags -o "$out" "tests/host/$t.c" \
		$(ls src/game/*.c src/vision/*.c src/hud/*.c src/render/sprite.c src/render/proj.c src/ipc/*.c 2>/dev/null |
			grep -v main.c) -lm && "$out" >/dev/null; then
		echo "PASS: tests/host/$t.c -DTR_PANEL_HZ=30"
	else
		echo "FAIL: tests/host/$t.c -DTR_PANEL_HZ=30"; rc=1
	fi
done
# The synth's other builds (bench A/B): V1 (must keep golden 0xDBF70C58), V2
# and V3 at 48 kHz, each with its own golden in test_audio.c.
for f in "-DTR_AUDIO_V2=0" "-DTR_AUDIO_V3=0" "-DTR_AUDIO_RATE=48000u"; do
	out="$RUN_TMP/tr-test_audio$(echo "$f" | tr -dc 'A-Za-z0-9_')"
	if cc -std=c11 -Wall -Wextra -Werror -ffp-contract=off -g $f -o "$out" tests/host/test_audio.c \
		src/audio/tr_audio.c src/ipc/tr_aring.c -lm && "$out" >/dev/null; then
		echo "PASS: tests/host/test_audio.c $f"
	else
		echo "FAIL: tests/host/test_audio.c $f"; rc=1
	fi
done
# The bench tools: the PC renderer must build (tools/audio_spectro.py
# compiles it to analyse every capture), and the analyser's selftest must
# pass. numpy is the one non-stdlib need; without it the stage is skipped
# loudly below (TR_REQUIRE_CROSS=1 turns that into a failure).
if cc -std=c11 -Wall -Wextra -Werror -O2 -Isrc -o "$RUN_TMP/tr-audio_preview" tools/audio_preview.c \
	src/audio/tr_audio.c; then
	echo "PASS: tools/audio_preview.c builds"
else
	echo "FAIL: tools/audio_preview.c does not build"; rc=1
fi
SPECTRO_NUMPY=1
python3 -c "import numpy" 2>/dev/null || SPECTRO_NUMPY=0
# The HP sound release interlock (a32/release/snd_hp_check.sh).
if bash tests/host/test_snd_hp_check.sh; then
	echo "PASS: tests/host/test_snd_hp_check.sh"
else
	echo "FAIL: tests/host/test_snd_hp_check.sh"; rc=1
fi
# The HP vision release interlock (a32/release/hp_vision_check.sh).
if bash tests/host/test_hp_vision_check.sh; then
	echo "PASS: tests/host/test_hp_vision_check.sh"
else
	echo "FAIL: tests/host/test_hp_vision_check.sh"; rc=1
fi
# The combined HP image (camera + NPU + game sound in one HP_APP): its build gate, release
# interlocks and the source-level wiring of the I2C2/GPIO5 lease (a32/release/*_check.sh).
if bash tests/host/test_hp_combined.sh; then
	echo "PASS: tests/host/test_hp_combined.sh"
else
	echo "FAIL: tests/host/test_hp_combined.sh"; rc=1
fi
# The power graph's source interlock: the I2C2 lease check before any transfer, a gap for every slot the poll
# cannot sample, never blocking (platform/rail5v_power.c is a Zephyr file with no host build).
if bash tests/host/test_rail5v_gap.sh; then
	echo "PASS: tests/host/test_rail5v_gap.sh"
else
	echo "FAIL: tests/host/test_rail5v_gap.sh"; rc=1
fi
# The TR_PANEL_HZ release interlock (a32/release/panel_hz_check.sh).
if bash tests/host/test_panel_hz_check.sh; then
	echo "PASS: tests/host/test_panel_hz_check.sh"
else
	echo "FAIL: tests/host/test_panel_hz_check.sh"; rc=1
fi
# Cross stage -- skipped (not failed) when a tool is absent. Point the
# TR_* variables at the toolchains if they are not on PATH.
#  - A32: every test_r3d_*.c built for Cortex-A32 NEON (hard float, newlib
#    semihosting via rdimon) and run under qemu-arm user mode: runs the NEON
#    span/raster loops against their scalar oracles and proves the A32 float
#    front-end emits the committed golden DL bit for bit.
#    test_audio.c runs there too: the synth's golden CRC holds on ARM.
#  - M55: compile-only smoke of the renderer sources (Helium span.h branch),
#    of the sound sources the HE/HP images link, and of the NPU body-control
#    pre/post-processing + IPC both cores link (src/vision/movenet.c, pose.c,
#    camera_ae.c, src/ipc/tr_pslot.c).
A32_GCC=${TR_A32_GCC:-$(command -v arm-none-eabi-gcc)}
QEMU_ARM=${TR_QEMU_ARM:-$(command -v qemu-arm)}
M55_GCC=${TR_M55_GCC:-$(command -v arm-zephyr-eabi-gcc)}
# Missing tools SKIP locally (loudly); TR_REQUIRE_CROSS=1 (CI) makes a
# skipped stage a failure.
skip() {
	echo "!!!!! SKIP: $1 -- cross stage NOT run; set TR_REQUIRE_CROSS=1 to fail instead !!!!!"
	if [ "${TR_REQUIRE_CROSS:-0}" = 1 ]; then rc=1; fi
}
R3D_SRC="src/render/sprite.c src/render/proj.c src/render/r3d_math.c src/render/r3d_raster.c src/render/r3d_scene.c src/render/r3d_rig.c src/render/cam_pip.c"
AUDIO_SRC="src/audio/tr_audio.c src/ipc/tr_aring.c"
WARN="-std=c11 -Wall -Wextra -Wdouble-promotion -Werror -ffp-contract=off -O2"
# r3d_raster.c at the renderer's RASTER_OPT and cam_pip.c at its HOT_OPT (a32/renderer/Makefile), the
# rest at -O2 like the image: the A32 r3d tests prove the goldens and the NEON kernels as shipped.
A32_RASTER_OPT="-O3 -funroll-loops"
A32_HOT_OPT="-O3"

if [ -n "$A32_GCC" ] && [ -n "$QEMU_ARM" ]; then
	A32_R3D="${R3D_SRC/src\/render\/r3d_raster.c/} $RUN_TMP/r3d_raster.o"
	A32_R3D="${A32_R3D/src\/render\/cam_pip.c/} $RUN_TMP/cam_pip.o"
	"$A32_GCC" $WARN $A32_HOT_OPT -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
		-c -o "$RUN_TMP/cam_pip.o" src/render/cam_pip.c || { echo "BUILD FAIL (A32): cam_pip.c"; rc=1; }
	"$A32_GCC" $WARN $A32_RASTER_OPT $CHECKS -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
		-c -o "$RUN_TMP/r3d_raster.o" src/render/r3d_raster.c || { echo "BUILD FAIL (A32): r3d_raster.c"; rc=1; }
	for t in tests/host/test_r3d_*.c tests/host/test_audio.c; do
		out="$RUN_TMP/tr-a32-$(basename "$t" .c).elf"
		if ! "$A32_GCC" $WARN -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
			--specs=rdimon.specs -o "$out" "$t" $A32_R3D $AUDIO_SRC -lm; then
			echo "BUILD FAIL (A32): $t"; rc=1; continue
		fi
		if "$QEMU_ARM" -cpu max "$out" >/dev/null; then echo "PASS (A32 qemu): $t"; else echo "FAIL (A32 qemu): $t"; rc=1; fi
	done
	# The rot-90/270 NEON copy-out header (tr_rot_blit_neon) at the image's -O3 too.
	out="$RUN_TMP/tr-a32-panel_rot-O3.elf"
	if "$A32_GCC" $WARN $A32_HOT_OPT -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
		--specs=rdimon.specs -o "$out" tests/host/test_r3d_panel_rot.c $A32_R3D -lm &&
		"$QEMU_ARM" -cpu max "$out" >/dev/null; then
		echo "PASS (A32 qemu -O3): tests/host/test_r3d_panel_rot.c"
	else
		echo "FAIL (A32 qemu -O3): tests/host/test_r3d_panel_rot.c"; rc=1
	fi
	for f in "-DTR_AUDIO_V2=0" "-DTR_AUDIO_V3=0" "-DTR_AUDIO_RATE=48000u"; do
		out="$RUN_TMP/tr-a32-test_audio$(echo "$f" | tr -dc 'A-Za-z0-9_').elf"
		if "$A32_GCC" $WARN -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard --specs=rdimon.specs \
			$f -o "$out" tests/host/test_audio.c $AUDIO_SRC -lm && "$QEMU_ARM" -cpu max "$out" >/dev/null; then
			echo "PASS (A32 qemu): tests/host/test_audio.c $f"
		else
			echo "FAIL (A32 qemu): tests/host/test_audio.c $f"; rc=1
		fi
	done
else
	skip "A32 NEON stage (set TR_A32_GCC / TR_QEMU_ARM)"
fi

# render.c at the renderer image's HOT_OPT (a32/renderer/Makefile, -O3) on Cortex-A32 under qemu-arm:
# the test_a32_*.c suites include it (RENDER_A32=0: the scalar paths, no CP15), so the goldens hold
# for what the image's -O3 objects compute; test_a32_copyout_neon.c includes it as RENDER_A32=1 and runs
# the NEON rot-90/270 copy-out (tr_rot_blit_neon inlined into copy_rows_turned) against the mapping.
# Only the test's own TU (render.c) is -O3; the game / vision / hud / ipc sources it links are built at -O2 like the other A32 stages.
if [ -n "$A32_GCC" ] && [ -n "$QEMU_ARM" ]; then
	A32_DEPS=""
	for f in $(ls src/game/*.c src/vision/*.c src/hud/*.c src/render/sprite.c src/render/proj.c \
		src/render/r3d_math.c src/render/r3d_scene.c src/render/r3d_rig.c src/render/cam_pip.c \
		src/ipc/*.c src/audio/*.c 2>/dev/null | grep -v main.c); do
		o="$RUN_TMP/a32d-$(echo "$f" | tr / _).o"
		"$A32_GCC" $WARN -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard -c -o "$o" "$f" ||
			{ echo "BUILD FAIL (A32): $f"; rc=1; }
		A32_DEPS="$A32_DEPS $o"
	done
	# (test_a32_turned.c / test_a32_video.c mmap a host scratch: host cc only)
	for t in tests/host/test_a32_cold_mailbox.c tests/host/test_a32_font.c tests/host/test_a32_render.c \
		tests/host/test_a32_scene.c tests/host/test_a32_copyout_neon.c; do
		out="$RUN_TMP/tr-a32o3-$(basename "$t" .c).elf"
		if ! "$A32_GCC" $WARN -O3 -mcpu=cortex-a32 -marm -mfpu=neon-fp-armv8 -mfloat-abi=hard \
			--specs=rdimon.specs -o "$out" "$t" $A32_DEPS "$RUN_TMP/r3d_raster.o" -lm; then
			echo "BUILD FAIL (A32 -O3): $t"; rc=1; continue
		fi
		if "$QEMU_ARM" -cpu max "$out" >/dev/null; then echo "PASS (A32 qemu -O3): $t"; else echo "FAIL (A32 qemu -O3): $t"; rc=1; fi
	done
fi

# A32 renderer image (a32/renderer), scene and golden builds: must build warning-free and fit its
# 512 KiB budget (renderer.ld asserts it); the host half of it is
# test_a32_render.c above.
if [ -n "$A32_GCC" ]; then
	if make -s -C a32/renderer CROSS="${A32_GCC%gcc}" >/dev/null &&
		make -s -C a32/renderer CROSS="${A32_GCC%gcc}" V=golden >/dev/null; then
		echo "PASS (A32 build): a32/renderer (scene + golden)"
	else
		echo "FAIL (A32 build): a32/renderer"; rc=1
	fi
else
	skip "A32 renderer image build (set TR_A32_GCC)"
fi

# A32 ISA microbenchmark (a32/payload-isa): its kernels bit-exact against the
# renderer's on the host cc and under qemu-arm NEON, and the -O2 payload builds.
if [ -n "$A32_GCC" ] && [ -n "$QEMU_ARM" ]; then
	if make -s -C a32/payload-isa CROSS="${A32_GCC%gcc}" QEMU_ARM="$QEMU_ARM" TMP="$RUN_TMP/isa-check" check >/dev/null &&
		make -s -C a32/payload-isa CROSS="${A32_GCC%gcc}" >/dev/null; then
		echo "PASS (A32 qemu + build): a32/payload-isa"
	else
		echo "FAIL (A32 qemu + build): a32/payload-isa"; rc=1
	fi
else
	skip "A32 ISA payload (set TR_A32_GCC / TR_QEMU_ARM)"
fi

if [ "$SPECTRO_NUMPY" = 1 ]; then
	if python3 tools/audio_spectro.py --selftest >/dev/null 2>&1; then
		echo "PASS: tools/audio_spectro.py --selftest"
	else
		echo "FAIL: tools/audio_spectro.py --selftest"; rc=1
	fi
else
	skip "tools/audio_spectro.py --selftest (python3 numpy missing)"
fi

if [ -n "$M55_GCC" ]; then
	for f in $R3D_SRC $AUDIO_SRC src/game/sfx.c src/vision/movenet.c src/vision/pose.c src/vision/arms.c \
		src/vision/camera_ae.c src/vision/kp_smooth.c src/ipc/tr_pslot.c src/ipc/tr_cam_view.c; do
		if "$M55_GCC" $WARN -mcpu=cortex-m55 -mthumb -mfloat-abi=hard -c -o /dev/null "$f"; then
			echo "PASS (M55 compile): $f"
		else
			echo "FAIL (M55 compile): $f"; rc=1
		fi
	done
else
	skip "M55 compile smoke (set TR_M55_GCC)"
fi

exit "$rc"
