#!/usr/bin/env bash
# scripts/bench/aen/flash-update-log-firewall-probe.sh [--package-only] [--replace-atoc] <he-probe-build-dir>
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives the Alif SETOOLS + JLinkExe over the labgrid-held AEN bench).
# Runs under WSL2 on Windows. See docs/aen-bench-bringup.md.
#
# Build and optionally flash the HE direct-write MRAM firewall probe for
# examples/connectivity/firmware-update-log. The default package is app-only so
# it preserves the board's existing DEVICE policy (SETOOLS keeps DEVICE when a
# JSON omits it, docs/aen-provisioning.md section 4). Set
# ALP_AEN_INCLUDE_DEVICE_CONFIG=yes only when intentionally replacing that
# policy; set ALP_AEN_DEVICE_CONFIG_JSON to a config filename under the SETOOLS
# build/config directory when using a board-specific policy. Every OTHER
# resident app entry NOT named HE-PROBE is a different matter: the `loadbin`
# below writes the SAME signed ATOC structure `app-write-mram -p` would
# (docs/debugging-aen.md), which REPLACES rather than merges, so a foreign app
# entry (e.g. an A32 Linux boot chain) is silently delisted unless
# --replace-atoc is passed (alp-sdk#2025 -- see the GUARD before the write,
# below). The probe is destructive when the firewall is absent: the helper
# records the first 16 bytes of alp_ulog_partition, lets HE try to overwrite
# them, then reports failure if the SWD post-read differs from the baseline.
set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# shellcheck source=scripts/bench/aen/bench-env.sh
source "$HERE/bench-env.sh"

PACKAGE_ONLY=0
REPLACE_ATOC=0
while [ $# -gt 0 ]; do
	case "$1" in
	--package-only) PACKAGE_ONLY=1; shift ;;
	--replace-atoc) REPLACE_ATOC=1; shift ;;
	--) shift; break ;;
	-*) echo "unknown flag: $1" >&2; exit 2 ;;
	*) break ;;
	esac
done

if [ "$#" -ne 1 ]; then
	echo "usage: $0 [--package-only] [--replace-atoc] <he-probe-build-dir>" >&2
	exit 2
fi

HE_BD="$(cd "$1" && pwd)"
HE_BIN="$HE_BD/zephyr/zephyr.bin"
HE_DTS="$HE_BD/zephyr/zephyr.dts"

[ -f "$HE_BIN" ] || { echo "missing HE zephyr.bin: $HE_BIN" >&2; exit 2; }
grep -q '^CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_FIREWALL_PROBE=y' "$HE_BD/zephyr/.config" || {
	echo "HE build is not the firewall-probe profile" >&2
	exit 2
}

bench_require_setools || exit $?
SET="$SETOOLS_DIR"
# Routed through bench_jlink_run (bench-env.sh, alp-sdk#2064): masks every
# OTHER probe out of a private namespace so -SelectEmuBySN resolves
# unambiguously to the ONE probe LG_PLACE actually owns.
JLINK_ARGS=(bench_jlink_run)

ULOG_OFFSET=$(awk '
	/alp_ulog_partition:/ { in_node = 1 }
	in_node && /reg = </ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^0x[0-9a-fA-F]+$/) {
				print $i
				exit
			}
		}
	}
	in_node && /};/ { in_node = 0 }
' "$HE_DTS")
[ -z "$ULOG_OFFSET" ] && { echo "could not parse alp_ulog_partition offset from $HE_DTS" >&2; exit 2; }
ULOG_ADDR=$(printf '0x%08X' $(($ULOG_OFFSET + 0x80000000)))

