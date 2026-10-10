#!/bin/bash
# Cross-platform scope: Linux-side bench/host tooling (runs under WSL2 on Windows).
# a32/release/build-release.sh -- T-A9 release ATOC for E1M-AEN803 EVK
# (plan sec 8): stub + embedded renderer as A32_APP, the trace-runner HE
# A32-mode image as HE_APP (SE-loaded to ITCM 0x58000000, booted), the
# parked m55_stub_hp as HP_APP, bl32 (TF-A) as BOOTLOAD.
#
#   build-release.sh SETOOLS_COPY HE_BUILD_DIR [MRAM_READBACK]
#
# SETOOLS_COPY: a PRIVATE copy of Alif's SETOOLS (app-release-exec-linux,
#   cp -a it first): app-gen-toc rewrites build/ (config, images,
#   certificates, logs), so never point this at a shared install.
#   Must already hold build/images/{bl32.bin,m55_stub_hp.bin} and
#   build/config/app-device-config.json (the proven A32 ATOC's inputs).
# HE_BUILD_DIR: the Zephyr build dir of the RELEASE HE image -- A32 mode
#   with the watchdog relaunch, built against THIS renderer:
#     python3 a32/stub/mkpayload.py info a32/renderer/renderer.bin --c-header H
#     west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d DIR . -- \
#       -DSHIELD=<the display: e1m_evk_rk055hdmipi4ma0 with
#       -DEXTRA_DTC_OVERLAY_FILE=panel_30hz.overlay, or e1m_evk_rvt121hvdfwca0>
#       -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=H -DTR_INPUT_NPU=ON ...
#   The renderer is the SAME binary for every display: the HE tells it the
#   panel's mount-rotation in every frame.
#   Refused unless zephyr.elf carries tr_a32_autolaunch_id (src/platform/
#   a32.c) and it equals this renderer's {0x02500000, len, CRC} -- the
#   values the stub's release mode LAUNCHes from MRAM, so the HE's
#   relaunch after a fault re-checks the same bytes. <= 256 KiB (HE ITCM).
#   Also refused unless the display the HE was built for refreshes at 30 Hz
#   (panel_hz_check.sh reads it from zephyr/zephyr.dts: the RK055 shield's
#   stock timing is 40 Hz -- add panel_30hz.overlay; the RVT121 is 30 Hz;
#   TR_ALLOW_PANEL_HZ_40=ON overrides for a genuine 40 Hz release).
#   With TR_HP_SOUND (the combined image below) the HE must also be built with
#   -DTR_HP_SOUND=ON (src/platform/bus2_he.c): without it the HP's sound never
#   gets the bus. Refused unless zephyr.elf carries tr_bus2_he_frame.
# Partner logo (optional): the HE built with -DTR_PARTNER_LOGO=<header from tools/genlogo.py> puts it
#   beside the ALP LAB mark in the attract header (src/hud/hud.c). Without the option the banner is unchanged. This script only checks
#   that the elf and the build's CMakeCache agree (a configured logo is linked in, none otherwise)
#   and says so; the header is the partner's artwork and lives outside this repo.
# MRAM_READBACK (optional): a raw read-back of LIVE MRAM from 0x80000000
#   (J-Link savebin, >= 0x580000 bytes). With it, flowd/ gets whole 16 KiB
#   sector blobs whose bytes outside the images come from the read-back
#   (Flow D rewrites whole sectors; nothing else in them changes). Without
#   it, flowd/ gets only the exact images and recipe.txt names the sectors
#   the flash step must merge with a live read-back itself.
#
# TR_SND_HP=ON (env, default OFF): HP_APP becomes the P10 sound firmware
#   instead of the parked m55_stub_hp. Needs TR_SND_HP_BUILD=<sound/ GAME
#   build dir> AND TR_SND_CARRIER_SERIAL=<unit serial> listed "allow" in
#   a32/release/sound-carriers.txt (today only 2026W36-0002; 2026W36-0009 =
#   the stock-U46 display board is "deny"), and the build's CMakeCache must
#   say TR_SND_REWORKED_U46:BOOL=ON -- snd_hp_check.sh, checked BEFORE any
#   packaging. The sound image muxes I2S3 onto P9_3/4/5 at HP boot; on a
#   stock 74LVC157 U46 that contends. OFF leaves the release unchanged; the
#   HE still pushes sound events into its SRAM0 ring, which nothing reads,
#   and the HUD shows "M55-HP --".
#
# TR_HP_VISION=ON (env, default OFF): HP_APP becomes hp_vision (camera + NPU
#   + pose slot, docs/superpowers/specs/2026-09-24-npu-body-control-design.md)
#   instead of the parked m55_stub_hp. Needs TR_HP_VISION_BUILD=<hp_vision/
#   Zephyr build dir> AND TR_HP_VISION_MODEL=<Vela'd cut MoveNet .tflite, raw
#   bytes, exactly TR_MOVENET_MRAM_SIZE> -- hp_vision_check.sh, checked BEFORE
#   any packaging. The model is NOT part of the ATOC: it is written as its
#   own Flow D item at TR_MOVENET_MRAM_ADDR (src/vision/movenet_mram.h), read
#   in place by the NPU (design sec 2 pass B, no SRAM copy). Pair with the HE's own
#   -DTR_INPUT_NPU=ON build (../CMakeLists.txt) -- an HE built without it
#   still runs (its own camera/detector, or TR_FALLBACK_MODE), just not
#   reading the pose this HP image publishes.
#
# TR_HP_VISION=ON TR_SND_HP=ON together = the COMBINED mode: ONE HP image with the
#   camera + NPU AND the game sound (hp_vision built with -DTR_SND_REWORKED_U46=ON
#   -DTR_HP_SOUND=ON; the HE with -DTR_HP_SOUND=ON). TR_HP_VISION_BUILD is that
#   image; hp_vision_check.sh (combined) AND snd_hp_check.sh (combined: carrier
#   serial allowed in sound-carriers.txt, REWORKED_U46=ON, embedded GAME build,
#   synth linked, sound buffers/heap/stack in the HP DTCM) must pass on it. A
#   TR_SND_HP_BUILD, if given, must name the same directory: two separate HP
#   images are refused (only one can be HP_APP). A sound-carrying hp_vision with
#   TR_HP_VISION=ON alone is refused -- the sound never skips the carrier list.
#
# Writes into SETOOLS_COPY/build: AppTocPackage.bin (+ .sign), the package
# map, flowd/ (+ recipe.txt: address, size, md5 per blob). Writes nothing to
# MRAM itself, touches no hardware.
set -euo pipefail

