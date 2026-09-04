#!/usr/bin/env bash
# scripts/bench/aen/ram-run.sh <build-dir> [sleep_ms] [bufsize_hex] [preload_jlink_file]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives JLinkExe). Runs under WSL2 on Windows. No SETOOLS/SE-UART —
# this flow never writes MRAM. See docs/aen-bench-bringup.md.
#
# FLOW C -- RAM-run a Zephyr ITCM image on the E8 (M55-HE) over J-Link and
# ASCII-decode the CONFIG_RAM_CONSOLE buffer ('ram_console_buf') read back over SWD.
#   - loadbin does an implicit SYSRESETREQ + halt-at-reset-vector; we then
#     setpc <entry> + go (loadbin alone does NOT reliably enter our vectors).
#   - optional preload file: extra JLink commands run AFTER halt, BEFORE loadbin
#     (e.g. clear a SoC integration reg for the cold-RAM-run gotcha).
#   - the load address is DERIVED from the LOAD segment with the LOWEST
#     p_paddr among those with a NONZERO p_filesz (readelf -l), not hard-coded
#     0x0 and NOT just "the first LOAD segment" -- a Flow-C-ITCM build's first
#     LOAD segment is often a zero-FileSiz .bss segment in DTCM
#     (e.g. 0x20000228), and loading the image there splats it over live RAM.
#     An app that hard-codes CONFIG_FLASH_LOAD_OFFSET (e.g. the slot0 offset
#     0x10000) still links correctly at a non-zero ITCM address and must be
#     loaded there. If the derived base is >= 0x80000000 the image is
#     slot0/MRAM-linked and Flow C cannot run it (loading it at its resident
#     address just re-enters the already-resident MRAM image, not the freshly
#     built one) -- refuse rather than silently mis-run it. A second,
#     positive check also refuses any derived base that isn't 0x0, the ITCM
#     global alias (0x50000000 / 0x58000000), or SRAM (0x02xxxxxx) -- a DTCM
#     address slipping through here is exactly the bug this guards against.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SLEEP="${2:-1500}"
SIZE="${3:-0x600}"
PRELOAD="${4:-}"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
# Select the AEN probe by serial when JLINK_SN is set (bench-env.sh resolves it
# from JLINK_SN/JLINK_SERIAL) -- on alplab-gw, JLinkExe otherwise picks an
# arbitrary probe among the V2N CM33 DAP / AEN E8 / GD32 bridge and either
# fails to connect or attaches the wrong one.
#
# Leaving JLINK_SN unset is NOT a no-op: an earlier revision of this comment
# claimed it "preserves today's single-probe behaviour exactly", which was true
# when only one probe was attached and is false now. alplab-gw carries three,
# two sharing a cloned OEM serial, and an unselected run there fails every
# command with "Cannot connect to the probe/programmer" (alp-sdk#1318). That is
# why the connect assertion below is mandatory rather than advisory.
#
# NOTE: this is read/RAM-run only (no MRAM write) and this script does not
# confirm the SW-DP ID the way flash-jlink-mramxip.sh does before a write.
JLINK_ARGS=("$JLINK")
[ -n "${JLINK_SN:-}" ] && JLINK_ARGS+=(-SelectEmuBySN "$JLINK_SN")
ELF="$BD/zephyr/zephyr.elf"
BIN="$BD/zephyr/zephyr.bin"
ENTRY_RAW=$($OBJ-readelf -h "$ELF" | awk '/Entry point/{print $NF}')
ENTRY=$(printf '0x%X' $(( ENTRY_RAW & ~1 )))         # clear thumb bit
BUF_SYM=$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')
if [ -z "$BUF_SYM" ]; then
	# No RAM console linked in.  Without this guard BUF would be the bare
	# string "0x", JLink would run `mem8 0x, <size>`, and the operator would
	# get an EMPTY "RAM console (decoded)" block with no hint why -- which
	# reads as "the app crashed" when the app is fine and simply routed its
	# output to a UART.  Flow C produces no capturable UART output, so a
	# UART-console app is invisible here (issue #935).
	cat >&2 <<-EOF
	ram-run: '$ELF' has no 'ram_console_buf' symbol -- this app was built
	         with the UART console, which Flow C cannot capture.
	         Rebuild it with the RAM console AND the Flow C ITCM retarget
	         layered on top (both the conf fragments AND the overlay, in
	         this order):
	             scripts/bench/aen/build.sh <app-dir> \
	                 -DEXTRA_CONF_FILE="$ALP_SDK_DIR/scripts/bench/aen/aen-bench-shared.conf;$ALP_SDK_DIR/scripts/bench/aen/aen-flowc-itcm.conf" \
	                 -DEXTRA_DTC_OVERLAY_FILE="$ALP_SDK_DIR/scripts/bench/aen/aen-flowc-itcm.overlay"
	         (aen-bench-shared.conf sets CONFIG_RAM_CONSOLE=y +
	         CONFIG_UART_CONSOLE=n; aen-flowc-itcm.conf sets
	         CONFIG_USE_DT_CODE_PARTITION=n + CONFIG_FLASH_LOAD_OFFSET=0x0;
	         aen-flowc-itcm.overlay retargets zephyr,flash to &itcm and
	         drops zephyr,code-partition -- all Flow-C-only, do not use any
	         of them for a Flow A/D MRAM build. Both the conf half AND the
	         overlay half are required -- the conf alone still links into
	         MRAM).
	         The app itself is unchanged -- its committed prj.conf keeps the
	         customer-facing UART console.
	EOF
	exit 3
fi
BUF=0x$BUF_SYM