read_ulog_words() {
	local out="$1"
	cat > /tmp/firmware-update-log-probe-read-mram.jlink <<EOF
device $JLINK_DEVICE_READ
si SWD
speed $JLINK_SPEED
connect
mem32 $ULOG_ADDR, 0x4
exit
EOF
	"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-probe-read-mram.jlink \
		2>/tmp/firmware-update-log-probe-read-mram.err \
		>"/tmp/firmware-update-log-probe-read-mram-$out.out" || true
	awk -v addr="${ULOG_ADDR#0x}" 'toupper($1) == toupper(addr) && $2 == "=" {
		print toupper($3" "$4" "$5" "$6)
		exit
	}' "/tmp/firmware-update-log-probe-read-mram-$out.out"
}

rv=$(xxd -e -l 8 "$HE_BIN" | awk '{print $3}')
echo ">>> HE firewall-probe reset vector: 0x$rv" >&2
case "$rv" in
	000*) : ;;
	800*) echo "!! HE probe is MRAM-linked, not ITCM-loadable for the ATOC" >&2; exit 3 ;;
	*) echo "!! HE probe reset vector is unexpected for an ITCM-loaded AEN image" >&2; exit 3 ;;
esac

HE_IMG=firmware-update-log-he-firewall-probe.bin
cp -f "$HE_BIN" "$SET/build/images/$HE_IMG"
DEVICE_CONFIG_JSON="${ALP_AEN_DEVICE_CONFIG_JSON:-app-device-config.json}"