die() { echo "build-release: $*" >&2; exit 1; }
[ $# -eq 2 ] || [ $# -eq 3 ] || die "usage: $0 SETOOLS_COPY HE_BUILD_DIR [MRAM_READBACK]"
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
st=$(realpath "$1")
hed=$(realpath "$2")
rb=${3:+$(realpath "$3")}
NM=${NM:-arm-zephyr-eabi-nm}
command -v "$NM" >/dev/null || die "$NM not found -- put the Zephyr SDK's arm-zephyr-eabi/bin on PATH or set NM"
[ -x "$st/app-gen-toc" ] || die "$st has no app-gen-toc"
for f in build/images/bl32.bin build/images/m55_stub_hp.bin build/config/app-device-config.json; do
	[ -f "$st/$f" ] || die "$st/$f missing"
done
he="$hed/zephyr/zephyr.bin"
elf="$hed/zephyr/zephyr.elf"
[ -f "$he" ] && [ -f "$elf" ] || die "$hed is not a Zephyr build dir (zephyr/zephyr.{bin,elf})"
[ "$(stat -c %s "$he")" -le 262144 ] || die "HE image > 256 KiB ITCM"
# shellcheck source=a32/release/panel_hz_check.sh
source "$here/panel_hz_check.sh"
panel_hz_check "$hed" || exit 3

# 0. The HP interlocks (sound, vision), before anything is built or copied.
#    One HP_APP: the standalone sound image, the vision image, or the combined vision +
#    sound image (TR_HP_VISION=ON TR_SND_HP=ON, ONE build). Two separate images are refused.
for v in TR_SND_HP TR_HP_VISION; do
	case "${!v:-OFF}" in
	ON | OFF) ;;
	*) die "$v must be ON or OFF, got '${!v}'" ;;
	esac