# Select the LOAD segment that objcopy -O binary actually emits from: the
# one with the LOWEST p_paddr among segments with a NONZERO p_filesz. Do NOT
# rely on segment order -- a DTCM .bss LOAD segment (p_filesz=0) commonly
# sorts first in an ITCM-retargeted link.
BASE_RAW=$($OBJ-readelf -l "$ELF" | awk '
/^[[:space:]]*LOAD[[:space:]]/ {
	paddr_hex = $4
	if (strtonum($5) != 0 && (!found || strtonum($4) < min)) {
		min = strtonum($4); min_hex = paddr_hex; found = 1
	}
}
END { if (found) print min_hex }
')
if [ -z "$BASE_RAW" ]; then
	echo "ram-run: could not find a LOAD segment with nonzero FileSiz in '$ELF' -- can't derive the load address." >&2
	exit 4
fi
if ! [[ "$BASE_RAW" =~ ^0x[0-9A-Fa-f]+$ ]]; then
	echo "ram-run: parsed load address '$BASE_RAW' is not a valid hex value -- refusing to guess." >&2
	exit 4
fi
BASE=$(printf '0x%X' "$BASE_RAW")
if (( BASE_RAW >= 0x80000000 )); then
	cat >&2 <<-EOF
	ram-run: '$ELF' is slot0/MRAM-linked (LOAD segment at $BASE) --
	         Flow C cannot RAM-run this: loading it at its resident address
	         just re-enters the ALREADY-RESIDENT MRAM image, not the freshly
	         built one, and JLinkExe will not warn you. Rebuild with the
	         ITCM retarget (see docs/aen-bench-bringup.md, Flow C) or use
	         Flow D (scripts/bench/aen/flash-jlink-mramxip.sh).
	EOF
	exit 5
fi
# Positive plausibility check: Flow C can only legitimately land at ITCM
# 0x0, the ITCM global alias (0x50000000 / 0x58000000), or SRAM
# (0x02xxxxxx). Anything else -- notably a DTCM address like 0x2000xxxx --
# means we picked the wrong LOAD segment (e.g. a .bss/data segment instead
# of the code segment) and would splat the image over live RAM if we loaded
# it. This is the check that would have caught the original bug.
if (( BASE_RAW != 0x0 && BASE_RAW != 0x50000000 && BASE_RAW != 0x58000000 &&
      (BASE_RAW < 0x02000000 || BASE_RAW >= 0x03000000) )); then
	cat >&2 <<-EOF
	ram-run: REFUSING to load -- derived base $BASE for '$ELF' is not a
	         plausible Flow C target (expected 0x0, the ITCM global alias
	         0x50000000/0x58000000, or SRAM 0x02xxxxxx). This looks like a
	         DTCM/data LOAD segment was picked instead of the code segment,
	         which would corrupt live RAM (the resident app's stack/bss) if
	         loaded. Check 'readelf -l $ELF' by hand before proceeding.
	EOF
	exit 6
fi

# SAFETY GATE -- confirm the AEN E8 is on the other end BEFORE loadbin+go.
#
# Flow C is not a read: it writes an AEN-linked image into ITCM and executes
# it. Two probes on this bench share OEM serial 603000869, and JLinkExe has
# no USB-path selector, so JLINK_SN cannot disambiguate them -- landing this
# on the GD32 probe would execute foreign code on a DIFFERENT board, held
# under a different reservation that this one does not cover (alp-sdk#1312).
#
# Read-only connect first; nothing is written until the DP ID matches. The
# MRAM writers have had this gate since #1069; Flow C -- the flow people run
# most often -- did not.
cat > /tmp/ram-run-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/ram-run-preflight.jlink \
  > /tmp/ram-run-preflight.out 2>&1 || true
bench_jlink_assert_connected /tmp/ram-run-preflight.out "RAM-run preflight" || exit 7
bench_jlink_assert_aen_dpidr /tmp/ram-run-preflight.out "RAM-run preflight" || exit 4

SCRIPT=$(mktemp /tmp/jlink.XXXX.jlink)
{
  echo connect
  echo halt
  [ -n "$PRELOAD" ] && cat "$PRELOAD"
  echo "loadbin $BIN $BASE"
  echo "setpc $ENTRY"
  echo go
  echo "Sleep $SLEEP"
  echo halt
  bench_jlink_mem8_chunks "$BUF" "$SIZE"
  echo qc
} > "$SCRIPT"
echo ">>> RAM-run $(basename "$BD")  entry=$ENTRY  base=$BASE  ram_console_buf=$BUF  sleep=${SLEEP}ms" >&2
"${JLINK_ARGS[@]}" -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript "$SCRIPT" 2>/tmp/jlink.err > /tmp/jlink.out || true
# JLinkExe exits 0 even when it never opened the probe, so the `|| true` above
# cannot be relied on. Without this the decoder below prints an EMPTY console
# block for a pure infrastructure failure, which reads as a crashed app
# (alp-sdk#1318). Fail before decoding, not after.
bench_jlink_assert_connected /tmp/jlink.out "RAM-run $(basename "$BD")" || { rm -f "$SCRIPT"; exit 7; }
echo "----- RAM console (decoded) -----"
# Decode the 'ADDR = HH HH ...' mem8 lines into ASCII; stop at first NUL run.
awk '
/^[0-9A-Fa-f]+ = / {
  for (i=3; i<=NF; i++) {
    if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue
    b = strtonum("0x" $i)
    if (b == 0) { nul++; if (nul > 4) exit; next }
    nul = 0
    if (b == 10 || b == 13) { printf "\n"; continue }
    if (b >= 32 && b < 127) printf "%c", b
  }
}' /tmp/jlink.out
echo
echo "---------------------------------"
rm -f "$SCRIPT"