{
	echo "{"
	if [ "${ALP_AEN_INCLUDE_DEVICE_CONFIG:-no}" = "yes" ]; then
		printf '    "DEVICE":   { "disabled": false, "binary": "%s", "version": "0.5.00", "signed": true },\n' \
			"$DEVICE_CONFIG_JSON"
		echo ">>> including DEVICE config in firewall-probe ATOC: $DEVICE_CONFIG_JSON" >&2
	else
		echo ">>> app-only firewall-probe ATOC; preserving existing DEVICE/firewall policy" >&2
	fi
	cat <<JSON
    "HE-PROBE": { "disabled": false, "binary": "$HE_IMG", "version": "1.0.0", "signed": true,
                  "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON
} > "$SET/build/config/firmware-update-log-firewall-probe.json"

cd "$SET"
echo ">>> AEN firmware-update-log HE firewall-probe ATOC" >&2
./app-gen-toc -f build/config/firmware-update-log-firewall-probe.json >/tmp/firmware-update-log-firewall-probe-gentoc.log 2>&1 \
	|| { echo "gen-toc FAILED"; tail -20 /tmp/firmware-update-log-firewall-probe-gentoc.log; exit 1; }

PKG="$SET/build/AppTocPackage.bin"
ATOC_ADDR=$(awk '/APP Package Start Address:/{print $NF}' build/app-package-map.txt | tail -1)
[ -z "$ATOC_ADDR" ] && { echo "could not parse APP Package Start Address" >&2; exit 1; }
echo "    package: $PKG ($(stat -c%s "$PKG") B) -> MRAM $ATOC_ADDR" >&2

if [ "$PACKAGE_ONLY" -eq 1 ]; then
	echo "package-only: not flashing MRAM" >&2
	exit 0
fi

if [ "${ALP_CONFIRM_DESTRUCTIVE_FLASH:-}" != "yes" ]; then
	echo "refusing destructive MRAM flash: set ALP_CONFIRM_DESTRUCTIVE_FLASH=yes for this run" >&2
	exit 4
fi

# GUARD (alp-sdk#2025) -- see bench_atoc_replace_guard in bench-env.sh.
# HE-PROBE is what THIS run itself is about to (re)write, so it is the
# allowed set -- the guard fires only on a genuinely foreign resident entry
# (e.g. an A32 Linux boot chain), never on this script's own output. This
# helper has no other SE_UART dependency (its write goes over JLinkExe, not
# app-write-mram) -- the guard needs SE_UART only for its own read-only
# `maintenance -opt gettoc` query and reports "unverified" (abort unless
# --replace-atoc) if it is unset, same as any other missing input.
bench_atoc_replace_guard "$REPLACE_ATOC" flash-update-log-firewall-probe HE-PROBE || exit $?

BASELINE_WORDS=$(read_ulog_words before)
[ -z "$BASELINE_WORDS" ] && { echo "could not read pre-flash alp_ulog_partition words at $ULOG_ADDR" >&2; exit 2; }
echo "    alp_ulog baseline @ $ULOG_ADDR: $BASELINE_WORDS" >&2

# SAFETY GATE (alp-sdk#1312): this helper `loadbin`s a signed package to
# $ATOC_ADDR -- an MRAM write. Two probes on this bench share OEM serial
# 603000869 and JLinkExe has no USB-path selector, so confirm the SW-DP ID
# before writing. Read-only connect first.
cat > /tmp/fwprobe-preflight.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_READ
connect
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/fwprobe-preflight.jlink \
  > /tmp/fwprobe-preflight.out 2>&1 || true
bench_jlink_assert_connected /tmp/fwprobe-preflight.out "firewall-probe preflight" || exit 7
bench_jlink_assert_aen_dpidr /tmp/fwprobe-preflight.out "firewall-probe preflight" || exit 4

# SECTOR-PAD (alp-sdk#2233): the built-in loader rewrites the WHOLE 16 KiB
# sector(s) $PKG touches and never reads their prior contents first -- see
# bench-env.sh's Flow D section header. Read those sectors' current MRAM
# content and overlay $PKG on them; the padded image is what gets loadbin'ed.
FLOWD_SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/flowd-update-log-fwprobe-XXXXXX")" || exit 9
bench_flowd_prepare_write flash-update-log-firewall-probe "$FLOWD_SCRATCH" "$PKG:$ATOC_ADDR" || exit 9

# `verifybin` is deliberately GONE here (#2233): it only ever compared
# against J-Link's own in-process flash cache, never a fresh chip read -- see
# the bench_flowd_proof gate below, which runs BEFORE the boot script (#1526
# unchanged: a failed proof must still keep the board from booting an
# unverified image, which for THIS script means the HE probe never runs its
# destructive overwrite attempt on alp_ulog_partition).
#
# Computed into a variable BEFORE the heredoc (#2233 review item 13c): a
# `$(...)` inline inside a heredoc discards the command's own exit status.
FLOWD_LOADBIN_LINES="$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)" || {
	echo "!! bench_flowd_loadbin_lines failed for $FLOWD_MANIFEST -- refusing to write nothing." >&2
	exit 9
}
[ -n "$FLOWD_LOADBIN_LINES" ] || {
	echo "!! bench_flowd_loadbin_lines produced no loadbin line for $FLOWD_MANIFEST -- refusing." >&2
	exit 9
}
# RACE CHECK setup (#2233 review major 4) -- see bench-env.sh's "Pre-read ->
# write RACE detection" section.
FLOWD_PREWRITE_LINES="$(bench_flowd_prewrite_lines "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/prewrite")"
cat > /tmp/firmware-update-log-firewall-probe-write.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
$FLOWD_PREWRITE_LINES
$FLOWD_LOADBIN_LINES
exit
EOF

# FLOWD_DRY_RUN (#2233 review blocker 1a): exit right here, having printed
# what the write session WOULD run, WITHOUT ever invoking JLinkExe on it --
# and, since the boot CommandFile is separate (#1526), without reaching it.
if [ -n "$FLOWD_DRY_RUN" ]; then
	echo "--- DRY RUN: nothing written, no probe was opened (FLOWD_DRY_RUN) ---"
	cat /tmp/firmware-update-log-firewall-probe-write.jlink
	exit 10
fi

# Write the transcript FIRST, fully, then grep|head it for display (#1488
# finding 5) -- a `... | tee out | grep ... | head -N` pipeline lets `head`
# exit after N lines and SIGPIPE grep, which then closes tee's stdout pipe;
# tee can die from that SIGPIPE before JLinkExe's full transcript is written
# to disk, and the connect-failure check below depends on the FULL transcript.
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-firewall-probe-write.jlink \
	> /tmp/firmware-update-log-firewall-probe-write.out 2>&1 || true
grep -iE "could not connect|fail|error|Verify|O\\.K\\.|Reset|Writing|Programming" \
	/tmp/firmware-update-log-firewall-probe-write.out | head -40

if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" \
	/tmp/firmware-update-log-firewall-probe-write.out; then
	echo "!! $JLINK_DEVICE_FLASH profile failed to connect" >&2
	exit 2
fi

# RACE CHECK (#2233 review major 4).
if ! bench_flowd_check_race flash-update-log-firewall-probe "$FLOWD_SECTORS_FILE" "$FLOWD_SCRATCH/sectors" "$FLOWD_SCRATCH/prewrite"; then
	echo "!! RACE DETECTED -- restore from $FLOWD_SCRATCH/sectors and $FLOWD_SCRATCH/prewrite" >&2
	echo "   before trusting this board. The write HAS already happened (the boot has not --" >&2
	echo "   #1526 still gates that on the proof below)." >&2
	exit 11
fi

# This gate is LOAD-BEARING (#1526).  The CommanderScript above carries only
# `loadbin`; `RSetType 2` / `r` / `g` moved to a SECOND script that runs
# further down, and only if the check below passes.  So a failed proof now
# stops the board being reset into an image that did not verify -- and
# because the ATOC entry carries `"flags": ["load", "boot"]`, not booting is
# what keeps the HE probe from running and overwriting `alp_ulog_partition`.
#
# The MRAM write itself has of course already happened -- that is what
# `loadbin` is.  What is prevented is acting on it.
#
# GATE ON THE READ-BACK PROOF (#2233, replacing the old #1488 verifybin gate)
# -- a FRESH read-only J-Link session savebin's every padded range back and
# cmp's it byte-for-byte against the padded image, proving both that $PKG
# landed AND that its sector neighbours survived THIS write -- NOT a
# persistence proof across a power cycle.
if ! bench_flowd_proof flash-update-log-firewall-probe "$FLOWD_MANIFEST" "$FLOWD_SCRATCH/postread"; then
	echo "!! READ-BACK PROOF FAILED -- MRAM does NOT match the padded image for $PKG @ $ATOC_ADDR." >&2
	echo "   Do not treat this board as flashed." >&2
	echo "   The board was NOT reset or booted (#1526): the reset/boot CommanderScript" >&2
	echo "   runs only past this gate, so the HE probe never ran and" >&2
	echo "   alp_ulog_partition is intact.  MRAM now holds an image that failed proof --" >&2
	echo "   reflash before booting this board." >&2
	exit 3
fi
echo "verify: read-back proof OK ($PKG @ $ATOC_ADDR, sector-padded; not a cold-cycle persistence proof)" >&2

# ONLY NOW reset into the image (#1526).  Separate CommanderScript so the boot
# is genuinely downstream of the verify result -- inside one script JLinkExe
# runs everything before the shell can read anything, which is what made the
# old gate advisory.
cat > /tmp/firmware-update-log-firewall-probe-boot.jlink <<EOF
si SWD
speed $JLINK_SPEED
device $JLINK_DEVICE_FLASH
connect
RSetType 2
r
g
exit
EOF
"${JLINK_ARGS[@]}" -nogui 1 -CommanderScript /tmp/firmware-update-log-firewall-probe-boot.jlink 	> /tmp/firmware-update-log-firewall-probe-boot.out 2>&1 || true
if grep -qiE "Could not connect to the target device|Cannot connect to the probe/programmer" 	/tmp/firmware-update-log-firewall-probe-boot.out; then
	echo "!! reset/boot script failed to connect -- image is verified in MRAM but the" >&2
	echo "   board was not booted; alp_ulog_partition is untouched." >&2
	exit 2
fi

echo "flash complete; reading firewall-probe beacon" >&2
sleep 3
ALP_AEN_FIREWALL_PROBE_BASELINE="$BASELINE_WORDS" \
	"$HERE/read-update-log-proof.sh" --expect-firewall-probe