done
snd=${TR_SND_HP:-OFF} vis=${TR_HP_VISION:-OFF}
# shellcheck source=a32/release/snd_hp_check.sh
source "$here/snd_hp_check.sh"
# shellcheck source=a32/release/hp_vision_check.sh
source "$here/hp_vision_check.sh"
sd="" hv="" hv_model="" combined=0
if [ "$vis" = ON ]; then
	hv=$(realpath -m "${TR_HP_VISION_BUILD:-/nonexistent-TR_HP_VISION_BUILD-unset}")
	hv_model=$(realpath -m "${TR_HP_VISION_MODEL:-/nonexistent-TR_HP_VISION_MODEL-unset}")
	if [ "$snd" = ON ]; then
		if [ -n "${TR_SND_HP_BUILD:-}" ] && [ "$(realpath -m "$TR_SND_HP_BUILD")" != "$hv" ]; then
			snd_hp_refuse "two separate HP images (TR_SND_HP_BUILD=$TR_SND_HP_BUILD, TR_HP_VISION_BUILD=$hv): only one can be HP_APP -- build hp_vision with -DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=ON and leave TR_SND_HP_BUILD unset"
			exit 3
		fi
		hp_vision_check "$hv" "$hv_model" "$NM" "$hed" combined || exit 3
		snd_hp_check "$hv" "${TR_SND_CARRIER_SERIAL:-}" "$here/sound-carriers.txt" "$NM" combined || exit 3
		combined=1
		# The HE must run its side of the I2C2 + GPIO5 lease, or the HP's amp bring-up waits for ever.
		if ! grep -q ' tr_bus2_he_frame$' <<<"$("$NM" "$elf" 2>/dev/null)"; then
			snd_hp_refuse "$hed's zephyr.elf lacks tr_bus2_he_frame: build the HE with -DTR_HP_SOUND=ON (the HP's amp bring-up needs the HE to lease it I2C2 + GPIO5)"
			exit 3
		fi
	else
		hp_vision_check "$hv" "$hv_model" "$NM" "$hed" || exit 3
	fi
elif [ "$snd" = ON ]; then
	sd=$(realpath -m "${TR_SND_HP_BUILD:-/nonexistent-TR_SND_HP_BUILD-unset}")
	snd_hp_check "$sd" "${TR_SND_CARRIER_SERIAL:-}" "$here/sound-carriers.txt" "$NM" || exit 3
fi

# 0b. The partner logo: what the HE's CMakeCache says must be what its elf carries (hud.c's
#     tr_partner_logo[]), so a stale build dir cannot ship a logo nobody asked for, or lose one.
logo=$(sed -n 's/^TR_PARTNER_LOGO:[A-Z]*=//p' "$hed/CMakeCache.txt" 2>/dev/null | tr -d '\r')
syms=$("$NM" "$elf" 2>/dev/null) && [ -n "$syms" ] || die "$NM could not read the symbols of $elf"
has_logo=0
grep -q ' tr_partner_logo$' <<<"$syms" && has_logo=1
if [ -n "$logo" ] && [ "$has_logo" = 0 ]; then
	die "$hed was configured with TR_PARTNER_LOGO=$logo but zephyr.elf has no tr_partner_logo -- rebuild the HE"
elif [ -z "$logo" ] && [ "$has_logo" = 1 ]; then
	die "$hed's zephyr.elf carries a partner logo but its CMakeCache has no TR_PARTNER_LOGO -- rebuild the HE"
fi
[ -z "$logo" ] || echo "build-release: partner logo compiled in (TR_PARTNER_LOGO=$logo)" >&2

# 1. stub + renderer (scene build) -> one A32_APP image, stub release mode:
#    the stub copies the payload MRAM -> 0x02500000 and self-LAUNCHes.
make -s -C "$repo/a32/stub" >/dev/null
# The renderer's boot layout follows the HE's camera rotation (same as the HP's, hp_vision_check.sh).
hrot=$(sed -n 's/^TR_CAM_ROTATE:[A-Z]*=//p' "$hed/CMakeCache.txt" 2>/dev/null | tr -d '\r')
case "${hrot:-90}" in 0 | 90 | 270) ;; *) die "HE TR_CAM_ROTATE='$hrot' must be 0, 90 or 270" ;; esac
make -s -C "$repo/a32/renderer" TR_CAM_ROTATE="${hrot:-90}" >/dev/null
rend="$repo/a32/renderer/renderer.bin"
python3 "$repo/a32/stub/mkpayload.py" release "$repo/a32/stub/a32_stub.bin" "$rend" \
	-o "$st/build/images/trace_runner_a32.bin"

# 2. The HE must be the A32-mode AUTOLAUNCH build for exactly this renderer.
python3 - "$NM" "$elf" "$he" "$rend" <<'PY'
import struct, subprocess, sys, zlib
nm, elf, binp, rend = sys.argv[1:]
syms = {}
for l in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
for s in ("tr_a32_boot", "tr_display_flip_to", "tr_a32_autolaunch_id"):
    if s not in syms:
        sys.exit("build-release: HE image lacks %s -- not an A32-mode TR_M55_AUTOLAUNCH build" % s)
img = open(binp, "rb").read()
off = syms["tr_a32_autolaunch_id"] - syms["__rom_region_start"]
got = struct.unpack_from("<3I", img, off)
r = open(rend, "rb").read()
want = (0x02500000, len(r), zlib.crc32(r) & 0xFFFFFFFF)
if got != want:
    sys.exit("build-release: HE LAUNCHes %s, this renderer is %s -- rebuild the HE with a fresh "
             "mkpayload --c-header from THIS renderer.bin. Build the whole release (renderer, launch "
             "header, HE) on ONE machine with ONE arm-none-eabi toolchain: the same sources give "
             "different renderer.bin bytes (so a different CRC) under different gcc packages."
             % (tuple(hex(x) for x in got), tuple(hex(x) for x in want)))
print("build-release: HE autolaunch id matches renderer (entry 0x%08X len %d crc 0x%08X)" % want)
PY
cp "$he" "$st/build/images/trace_runner_he.bin"
# app-gen-toc accounts images in 16-byte units (the map shows the rounded
# size); pad both images ourselves so the signed bytes, the map and the
# flashed bytes are the same (zeros past payload_len are ignored by the
# stub; the HE image is loaded whole into ITCM).
for f in "$st/build/images/trace_runner_a32.bin" "$st/build/images/trace_runner_he.bin"; do
	sz=$(stat -c %s "$f")
	truncate -s $(( (sz + 15) / 16 * 16 )) "$f"
done
cp "$here/e1m-aen-evk-trace-runner.json" "$st/build/config/"
if [ -n "$sd" ]; then # passed snd_hp_check above
	cp "$sd/zephyr/zephyr.bin" "$st/build/images/trace_runner_hp_sound.bin"
	sz=$(stat -c %s "$st/build/images/trace_runner_hp_sound.bin")
	truncate -s $(( (sz + 15) / 16 * 16 )) "$st/build/images/trace_runner_hp_sound.bin"
	sed -i 's/"m55_stub_hp.bin"/"trace_runner_hp_sound.bin"/' "$st/build/config/e1m-aen-evk-trace-runner.json"
	echo "build-release: TR_SND_HP=ON -- HP_APP = P10 sound firmware ($sd); REWORKED-U46 CARRIERS ONLY" >&2
fi
if [ -n "$hv" ]; then # passed hp_vision_check above
	cp "$hv/zephyr/zephyr.bin" "$st/build/images/trace_runner_hp_vision.bin"
	sz=$(stat -c %s "$st/build/images/trace_runner_hp_vision.bin")
	truncate -s $(( (sz + 15) / 16 * 16 )) "$st/build/images/trace_runner_hp_vision.bin"
	sed -i 's/"m55_stub_hp.bin"/"trace_runner_hp_vision.bin"/' "$st/build/config/e1m-aen-evk-trace-runner.json"
	if [ "$combined" = 1 ]; then
		echo "build-release: TR_HP_VISION=ON TR_SND_HP=ON -- HP_APP = camera+NPU vision AND game sound in ONE image ($hv); REWORKED-U46 CARRIERS ONLY" >&2
	else
		echo "build-release: TR_HP_VISION=ON -- HP_APP = camera+NPU vision firmware ($hv)" >&2
	fi
fi

# 3. ATOC package, in the private copy.
(cd "$st" && ./app-gen-toc -f build/config/e1m-aen-evk-trace-runner.json) > "$st/build/gen-toc.log"
tail -3 "$st/build/gen-toc.log"

# 4. Flow D inputs.
python3 - "$st" "$rb" "$hv_model" <<'PY'
import hashlib, os, re, sys
st, rb, model = sys.argv[1], sys.argv[2], sys.argv[3]
b = os.path.join(st, "build")
SECT, BASE, END = 0x4000, 0x80000000, 0x80580000  # 16 KiB sectors; package ends at the System MRAM base
text = open(os.path.join(b, "app-package-map.txt")).read()
start = int(re.search(r"APP Package Start Address: (0x[0-9a-fA-F]+)", text).group(1), 16)
pkg = open(os.path.join(b, "AppTocPackage.bin"), "rb").read()
assert start + len(pkg) == END, "package does not end at 0x%08X" % END
items = [("bl32", 0x80002000, open(os.path.join(b, "images", "bl32.bin"), "rb").read()),
         ("a32_app", 0x80020000, open(os.path.join(b, "images", "trace_runner_a32.bin"), "rb").read()),
         ("atoc", start, pkg)]
if model and os.path.isfile(model):
    # TR_MOVENET_MRAM_ADDR/SIZE (src/vision/movenet_mram.h): NOT part of the
    # ATOC -- read in place by the NPU (design sec 2 pass B). hp_vision_check.sh
    # already checked the byte count against the same SIZE constant.
    items.append(("movenet_model", 0x80100000, open(model, "rb").read()))
spans = [(n, a & ~(SECT - 1), (a + len(d) + SECT - 1) & ~(SECT - 1)) for n, a, d in items]
for i in range(len(spans)):
    for j in range(i + 1, len(spans)):
        (n1, l1, h1), (n2, l2, h2) = spans[i], spans[j]
        assert h1 <= l2 or h2 <= l1, "%s and %s share a 16 KiB sector: 0x%08X..0x%08X / 0x%08X..0x%08X" % (
            n1, n2, l1, h1, l2, h2)
live = open(rb, "rb").read() if rb else None
if live is not None:
    assert len(live) >= END - BASE, "read-back is %d B, need >= 0x%X from 0x%08X" % (len(live), END - BASE, BASE)
out = os.path.join(b, "flowd")
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))
hdr = ("# whole 16 KiB sectors, bytes outside the image from the live read-back %s" % os.path.basename(rb)
       if live is not None else
       "# EXACT images only: Flow D rewrites whole 16 KiB sectors, so the flash step\n"
       "# must build each sector from a LIVE read-back + the image (sector range given)")
lines = ["# Flow D inputs for e1m-aen-evk-trace-runner", hdr,
         "# addr        size     md5                               image / sectors"]
for (name, addr, data), (_, lo, hi) in zip(items, spans):
    if live is not None:
        blob = bytearray(live[lo - BASE:hi - BASE])
        blob[addr - lo:addr - lo + len(data)] = data
        at = lo
    else:
        blob, at = bytes(data), addr
    open(os.path.join(out, "%s-0x%08X.bin" % (name, at)), "wb").write(blob)
    lines.append("0x%08X  %7d  %s  %s 0x%08X..0x%08X (%d B), sectors 0x%08X..0x%08X" % (
        at, len(blob), hashlib.md5(blob).hexdigest(), name, addr, addr + len(data) - 1, len(data), lo, hi - 1))
open(os.path.join(out, "recipe.txt"), "w").write("\n".join(lines) + "\n")
print("\n".join(lines))
PY
echo "build-release: package + Flow D inputs in $st/build (flowd/recipe.txt)"
