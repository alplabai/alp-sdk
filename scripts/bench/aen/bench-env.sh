# shellcheck shell=bash
# scripts/bench/aen/bench-env.sh
#
# Cross-platform scope: this env layer + the AEN bench helpers that
# source it are Linux-side bench tooling (J-Link CommanderScript +
# the Alif SETOOLS, both Linux binaries on this bench). Windows users
# run them via WSL2; macOS users have the J-Link tools but the Alif
# SETOOLS are Linux-only. There is no native PowerShell equivalent —
# the bench is physically Linux-attached. See docs/aen-bench-bringup.md.
#
# SHARED, SANITIZED env for the AEN803 (Alif Ensemble E8, M55-HE) bench
# flash/RAM-run helpers. SOURCE this (don't execute it):
#
#     source "$(dirname "$0")/bench-env.sh"
#
# Every host-specific value (workspace root, serial device, SETOOLS
# install, J-Link probe) is resolved here from the environment, with
# sensible repo-relative defaults where one exists. Override any of
# them by exporting the variable before invoking a helper, e.g.:
#
#     SE_UART=<your-serial-device> ./flash-run.sh "$BENCH_ROOT/build/aen-gpio-bench"
#
# NOTHING host-specific is hard-coded into the committed scripts — the
# originals lived outside the repo with absolute /home paths; this
# layer is the single place those values come from.
#
# EXCEPTION: AEN_DPIDR (below, "DP-ID safety gate") is a silicon-fixed,
# bench-verified identity constant for the wrong-board MRAM-write interlock,
# not host-specific config -- it is deliberately NOT environment-overridable,
# unlike everything else in this file. GD32_DPIDR and V2N_CM33_DPIDR sit in
# the same gate but keep the normal override (alp-sdk#1716 scoped the
# closure to AEN_DPIDR only; GD32_DPIDR is also not bench-verified, see the
# gate's own comment).

# --------------------------------------------------------------------
# Workspace + Zephyr
# --------------------------------------------------------------------

# BENCH_ROOT — where build outputs live. Derived from this file's own
# location (scripts/bench/aen/.. -> repo root); override to keep build dirs
# outside the tree.
#
# Deliberately does NOT shell out to `git rev-parse --show-toplevel`, which is
# what this used to do. In a worktree-isolated session the harness does not
# merely make that call FAIL -- it KILLS it, so the `2>/dev/null` and the
# empty-string fallback below never got a chance to run: sourcing this file
# aborted the caller with rc=128 and NO OUTPUT AT ALL.
#
# That is the exact silent-failure mode openocd-ram-run.sh's own SAFETY GATE
# comment exists to prevent, and it cost a bench cold-cycle on 2026-09-11
# before an operator worked it out and exported BENCH_ROOT by hand.
#
# The path derivation was already here as the fallback and is strictly better
# for this purpose: this file lives at a fixed depth inside the repo, so it is
# deterministic, needs no subprocess, and works in a worktree, a plain
# checkout, and an extracted archive alike.
if [ -z "${BENCH_ROOT:-}" ]; then
	_self="${BASH_SOURCE[0]:-$0}"
	BENCH_ROOT="$(cd "$(dirname "$_self")/../../.." && pwd)"
fi
export BENCH_ROOT

# ALP_SDK_DIR — the alp-sdk checkout. Same as BENCH_ROOT when you build
# in-tree; kept separate so a future split (build dir != sdk dir) is a
# one-line override.
export ALP_SDK_DIR="${ALP_SDK_DIR:-$BENCH_ROOT}"

# ZEPHYR_BASE — the pinned Zephyr 4.4.0 checkout. west usually exports
# this; resolve it via `west topdir` if unset. Left empty (caller's
# environment is authoritative) when neither is available.
if [ -z "${ZEPHYR_BASE:-}" ]; then
	_topdir="$(west topdir 2>/dev/null || true)"
	[ -n "$_topdir" ] && [ -d "$_topdir/zephyr" ] && ZEPHYR_BASE="$_topdir/zephyr"
fi
export ZEPHYR_BASE

# ZEPHYR_SDK_INSTALL_DIR — root of the Zephyr SDK (the GNU Arm
# arm-zephyr-eabi toolchain lives under
# $ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin). No default — the SDK
# install path is host-specific; export it before sourcing, or let the
# helper fall back to a PATH-resolved arm-zephyr-eabi-* (see
# bench_tool_prefix below).
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-}"

# HAL_ALIF_DIR — the hal_alif Zephyr module, passed to the build as an
# EXTRA_ZEPHYR_MODULE. Resolve it from the west manifest; this is the
# robust, host-agnostic way (the module path is wherever west placed
# it). TBD fallback: if `west list` can't resolve it, the caller MUST
# export HAL_ALIF_DIR — we do NOT invent a path.
if [ -z "${HAL_ALIF_DIR:-}" ]; then
	HAL_ALIF_DIR="$(west list -f '{abspath}' hal_alif 2>/dev/null | head -1 || true)"
fi
export HAL_ALIF_DIR
# If still empty, helpers that need it print: "HAL_ALIF_DIR unresolved
# (TBD) — run inside the west workspace or export HAL_ALIF_DIR".

# --------------------------------------------------------------------
# Board target (the bench default: AEN803 / E8 / M55-HE, RTSS-HE)
# --------------------------------------------------------------------
# Every module on the Alp Lab AEN bench farm is an E1M-AEN803, so this is
# the default build.sh uses unconditionally. Its own preflight (alp-sdk#2094)
# checks overlay and .conf files SEPARATELY -- exit 2 -- scoped to AEN
# board-qualified alp_e1m_*_rtss_h[ep] names (plus same-board near-misses),
# never every board-qualified file; AEN_BOARD still overrides (see build.sh).
export AEN_BOARD="${AEN_BOARD:-alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he}"

# --------------------------------------------------------------------
# LG_PLACE resolution (alp-sdk#2032)
# --------------------------------------------------------------------
# The maintainer's hard rule: never touch a bench connection by hand --
# every connection goes through labgrid, addressed by PLACE NAME. A
# stale device-path table (SE_UART/console swapped, a J-Link USB path
# that named a DIFFERENT board) caused the 2026-09-07 incident. LG_PLACE
# is therefore the PRIMARY input below: set it and the SE-UART, console
# and SWD probe values are resolved live from labgrid, never guessed.
#
# NO default place -- if LG_PLACE is unset we do not fall back to
# guessing any particular board. The raw SE_UART variable stays
# available as an explicit, WARNED escape hatch for genuinely
# off-labgrid work (erase-storage.sh documents such a case); LG_PLACE
# wins whenever both are set.
#
# LG_COORDINATOR has NO default either, and deliberately so: a labgrid
# coordinator address is bench-specific infrastructure, not a portable
# SDK default -- baking in any one real address here would mean this
# file, shipped in a public repo, always resolves against ONE bench's
# coordinator regardless of who runs it. Export the coordinator you
# actually use before setting LG_PLACE; bench_labgrid_resolve() below
# refuses outright (rather than silently trying labgrid-client's own
# 127.0.0.1:20408 fallback, which would just hang or resolve nothing on
# every other host) when LG_PLACE is set but this is not.
export LG_COORDINATOR="${LG_COORDINATOR:-}"

# bench_labgrid_show <place> — echo `labgrid-client -p <place> show`.
# Strips a trailing CR from every line: a real interactive run (pty)
# emits CRLF line endings (measured 2026-09-07; labgrid-client itself
# prints no ANSI/SGR codes, so this is the actual "real output doesn't
# match a synthesised fixture" hazard here), and an untouched CR breaks
# any `$`-anchored match downstream the same way SETOOLS' CRLF does
# elsewhere in this file.
bench_labgrid_show() {
	LG_COORDINATOR="$LG_COORDINATOR" labgrid-client -p "$1" show 2>/dev/null | tr -d '\r'
}

# bench_labgrid_resource_field <show-output> <resource-name> <key>
# Parse ONE resource block the way board-farm/bin/jlink-run.sh:34-50
# already does: anchor on the block's OWN header line
# (^(Acquired|Matching) resource '<name>'), then scan only the
# indented continuation lines that follow it until the next
# unindented line. A naive whole-output regex is wrong here -- the
# `matches:` list near the top of `show` repeats the same resource
# names, and without block-scoping a search for e.g. the swd path can
# latch onto that list instead of the resource's own params dict.
# <key> may be a top-level params key (host, port) or nested inside
# 'extra' (path) -- both sit on their own pformat line, so a per-line
# regex finds either without needing to track dict nesting depth.
bench_labgrid_resource_field() {
	local show_output="$1" resource="$2" key="$3"
	printf '%s\n' "$show_output" | python3 -c "
import re, sys
name, key = sys.argv[1], sys.argv[2]
lines = sys.stdin.read().splitlines()
inblk = False
for ln in lines:
    if re.match(r\"^(Acquired|Matching) resource '\" + re.escape(name) + r\"'\", ln):
        inblk = True
        continue
    if inblk:
        if ln and not ln[0].isspace():
            break
        m = re.search(r\"'\" + re.escape(key) + r\"':\s*'([^']*)'\", ln)
        if not m:
            m = re.search(r\"'\" + re.escape(key) + r\"':\s*([^,}]+)\", ln)
        if m:
            print(m.group(1).strip())
            break
" "$resource" "$key"
}

# bench_labgrid_resolve <place> — resolve SE_UART, LG_CONSOLE_DEV,
# LG_CONSOLE_HOST, LG_CONSOLE_PORT and LG_SWD_PATH from a live
# `labgrid-client -p <place> show`, after confirming the reservation is
# actually held BY US (show's place-level `acquired:` equals
# "$(hostname)/$(whoami)", the same identity labgrid-client itself uses) --
# a lapsed reservation, or one held by a DIFFERENT operator, fails loudly
# here rather than silently resolving whatever labgrid last remembered or
# driving a board this invocation does not hold (alp-sdk#2064: a non-empty,
# non-"None" `acquired:` alone is NOT enough -- it must name us, not just
# name someone). Returns non-zero on ANY resolution failure that matters to
# EVERY caller -- an unheld/wrong-operator reservation, or a missing `swd`
# resource. `seuart` is resolved when present but is NOT required to
# succeed (see the comment at its own check below): not every AEN place has
# one, and plenty of callers never touch it. Callers must treat a non-zero
# return as fatal, not fall back to a guess; callers that specifically need
# SE_UART must check it themselves once this returns (see flash-run.sh,
# bench_atoc_replace_guard).
bench_labgrid_resolve() {
	local place="$1" out acquired me se_path console_dev console_host console_port swd_path

	if ! command -v labgrid-client >/dev/null 2>&1; then
		echo "bench-env: LG_PLACE=$place set but 'labgrid-client' is not on PATH" >&2
		return 1
	fi
	if [ -z "${LG_COORDINATOR:-}" ]; then
		echo "bench-env: LG_PLACE=$place set but LG_COORDINATOR is unset -- there is no" >&2
		echo "           default coordinator. Export the address of the labgrid" >&2
		echo "           coordinator you actually use, e.g.:" >&2
		echo "               export LG_COORDINATOR=<host>:<port>" >&2
		return 1
	fi

	out="$(bench_labgrid_show "$place")"
	if [ -z "$out" ]; then
		echo "bench-env: 'labgrid-client -p $place show' returned nothing -- is" >&2
		echo "           LG_COORDINATOR ($LG_COORDINATOR) reachable and does the place exist?" >&2
		return 1
	fi

	acquired=$(printf '%s\n' "$out" | awk -F': ' '/^  acquired:/{print $2; exit}')
	if [ -z "$acquired" ] || [ "$acquired" = "None" ]; then
		echo "bench-env: LG_PLACE=$place is NOT acquired (reservation lapsed or never taken)." >&2
		echo "           Acquire it first: labgrid-client -p $place acquire" >&2
		return 1
	fi

	# WHO holds it, not just whether anyone does (alp-sdk#2064). labgrid
	# itself records a reservation as "<hostname>/<username>" (see
	# board-farm/bin/jlink-run.sh's identical `ME="$(hostname)/$(whoami)"`) --
	# match that exactly. A place held by another operator resolving
	# cleanly here would let this invocation drive THEIR board; that is
	# verbatim the failure #2064 exists to prevent.
	me="$(hostname)/$(whoami)"
	if [ "$acquired" != "$me" ]; then
		echo "bench-env: LG_PLACE=$place is held by '$acquired', not you ('$me') --" >&2
		echo "           refusing to drive a board someone else's reservation covers." >&2
		echo "           Acquire it yourself first: labgrid-client -p $place acquire" >&2
		return 1
	fi

	se_path=$(bench_labgrid_resource_field "$out" seuart path)
	console_dev=$(bench_labgrid_resource_field "$out" console path)
	console_host=$(bench_labgrid_resource_field "$out" console host)
	console_port=$(bench_labgrid_resource_field "$out" console port)
	[ "$console_port" = "None" ] && console_port=""
	swd_path=$(bench_labgrid_resource_field "$out" swd path)

	if [ -z "$swd_path" ]; then
		echo "bench-env: LG_PLACE=$place ($acquired) exports no 'swd' resource path" >&2
		return 1
	fi

	# seuart is OPTIONAL here, unlike swd above (alp-sdk#2064 bench
	# verification, on two AEN EVK bench units): not every AEN place has a physical
	# SE-UART -- those two export only 'console' and 'swd', no 'seuart' at
	# all -- and a J-Link-only flow (Flow C/D: ram-run.sh, reread.sh,
	# flash-jlink*.sh, ...) never touches SE_UART, so failing resolution
	# entirely over a resource that flow doesn't need made #2064's fix
	# unreachable on exactly the two boards where wrong-board risk is
	# highest (all three AEN places share DPIDR 0x4C013477 and OEM serial
	# 000603000869). Resolving SE_UART empty when the resource genuinely
	# doesn't exist is not a guess -- it accurately reports "this place has
	# none" -- and every script that DOES need it already refuses loudly on
	# its own when SE_UART comes back empty: flash-run.sh's and
	# flash-run-dualcore.sh's own `[ -z "${SE_UART:-}" ]` gate ahead of
	# `app-write-mram`, and bench_atoc_replace_guard's own "SE_UART is
	# unset -- cannot query the resident ATOC" abort. This mirrors how
	# LG_CONSOLE_DEV/HOST/PORT above have never been hard-required either.
	SE_UART="$se_path"
	LG_CONSOLE_DEV="$console_dev"
	LG_CONSOLE_HOST="$console_host"
	LG_CONSOLE_PORT="$console_port"
	LG_SWD_PATH="$swd_path"
	return 0
}

# --------------------------------------------------------------------
# Serial + SETOOLS (Flow A — production MRAM flash over the SE-UART)
# --------------------------------------------------------------------

# SE_UART — the FT232R SE-UART device SETOOLS' app-write-mram talks to.
#
# LG_PLACE is the primary input (see "LG_PLACE resolution" above):
# set it and SE_UART is resolved live from labgrid's `seuart` resource,
# never guessed from a device-path table. LG_PLACE wins when both are
# set -- a raw SE_UART is then IGNORED, not merged.
#
# A raw SE_UART with NO LG_PLACE remains a supported escape hatch for
# genuinely off-labgrid work (erase-storage.sh documents such a case),
# but it is warned: serial enumeration is host-specific (Linux
# /dev/ttyUSB*, macOS /dev/cu.usbserial-*, Windows COMx), ttyUSBn
# numbering is enumeration-order-assigned and NOT stable across a
# reboot/replug, and this bench carries three AEN boards whose SE-UART
# and app-console paths have been measured swapped in a stale table --
# the exact mistake behind the 2026-09-07 incident.
# BENCH_ENV_NO_PROBE — set by a caller that never touches a probe or the
# SE-UART (build.sh: a pure `west build` wrapper) to skip the eager LG_PLACE
# resolve below entirely. Without this, sourcing bench-env.sh with LG_PLACE
# exported (e.g. an operator's own shell profile, set for OTHER helpers in
# the same session) aborts a compile-only invocation on a labgrid/reservation
# problem that has nothing to do with compiling. Every helper that DOES
# touch SE_UART or a J-Link probe leaves this unset, so the abort below still
# applies to them.
if [ -n "${LG_PLACE:-}" ] && [ -z "${BENCH_ENV_NO_PROBE:-}" ]; then
	if [ -n "${SE_UART:-}" ]; then
		echo "bench-env: LG_PLACE=$LG_PLACE is set; ignoring the raw SE_UART=$SE_UART" >&2
		echo "           you also exported -- LG_PLACE wins, resolving live instead." >&2
	fi
	if ! bench_labgrid_resolve "$LG_PLACE"; then
		echo "bench-env: labgrid resolution FAILED for LG_PLACE=$LG_PLACE -- refusing to" >&2
		echo "           fall back to a raw/guessed device path. Fix the reservation or" >&2
		echo "           coordinator reachability and re-source this file." >&2
		return 1 2>/dev/null || exit 1
	fi
	export SE_UART LG_CONSOLE_DEV LG_CONSOLE_HOST LG_CONSOLE_PORT LG_SWD_PATH
elif [ -n "${SE_UART:-}" ]; then
	echo "bench-env: WARNING -- SE_UART=$SE_UART is set WITHOUT LG_PLACE." >&2
	echo "           This is the off-labgrid ESCAPE HATCH, not the normal path: raw" >&2
	echo "           device paths go stale, /dev/ttyUSBn is enumeration-ordered (not" >&2
	echo "           stable across a reboot/replug), and this bench's three AEN boards" >&2
	echo "           have had their SE-UART/app-console paths measured SWAPPED in a" >&2
	echo "           stale table -- the exact mistake behind the 2026-09-07 incident." >&2
	echo "           Prefer: export LG_PLACE=<labgrid-place-name>" >&2
	export SE_UART
else
	export SE_UART="${SE_UART:-}"
fi

# SETOOLS_DIR — the Alif Security Toolkit "app-release-exec-linux"
# directory (contains app-gen-toc, app-write-mram, build/). NO default
# and ERROR-IF-UNSET when a Flow A/D helper actually needs it: SETOOLS
# is LICENSE-GATED and is NOT redistributed by alp-sdk (see README.md).
# Obtain it from Alif and export SETOOLS_DIR before running Flow A/D.
export SETOOLS_DIR="${SETOOLS_DIR:-}"

# --------------------------------------------------------------------
# J-Link (reads/RAM-run = generic device; Flow D MRAM flash = part dev)
# --------------------------------------------------------------------

# JLINK_DEVICE_FLASH — the Alif PART-NUMBER device profile. ONLY this
# profile unlocks J-Link's built-in Alif MRAM loader (Flow D). On J-Link
# DLLs older than V9.46 it will NOT connect to a live/running secure core
# (use _READ for that); on V9.46+ it also connects to a live core
# (bench-confirmed 2026-06-16, docs/aen-bench-bringup.md:14), but _READ
# stays the documented device for Flows B/C regardless of DLL version.
export JLINK_DEVICE_FLASH="${JLINK_DEVICE_FLASH:-AE822FA0E5597LS0_M55_HE}"

# JLINK_DEVICE_READ — the GENERIC Cortex-M55 device used for every
# read/attach/RAM-run (it attaches to the live core; on a pre-V9.46
# J-Link DLL the part profile cannot re-halt the running SE-booted app).
export JLINK_DEVICE_READ="${JLINK_DEVICE_READ:-Cortex-M55}"

# JLINK_SPEED — SWD clock in kHz.
export JLINK_SPEED="${JLINK_SPEED:-4000}"

# JLINK_SN / JLINK_SERIAL — RETIRED (alp-sdk#2064). No longer exported and
# no longer consumed anywhere in this directory: on THIS bench multiple
# J-Links answer the SAME cloned OEM serial 000603000869 (the exact count
# enumerated has drifted before and is not repeated here), so a bare serial
# selector could never disambiguate them. Every JLinkExe invocation here now
# goes through bench_jlink_run() below, which resolves the probe's USB
# topology from LG_SWD_PATH (set by bench_labgrid_resolve() when LG_PLACE is
# acquired) and masks every OTHER probe out of a private mount namespace
# before selecting by serial -- at that point the shared serial is
# unambiguous because only one probe is visible. bench_jlink_run refuses
# rather than guessing when LG_SWD_PATH is not resolved; there is no
# off-labgrid single-probe fallback. This bench also keeps the DPIDR safety
# gate below for every helper that touches a target -- that gate answers a
# DIFFERENT question (which chip answered: AEN E8 vs GD32 vs V2N CM33), not
# which of the three physically-identical AEN boards a shared-serial probe
# belongs to, which is what the masking in bench_jlink_run alone proves.

# --------------------------------------------------------------------
# DP-ID safety gate (every helper that touches a target)
# --------------------------------------------------------------------
# This bench has multiple J-Links (the exact count has drifted before, see
# bench_jlink_run's own header comment above -- not repeated here). The AEN
# E8 and the GD32 bridge share the same cloned OEM serial 000603000869,
# differing only by USB path; the V2N CM33 DAP answers a different serial:
#
#   AEN E8        000603000869          the AEN board
#   GD32 bridge   000603000869 (clone)  the other board
#   V2N CM33 DAP  600107451             the other board
#
# JLinkExe selects ONLY by serial and has no USB-path selector, so JLINK_SN
# narrows probe choice but cannot prove which board is on the other end --
# for the cloned pair it is ambiguous by construction. The SW-DP IDR read on
# connect is the only working discriminator, which is why every helper that
# writes to or executes on a target checks it and aborts on a mismatch
# (alp-sdk#1312).
#
# AEN_DPIDR is BENCH-VERIFIED per docs/aen-bench-bringup.md. The V2N CM33
# was measured 2026-08-08 on the bench (`Found SW-DP with ID 0x6BA02477`,
# `Found Cortex-M33 r0p4`) -- note that core answers on SWD, NOT JTAG.
# GD32_DPIDR is NOT bench-verified: 0x0BE12477 is the only GD32 candidate
# on record but has NOT been measured on a GD32 with a probe attached
# (see #1369) -- treat it as unattested, not bench-verified, matching
# flash-jlink-mramxip.sh's "0b. SAFETY GATE" comment.
#
# That is a labelling defect, NOT a hole in the guard, and the difference
# matters to whoever reads this next.  bench_jlink_assert_aen_dpidr() below
# is fail-CLOSED on the POSITIVE AEN_DPIDR match: GD32_DPIDR and
# V2N_CM33_DPIDR only pick which board the abort message NAMES.  Land on a
# real GD32 with a wrong GD32_DPIDR and the first branch simply misses,
# then the `! grep -qi "$AEN_DPIDR"` branch aborts anyway -- with a bare
# "expected AEN E8 SW-DP IDR ... not seen" instead of "you are on the GD32
# bridge".  So do NOT weaken the AEN_DPIDR check to compensate.
#
# To close the gap: attach a probe to a GD32 bridge, connect, and read the
# reported SW-DP ID off the transcript -- the same evidence shape that
# settled V2N_CM33_DPIDR.  Note there is currently no J-Link path to the
# GD32 on this bench (2026-08-07), which is why it is still open.
#
# NOT operator-overridable (alp-sdk#1716) -- AEN_DPIDR ONLY. The issue
# scoped this narrowly: "I lean (a), scoped to AEN_DPIDR only ... Note
# GD32_DPIDR should keep its override -- bench-env.sh:148 records that
# 0x0BE12477 is not bench-verified." A pre-exported AEN_DPIDR winning here
# is exactly backwards for a wrong-board interlock: `export
# AEN_DPIDR=<whatever the wrong board answers>` before running any helper
# below would make bench_jlink_assert_aen_dpidr() accept that board
# silently, on 5 of its 6 call sites -- flash-jlink.sh, flash-jlink-hp.sh,
# flash-update-log-dual.sh, flash-update-log-firewall-probe.sh and
# ram-run.sh all resolve it from here alone. AEN_DPIDR is bench-verified
# (see above), so there is no legitimate reason for it to differ from the
# real AEN E8 constant; assign it unconditionally, not via a
# caller-visible default, so the same defense-in-depth applies at every
# call site with one change instead of five.
#
# GD32_DPIDR and V2N_CM33_DPIDR keep the `${VAR:-default}` shape used
# everywhere else in this file -- they are deliberately left
# operator-overridable, NOT closed. GD32_DPIDR in particular is NOT
# bench-verified (see above): nobody has measured a real GD32 with a probe
# attached, so hard-closing it would risk blocking a legitimate future
# recovery against whatever a real GD32 actually answers -- the opposite
# of a safety improvement. V2N_CM33_DPIDR is left open alongside it per
# the same "AEN_DPIDR only" scoping in the issue, even though it is itself
# bench-verified (see above). If a future part genuinely needs a
# different expected DPIDR, that is a new named constant for that part's
# own helper, not a way to override AEN_DPIDR.
AEN_DPIDR="4C013477"
export AEN_DPIDR
export GD32_DPIDR="${GD32_DPIDR:-0BE12477}"
export V2N_CM33_DPIDR="${V2N_CM33_DPIDR:-6BA02477}"

# bench_jlink_assert_aen_dpidr <preflight-output-file> <context>
# Abort unless the connect transcript proves the AEN E8 answered.
#
# Shared by the MRAM writers and by ram-run.sh. Flow C is NOT a read: it
# `loadbin`s an AEN-linked image into ITCM and `go`es. Landing that on the
# GD32 probe would execute foreign code on a DIFFERENT board, held under a
# different reservation that this one does not cover.
#
# Names the wrong board when it can, so the operator gets "you are on the
# CM33 DAP" rather than a bare mismatch.
bench_jlink_assert_aen_dpidr() {
	local out="$1" ctx="${2:-preflight}"
	local wrong=""
	if grep -qi "$GD32_DPIDR" "$out" 2>/dev/null; then
		wrong="the GD32 bridge (0x$GD32_DPIDR)"
	elif grep -qi "$V2N_CM33_DPIDR" "$out" 2>/dev/null; then
		wrong="the CM33 DAP (0x$V2N_CM33_DPIDR)"
	fi
	if [ -n "$wrong" ]; then
		echo "!! ABORT ($ctx): probe answered $wrong, NOT the AEN E8." >&2
		echo "   That is a DIFFERENT board on a different reservation," >&2
		echo "   which this one does not cover. Refusing to touch it." >&2
		echo "   Transcript: $out" >&2
		return 4
	fi
	if ! grep -qi "$AEN_DPIDR" "$out" 2>/dev/null; then
		echo "!! ABORT ($ctx): expected AEN E8 SW-DP IDR 0x$AEN_DPIDR, not seen." >&2
		echo "   Check wiring, or that LG_PLACE names the board you actually hold" >&2
		echo "   (export LG_PLACE=<labgrid place> -- see bench_jlink_run above)." >&2
		echo "   Transcript: $out" >&2
		return 4
	fi
	return 0
}

# --------------------------------------------------------------------
# Tool resolution helpers
# --------------------------------------------------------------------

# bench_tool_prefix — echo the arm-zephyr-eabi toolchain prefix, so a
# caller can run "${PFX}-nm", "${PFX}-readelf", "${PFX}-objdump". Uses
# ZEPHYR_SDK_INSTALL_DIR when set; otherwise falls back to a bare
# "arm-zephyr-eabi" resolved off PATH. Returns non-zero (and prints to
# stderr) if neither resolves.
bench_tool_prefix() {
	local pfx
	if [ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" ] &&
		[ -x "$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm" ]; then
		pfx="$ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi"
	elif command -v arm-zephyr-eabi-nm >/dev/null 2>&1; then
		pfx="arm-zephyr-eabi"
	else
		echo "bench-env: cannot resolve arm-zephyr-eabi toolchain — export ZEPHYR_SDK_INSTALL_DIR or put arm-zephyr-eabi-* on PATH" >&2
		return 1
	fi
	echo "$pfx"
}

# bench_jlink_exe — echo the JLink Commander binary name/path. Override
# with JLINK_EXE if your install uses a non-PATH location. The binary
# is "JLinkExe" on Linux/macOS (SEGGER J-Link software pack).
bench_jlink_exe() {
	local exe="${JLINK_EXE:-}"

	# WHICH JLinkExe YOU LAUNCH DECIDES WHICH DLL YOU GET, and a bare `JLinkExe`
	# picks the wrong one.  On alplab-gw `/usr/bin/JLinkExe` is a symlink to
	# /opt/SEGGER/JLink_V950/JLinkExe, so a bare name silently ran
	# `DLL version V9.50` for a whole session while a newer install sat unused --
	# every log said V9.50 even with the V9.74 directory on PATH.  Each JLinkExe
	# dlopen()s libjlinkarm from its OWN directory (no RUNPATH, and ldconfig has
	# no jlinkarm entry), so the binary is the choice, not the library path.
	#
	# This is a reproducibility hazard, not just cosmetics: the built-in Alif part
	# profile is documented to need DLL >= V9.50, and probe-capability results are
	# only comparable across runs on the same DLL.
	#
	# Prefer the newest versioned install; fall back to PATH.  Set JLINK_EXE to
	# pin a specific one deliberately (e.g. to reproduce an older result).
	# Search root is overridable and defaults under $HOME -- never a hardcoded
	# maintainer path (scripts/check_public_private.py enforces this).
	if [ -z "$exe" ]; then
		local cand
		for cand in "${ALP_JLINK_SEARCH_ROOT:-$HOME/segger-latest}"/JLink_Linux_V*_x86_64/JLinkExe; do
			[ -x "$cand" ] && exe="$cand"
		done
		[ -n "$exe" ] || exe="JLinkExe"
	fi

	if ! command -v "$exe" >/dev/null 2>&1 && [ ! -x "$exe" ]; then
		echo "bench-env: '$exe' not found — install the SEGGER J-Link software or set JLINK_EXE" >&2
		return 1
	fi
	echo "$exe"
}

# bench_jlink_run <JLinkExe args...> — run JLinkExe against ONLY the probe
# belonging to LG_PLACE (alp-sdk#2064). Drop-in replacement for invoking
# `$(bench_jlink_exe)` directly: callers build JLINK_ARGS=(bench_jlink_run)
# instead of JLINK_ARGS=("$JLINK") and call "${JLINK_ARGS[@]}" exactly as
# before.
#
# Multiple J-Link probes on this bench answer the SAME cloned OEM serial
# 000603000869 -- the exact count enumerated has drifted before (measured
# three and five on different days) and is not repeated here; treat "more
# than one" as the standing fact, not a specific number. JLinkExe selects
# ONLY by serial with no USB-path selector, so `-SelectEmuBySN` alone takes
# whichever same-serial probe it happens to enumerate first -- not
# necessarily the one on the board this invocation's labgrid reservation
# actually covers. The four parts below are ALL load-bearing; each was
# found only after a simpler theory failed on the real bench (alp-sdk#2064),
# and dropping any one silently reintroduces the wrong-board hazard:
#
#   1. -SelectEmuBySN is explicit and REQUIRED -- JLinkExe does not
#      auto-select an emulator even when exactly one is visible.
#   2. BOTH the /dev/bus/usb/<bus>/<dev> node AND the sysfs device directory
#      of every OTHER probe are `mount --bind`-masked out of view. Masking
#      only the usbfs node is not enough: the device still ENUMERATES from
#      sysfs, so JLINK_EMU_SelectByUSBSN finds the first match for the
#      (shared) serial there and never falls back to a working one.
#   3. --net and --ipc namespaces. When a probe is already open in one
#      process, a second JLinkExe does not even reach USB -- SEGGER shares
#      an in-use J-Link over loopback, so the second instance attaches to
#      THAT probe regardless of any USB masking. No amount of USB-node
#      masking substitutes for this.
#   4. `ip link set lo up` inside the fresh network namespace, VERIFIED via
#      `ip -o link show lo`'s UP flag rather than trusted blind -- a new
#      netns starts with loopback DOWN, the J-Link DLL segfaults on it, and
#      `ip link set` can itself fail silently (alp-sdk#2174); refuses (exit
#      9, same isolation-did-not-take family as the checks below) rather
#      than proceeding on an unconfirmed loopback.
#
# The mask is PROCESS-LOCAL (an unprivileged `unshare -rm`, no sudo needed):
# it exists only inside this invocation and vanishes with the process, even
# on SIGKILL, so two sessions targeting two different places can run
# concurrently without disturbing each other.
#
# REFUSES rather than guesses -- the stated #2064 expected behaviour --
# when it cannot establish which probe belongs to LG_PLACE: LG_SWD_PATH is
# unresolved (LG_PLACE unset, or bench_labgrid_resolve above failed), the
# sysfs path it named has since vanished, a sibling probe's own busnum/devnum
# can't be read (an incompletely masked bench is not a safe one to proceed
# on), or -- after masking -- more than one vendor-1366 device is still
# visible in sysfs (the mask did not fully take).
#
# PROBE-BRICK GUARD, ported from board-farm/bin/jlink-run.sh: these probes
# are CLONES and a SEGGER firmware-update write to one is unrecoverable.
# JLinkExe offers that update the moment it OPENS the probe, before any
# CommandFile/CommanderScript command runs, so `exec DisableAutoUpdateFW`
# is injected as the FIRST line of the caller's script (Commander opens the
# emulator LAZILY -- a script containing only this exec triggers no
# connection, confirmed by the reference this was ported from). Refuses
# (does not proceed) when the caller passed neither `-CommandFile` nor
# `-CommanderScript` -- there is then nowhere to inject the guard, and
# opening a probe without it is never acceptable.
#
# BENCH_JLINK_SYSFS_ROOT overrides the sysfs root (default
# /sys/bus/usb/devices) -- test-only, so a unit test can point this at a
# fake probe tree instead of the real one.
#
# BENCH_JLINK_RUN_DRY_RUN, if non-empty, computes JLINK_TARGET_NODE,
# JLINK_MASKS, JLINK_SYSMASKS and JLINK_SEL and prints them to stdout
# instead of masking/exec-ing anything -- test-only, so the mask/selection
# computation and both refusal paths can be asserted without a real USB
# topology, `unshare`, or JLinkExe. Never set this on a real bench host; it
# makes every call a safe no-op (nothing is opened or masked), not a
# shortcut past the guard.
#
# Kept SEPARATE from the DPIDR safety gate (bench_jlink_assert_aen_dpidr
# below) -- do not delete that gate as "now redundant". It answers a
# different question (which CHIP answered: AEN E8 vs GD32 vs V2N CM33), not
# which of the three physically-identical AEN boards a shared-serial probe
# belongs to -- only the USB-topology masking here answers that one.
bench_jlink_run() {
	local jlink port target_sn busnum devnum target_node sysfs_root dev_root
	local mask_args="" sysmask_args="" mask_empty p leaf vendor node pbus pdev sysreal
	local cmdfile="" prelude rc
	local -a argv newargs
	local i a

	sysfs_root="${BENCH_JLINK_SYSFS_ROOT:-/sys/bus/usb/devices}"
	# BENCH_JLINK_DEV_ROOT overrides where the usbfs device-node PATHS below
	# are computed from -- test-only, same reason as BENCH_JLINK_SYSFS_ROOT:
	# a unit test can point this at a fake tmp-dir tree of touched files
	# (mimicking /dev/bus/usb/<bus>/<dev>) and exercise real, non-empty mask
	# computation without CAP_MKNOD/root to create real device nodes. Default
	# is the real kernel path; production behaviour is unchanged.
	dev_root="${BENCH_JLINK_DEV_ROOT:-/dev/bus/usb}"

	jlink="$(bench_jlink_exe)" || return $?

	if [ -z "${LG_SWD_PATH:-}" ]; then
		echo "bench-env: bench_jlink_run: LG_SWD_PATH is unresolved -- export LG_PLACE=<labgrid" >&2
		echo "           place> (and LG_COORDINATOR) so the probe can be identified by USB" >&2
		echo "           topology. Refusing to guess which of the identically-serialised" >&2
		echo "           J-Link probes on this bench belongs to your reservation (alp-sdk#2064)." >&2
		return 9
	fi
	port="$LG_SWD_PATH"
	if [ ! -d "$sysfs_root/$port" ]; then
		echo "bench-env: bench_jlink_run: no USB device at sysfs path '$port' (from LG_SWD_PATH) --" >&2
		echo "           the probe may have been unplugged/re-enumerated since LG_PLACE was resolved." >&2
		return 9
	fi
	target_sn=$(cat "$sysfs_root/$port/serial" 2>/dev/null || true)
	if [ -z "$target_sn" ]; then
		echo "bench-env: bench_jlink_run: cannot read a serial for USB device '$port'" >&2
		return 9
	fi
	busnum=$(cat "$sysfs_root/$port/busnum" 2>/dev/null || true)
	devnum=$(cat "$sysfs_root/$port/devnum" 2>/dev/null || true)
	if [ -z "$busnum" ] || [ -z "$devnum" ]; then
		echo "bench-env: bench_jlink_run: cannot read busnum/devnum for '$port'" >&2
		return 9
	fi
	target_node=$(printf '%s/%03d/%03d' "$dev_root" "$busnum" "$devnum")

	# Probe-brick guard (see header): find the -CommandFile/-CommanderScript
	# argument and rewrite it to point at a copy with the DisableAutoUpdateFW
	# prelude prepended. Refuse if neither flag is present.
	#
	# NOT guarded: a SECOND -CommandFile/-CommanderScript in one argv would
	# overwrite $prelude and leak the first mktemp (only the last one gets
	# cleaned up below). No caller in this repo ever passes more than one --
	# JLinkExe itself only honours the last anyway -- so this is left as a
	# known, inert edge case rather than added complexity for a shape
	# nothing here produces.
	argv=("$@")
	newargs=()
	i=0
	while [ $i -lt ${#argv[@]} ]; do
		a="${argv[$i]}"
		case "$a" in
		-CommandFile | -CommanderScript)
			if [ $((i + 1)) -lt ${#argv[@]} ]; then
				cmdfile="${argv[$((i + 1))]}"
				# DEFENSE IN DEPTH (alp-sdk#2233 review blocker 1b): FLOWD_DRY_RUN's
				# whole promise is that NO probe is ever opened for a write. Every
				# Flow D writer's own dry-run exit (right after it prints the
				# CommandFile it WOULD run) is the primary guard; this is the
				# backstop for the case that guard is missing, deleted, or wrong --
				# refuse HERE, before the probe-brick prelude is even built, rather
				# than trust every caller got its own check right. Only `loadbin`
				# and `erase` are load-bearing (the two commands that write/erase
				# MRAM); `savebin`/`connect`/`h`/`r`/`g`/etc. are read-only or
				# boot-only and remain allowed under FLOWD_DRY_RUN (the sector
				# pre-read and proof read-back sessions are real reads even when
				# FLOWD_DRY_RUN is set for a WRITE elsewhere in the same run -- see
				# bench_flowd_read_sectors' own dry-run branch, which never reaches
				# this function at all).
				if [ -n "${FLOWD_DRY_RUN:-}" ] && grep -qiE '^[[:space:]]*(loadbin|erase)\b' "$cmdfile" 2>/dev/null; then
					echo "bench-env: bench_jlink_run: REFUSING -- FLOWD_DRY_RUN is set and" >&2
					echo "           '$cmdfile' contains a loadbin/erase command. FLOWD_DRY_RUN" >&2
					echo "           promises NO probe is ever opened for a write (alp-sdk#2233) --" >&2
					echo "           this is the backstop behind each writer's own dry-run exit." >&2
					return 13
				fi
				prelude=$(mktemp "${TMPDIR:-/tmp}/bench-jlink-run.jlink.XXXXXX") || {
					echo "bench-env: bench_jlink_run: cannot create the DisableAutoUpdateFW prelude file" >&2
					return 10
				}
				# Check `cat`'s own exit status (the compound command's
				# status is its LAST command's) -- an unreadable/missing
				# $cmdfile must not silently hand JLinkExe a guard-only
				# script (just the exec line, no caller commands at all),
				# which would open the probe, do nothing, and report success.
				if ! { printf 'exec DisableAutoUpdateFW\n'; cat "$cmdfile"; } >"$prelude"; then
					echo "bench-env: bench_jlink_run: cannot read '$cmdfile' -- refusing to open" >&2
					echo "           a probe with a guard-only script (none of the caller's own" >&2
					echo "           commands would run)." >&2
					rm -f "$prelude"
					return 10
				fi
				newargs+=("$a" "$prelude")
				i=$((i + 2))
				continue
			fi
			;;
		esac
		newargs+=("$a")
		i=$((i + 1))
	done
	if [ -z "$cmdfile" ]; then
		echo "bench-env: bench_jlink_run: refusing -- no -CommandFile/-CommanderScript in the" >&2
		echo "           arguments, so 'exec DisableAutoUpdateFW' cannot be injected ahead of" >&2
		echo "           it. These probes are CLONES and a SEGGER firmware-update write to one" >&2
		echo "           is unrecoverable -- never open a probe without this suppression." >&2
		return 10
	fi
	set -- "${newargs[@]}"

	echo "bench-env: bench_jlink_run: place=${LG_PLACE:-?} port=$port -> $target_node serial=$target_sn" >&2

	# Mask every OTHER SEGGER (USB vendor 1366) probe: usbfs node + sysfs
	# dir. A sibling probe whose busnum/devnum can't be read, or whose node
	# doesn't exist, means it CANNOT be masked -- abort rather than silently
	# proceed with a bench that still has it enumerable (that asymmetry
	# depended purely on enumeration order in the reference this was ported
	# from, and is exactly the hole the post-mask visible-count check below
	# also guards).
	for p in "$sysfs_root"/*-*; do
		leaf="${p##*/}"
		case "$leaf" in *:*) continue ;; esac
		vendor="$(cat "$p/idVendor" 2>/dev/null || true)"
		[ "$vendor" = "1366" ] || continue
		[ "$leaf" = "$port" ] && continue
		pbus="$(cat "$p/busnum" 2>/dev/null || true)"
		pdev="$(cat "$p/devnum" 2>/dev/null || true)"
		if [ -z "$pbus" ] || [ -z "$pdev" ]; then
			echo "bench-env: bench_jlink_run: cannot read busnum/devnum for sibling probe '$leaf' --" >&2
			echo "           refusing to proceed with a bench that can't be fully masked." >&2
			rm -f "$prelude"
			return 9
		fi
		node=$(printf '%s/%03d/%03d' "$dev_root" "$pbus" "$pdev")
		if [ ! -e "$node" ]; then
			echo "bench-env: bench_jlink_run: sibling probe '$leaf' resolved to '$node', which" >&2
			echo "           does not exist -- refusing to proceed with a bench that can't be" >&2
			echo "           fully masked." >&2
			rm -f "$prelude"
			return 9
		fi
		sysreal=$(readlink -f "$p")
		mask_args="$mask_args $node"
		sysmask_args="$sysmask_args $sysreal"
		echo "bench-env: bench_jlink_run: masking sibling probe $leaf ($node)" >&2
	done
	mask_empty=$(mktemp -d) || {
		rm -f "$prelude"
		return 9
	}

	if [ -n "${BENCH_JLINK_RUN_DRY_RUN:-}" ]; then
		echo "JLINK_TARGET_NODE=$target_node"
		echo "JLINK_MASKS=$mask_args"
		echo "JLINK_SYSMASKS=$sysmask_args"
		echo "JLINK_SEL=-SelectEmuBySN $target_sn"
		# Left behind deliberately (not rm'd) -- a test asserts the injected
		# prelude's CONTENT (the probe-brick guard, alp-sdk#2064 review
		# Blocker 1) without needing a real char device to reach the real
		# unshare/exec path below. $TMPDIR is a test's own sandboxed tmp_path
		# in every caller of this branch; nothing leaks on a real bench host
		# because BENCH_JLINK_RUN_DRY_RUN is never set on one.
		echo "JLINK_PRELUDE=$prelude"
		rmdir "$mask_empty" 2>/dev/null || true
		return 0
	fi

	JLINK_BIN="$jlink" JLINK_TARGET_NODE="$target_node" JLINK_MASKS="$mask_args" \
		JLINK_SYSMASKS="$sysmask_args" JLINK_EMPTY="$mask_empty" JLINK_SEL="-SelectEmuBySN $target_sn" \
		JLINK_SYSFS_ROOT="$sysfs_root" \
		unshare -rm --net --ipc --propagation private /bin/bash -c '
			set -e
			ip link set lo up 2>/dev/null || true
			# alp-sdk#2174: the line above was failure-tolerant, so a
			# netns where lo never came up (permission, race, ...) fell
			# through to JLinkExe with a still-down loopback and it
			# segfaulted. Verified via the UP flag word `ip -o link show lo`
			# prints -- NOT /sys/class/net/lo/flags, measured stale here
			# without an explicit remount; `ip` goes over netlink, always correct.
			#
			# `command -v ip` is checked FIRST, explicitly: under `set -e`
			# above, `lostate=$(ip ...)` alone would abort the whole
			# subshell with a bare, unexplained rc=127 the moment `ip` is
			# missing -- the command substitution assignment fails on its
			# own and trips `set -e` before the "did not come up" message
			# right below ever gets a chance to print (measured).
			command -v ip >/dev/null 2>&1 || {
				echo "bench_jlink_run: ip not found in the fresh netns -- cannot verify loopback state" >&2
				exit 9
			}
			lostate=$(ip -o link show lo 2>/dev/null)
			printf "%s\n" "$lostate" | tr ",<>" "\n\n\n" | grep -qx UP || {
				echo "bench_jlink_run: loopback did not come up in the fresh netns ($lostate)" >&2
				exit 9
			}
			for n in $JLINK_MASKS; do mount --bind /dev/null "$n"; done
			for s in $JLINK_SYSMASKS; do mount --bind "$JLINK_EMPTY" "$s"; done
			[ -c "$JLINK_TARGET_NODE" ] || {
				echo "bench_jlink_run: target node $JLINK_TARGET_NODE vanished under the mask" >&2
				exit 9
			}
			# Prove the mask actually took, before launching anything. /dev/null
			# is a char device too, so "is it a chardev" alone proves nothing --
			# the decisive check is the SAME major:minor as /dev/null.
			nulldev=$(stat -c "%t:%T" /dev/null)
			for n in $JLINK_MASKS; do
				[ "$(stat -c "%t:%T" "$n")" = "$nulldev" ] || {
					echo "bench_jlink_run: mask did NOT take on $n" >&2
					exit 9
				}
			done
			# And the decisive one: exactly one J-Link may remain visible in
			# sysfs, because that is what -SelectEmuBySN enumerates.
			vis=0
			for p in "$JLINK_SYSFS_ROOT"/*-*; do
				case "${p##*/}" in *:*) continue ;; esac
				[ "$(cat "$p/idVendor" 2>/dev/null || true)" = "1366" ] && vis=$((vis + 1))
			done
			[ "$vis" = "1" ] || {
				echo "bench_jlink_run: $vis J-Links still enumerate in sysfs, expected 1 -- -SelectEmuBySN would be ambiguous" >&2
				exit 9
			}
			exec "$JLINK_BIN" $JLINK_SEL "$@"
		' -- "$@"
	rc=$?
	rm -f "$prelude"
	rm -rf "$mask_empty"
	return $rc
}

# bench_jlink_assert_connected <jlink-output-file> [context] — fail when
# JLinkExe never reached the probe.
#
# JLinkExe exits 0 even when it could not open the probe at all: every
# command in the CommanderScript prints
#
#     J-Link connection not established yet but required for command.
#     Connecting to J-Link ...FAILED: Cannot connect to the probe/programmer.
#
# and the run still ends "Script processing completed." Callers that pipe
# that output through a decoder therefore render a total infrastructure
# failure as EMPTY app output -- which reads as "the app crashed" when no
# app was ever loaded (alp-sdk#1318). Call this on the captured output
# before decoding anything out of it.
#
# Deliberately NOT the same check as the DPIDR preflight gate in
# flash-jlink*.sh: that gate answers "is this the right board", this one
# answers "did we reach any board at all". An MRAM write needs both; a
# read-only path needs this one.
bench_jlink_assert_connected() {
	local out="$1" ctx="${2:-J-Link}"
		# 'Could not connect to' is deliberately left OPEN-ENDED.  JLinkExe prints
	# "Could not connect to the target device." -- the old pattern demanded
	# "Could not connect to target" (no "the ... device"), so it NEVER matched,
	# and because the log still contained a `J-Link>` prompt the guard passed a
	# session that had connected to nothing.  A read-back from that session
	# decoded to an EMPTY RAM console and was rendered as app output -- exactly
	# the alp-sdk#1318 failure this function exists to catch.  Bench-measured
	# again 2026-09-04.
	local pat='Cannot connect to the probe/programmer|Failed to connect to target|Could not connect to|No J-Link device found'
	if [ ! -s "$out" ]; then
		echo "bench-env: $ctx produced no J-Link output at all ('$out' missing or empty)." >&2
		return 7
	fi
	if grep -qiE "$pat" "$out"; then
		echo "bench-env: $ctx could NOT connect to the J-Link probe -- nothing was read" >&2
		echo "           from the target, so any decoded output would be empty for an" >&2
		echo "           infrastructure reason, not because the app was silent." >&2
		echo >&2
		grep -iE "$pat" "$out" | head -3 >&2
		echo >&2
		echo "           alplab-gw has multiple J-Link probes attached and several share a" >&2
		echo "           cloned OEM serial, so an unselected/unmasked JLinkExe can fail" >&2
		echo "           outright or attach the wrong board. Every helper here already routes" >&2
		echo "           through bench_jlink_run() (bench-env.sh, alp-sdk#2064), which resolves" >&2
		echo "           the probe from LG_PLACE and masks every other one -- confirm LG_PLACE" >&2
		echo "           is exported and its reservation is held by you:" >&2
		echo "               export LG_PLACE=<labgrid place>" >&2
		echo "               labgrid-client -p <place> show      # acquired: must be you" >&2
		echo "           Full J-Link transcript: $out" >&2
		return 7
	fi
	# POSITIVE evidence, not just the absence of a known negative (alp-sdk#1551).
	# The checks above enumerate failure strings, so any failure mode that stops
	# JLinkExe BEFORE it prints one of them passed. Measured case: the alp-sdk#1478
	# stray literal `n` made JLinkExe reject its own command line and exit, giving
	# this COMPLETE 147-byte transcript -- no connect attempted, no DP read:
	#
	#     SEGGER J-Link Commander V9.46 (Compiled May 27 2026 12:24:58)
	#     DLL version V9.46, compiled May 27 2026 12:23:54
	#
	#     Unknown command line option n.
	#
	# It contains none of the strings above, so this function returned 0 on it.
	# Only the sibling DPIDR gate caught that run, and the read-back-only paths
	# (the post-flash console dumps, e.g. flash-jlink.sh step 4 and
	# flash-run.sh's Flow A read) have no DPIDR gate on the READ ITSELF --
	# they rely on an earlier preflight DPIDR check in the same script run, so
	# for the read's own transcript this function is the sole check. (reread.sh
	# used to be in this list too; alp-sdk#813 gave it its own preflight.)
	#
	# The marker is the `J-Link>` command prompt: JLinkExe echoes it for every
	# CommanderScript line it executes, so its presence proves the script ran at
	# all. Deliberately NOT "Connecting to J-Link ...O.K." -- that is the stronger
	# signal but it is absent from trimmed transcripts (the alp-sdk#1318 REAL_GOOD_READ
	# fixture starts at the first `J-Link>` line), and this guard must not reject a
	# genuine read-back just because its capture began mid-run.
	#
	# ponytail: prompt-presence only, which cannot tell "connected" from "ran but
	# reached no target" -- that is bench_jlink_assert_aen_dpidr's job. Tighten to
	# the O.K. line if a failure mode ever slips past this that still prints a prompt.
	if ! grep -q 'J-Link>' "$out"; then
		echo "bench-env: $ctx produced J-Link output containing no 'J-Link>' command" >&2
		echo "           prompt, so the CommanderScript never executed -- JLinkExe" >&2
		echo "           exited before running it. Nothing was read from the target;" >&2
		echo "           any decoded output is empty for an infrastructure reason." >&2
		echo >&2
		head -5 "$out" >&2
		echo >&2
		if grep -q 'bench_jlink_run:' "$out" 2>/dev/null; then
			echo "           bench_jlink_run() itself refused before opening any probe (see its" >&2
			echo "           own message above) -- this is NOT alp-sdk#1478's rejected-command-" >&2
			echo "           line case below; JLinkExe never even ran." >&2
		else
			echo "           A rejected command line does this (see alp-sdk#1478: a stray" >&2
			echo "           argument gave 'Unknown command line option n.' and nothing else)." >&2
		fi
		echo "           Full J-Link transcript: $out" >&2
		return 7
	fi
	return 0
}

# bench_require_setools — guard for Flow A/D. Errors (exit 2) if
# SETOOLS_DIR is unset or doesn't look like a SETOOLS install. SETOOLS
# is license-gated and NOT shipped with alp-sdk; this is the single
# enforcement point.
bench_require_setools() {
	if [ -z "${SETOOLS_DIR:-}" ]; then
		echo "bench-env: SETOOLS_DIR is unset. The Alif Security Toolkit is" >&2
		echo "           license-gated and is NOT redistributed by alp-sdk." >&2
		echo "           Obtain it from Alif and: export SETOOLS_DIR=<...>/app-release-exec-linux" >&2
		return 2
	fi
	if [ ! -x "$SETOOLS_DIR/app-gen-toc" ]; then
		echo "bench-env: '$SETOOLS_DIR' does not look like a SETOOLS app-release-exec-linux dir (no app-gen-toc)" >&2
		return 2
	fi
	return 0
}

# --------------------------------------------------------------------
# OpenOCD (M55-HE core selection -- see scripts/bench/aen/openocd-ram-run.sh)
# --------------------------------------------------------------------

# AEN_OPENOCD_CFG -- the board-farm's shared SWD config for this bench. It
# declares BOTH E8 M55 cores as separate CoreSight-AP OpenOCD targets
# (alif.m55he @ AP 0x00300000, alif.m55hp @ AP 0x00200000, HP created last so
# it stays OpenOCD's default/current target -- bench-verified 2026-09-09,
# alp-sdk#2037). NO default: like SE_UART/SETOOLS_DIR above, it lives outside
# this repo on the bench host. Export it before running openocd-ram-run.sh,
# e.g. AEN_OPENOCD_CFG=<board-farm>/debug/openocd-alif-e8-swd.cfg.
export AEN_OPENOCD_CFG="${AEN_OPENOCD_CFG:-}"

# AEN_OPENOCD_USB_LOCATION -- the labgrid-pinned USB path for the AEN E8's
# J-Link (see the DP-ID safety gate comment above for why a bare serial can't
# disambiguate the three same-serial probes on this bench -- all three E8s
# answer the same SW-DP 0x4c013477, so the USB path is the ONLY thing that
# selects which physical board you talk to; OpenOCD, unlike JLinkExe, CAN
# select by USB path). Resolve it per-board from
# `labgrid-client -p <place> show`'s swd resource -- NEVER hardcode a path
# here or in the shared config (that file's own header says so: it is loaded
# by labgrid's OpenOCDDriver, which supplies this itself when driving the
# board for real; a by-hand run on the exporter is expected to prepend the
# flag on the command line instead of editing it in). NO default:
# host-specific, like SE_UART/AEN_OPENOCD_CFG above.
export AEN_OPENOCD_USB_LOCATION="${AEN_OPENOCD_USB_LOCATION:-}"

# bench_require_openocd [cfg-override] — guard for openocd-ram-run.sh. Errors
# (exit 2) if the resolved config path (arg, else AEN_OPENOCD_CFG) is unset
# or missing, if AEN_OPENOCD_USB_LOCATION is unset (the labgrid-pinned probe
# path -- with three same-serial J-Links on this bench, an unpinned OpenOCD
# run picks one arbitrarily, see the DP-ID safety gate comment above), or if
# the `openocd` binary itself is not on PATH. Same enforcement shape as
# bench_require_setools above -- fail CLOSED, not a warning.
bench_require_openocd() {
	local cfg="${1:-${AEN_OPENOCD_CFG:-}}"
	if [ -z "$cfg" ]; then
		echo "bench-env: AEN_OPENOCD_CFG is unset. This is the board-farm's" >&2
		echo "           shared SWD config (outside this repo, host-specific)." >&2
		echo "           export AEN_OPENOCD_CFG=<board-farm>/debug/openocd-alif-e8-swd.cfg" >&2
		return 2
	fi
	if [ ! -f "$cfg" ]; then
		echo "bench-env: AEN_OPENOCD_CFG='$cfg' does not exist." >&2
		return 2
	fi
	if [ -z "${AEN_OPENOCD_USB_LOCATION:-}" ]; then
		echo "bench-env: AEN_OPENOCD_USB_LOCATION is unset. This bench has THREE" >&2
		echo "           J-Links and two share a cloned OEM serial -- with no USB" >&2
		echo "           path pinned, OpenOCD picks a probe arbitrarily, and its" >&2
		echo "           first actions are halt + load_image (alp-sdk#2037)." >&2
		echo "           All three E8 boards answer the same SW-DP 0x4c013477," >&2
		echo "           so the USB path is the ONLY thing that selects the" >&2
		echo "           board -- resolve YOUR board's from:" >&2
		echo "               labgrid-client -p <your-bench-place> show" >&2
		echo "           (the swd resource's USB path) and export exactly that" >&2
		echo "           value, e.g.:" >&2
		echo "               export AEN_OPENOCD_USB_LOCATION=<path from the show above>" >&2
		return 2
	fi
	if ! command -v openocd >/dev/null 2>&1; then
		echo "bench-env: 'openocd' not found on PATH -- install OpenOCD." >&2
		return 2
	fi
	return 0
}

# bench_atoc_replace_guard <replace-atoc 0|1> <tag> [allowed-entry ...]
#
# GUARD (alp-sdk#2025) against the ATOC-replace hazard, shared by every
# helper that burns a freshly-generated `app-gen-toc` package: that package
# always contains exactly the app entries named in the JSON handed to it, and
# writing it -- whether over the SE-UART (`app-write-mram -c $SE_UART -p`,
# Flow A) or directly over SWD (`loadbin $PKG $ATOC_ADDR`, Flow D) -- burns
# the SAME signed ATOC structure at the SAME MRAM location either way (see
# docs/debugging-aen.md and docs/aen-bench-bringup.md: both call it "the
# signed ATOC the SE reads at boot"). `DEVICE` is the one entry SETOOLS is
# documented to preserve when a JSON omits it (docs/aen-provisioning.md
# section 4, "write an app-only ATOC ... don't overwrite the device config");
# every OTHER resident app entry NOT in the JSON you are about to burn is
# gone the instant the write lands -- no error, no SES warning (`[SES] ATOC
# ok` prints either way). This destroyed a live A32 Linux boot chain
# (`BOOTLOAD`/`A32_APP`/`HP_APP`/`HE_APP`) on an AEN EVK bench unit, 2026-09-07.
#
# Originally written into flash-run.sh alone for its own single ALP-HE entry
# (#2025); factored out here so every script that commits a TOC shares one
# guard instead of drifting copies. <allowed-entry ...> is the set of app
# entries THIS run is itself about to (re)write -- e.g. flash-run.sh passes
# ALP-HE, flash-run-dualcore.sh passes ALP-HP ALP-HE (its own legitimate
# two-entry write) -- so the guard fires only on a GENUINELY foreign resident
# entry, never on the script's own output.
#
# Queries resident state with SETOOLS' `maintenance -c $SE_UART -opt gettoc`
# -- a documented non-destructive TOC read (AUGD0005 Alif Security Toolkit
# User Guide v1.110.0, "Command line options (-opt)": gettoc "Returns the TOC
# information"), never a merge engine. Dumps it unconditionally, even under
# replace-atoc=1, so a run always leaves a record of what was resident
# immediately before a destructive write.
#
# Returns 0 to proceed, 5 to abort (could not verify what is resident, or a
# foreign entry would be delisted) unless replace-atoc=1 was passed.
bench_atoc_replace_guard() {
	local replace_atoc="$1" tag="$2"
	shift 2
	local allowed=("$@")

	# ${TMPDIR:-/tmp}, not a bare /tmp literal, so a test (or a host with a
	# non-default TMPDIR) can sandbox this. `tag` is a literal script name
	# (flash-run, flash-run-dualcore, ...), NOT run-unique -- three AEN
	# boards on this farm makes two concurrent runs of the
	# SAME script against DIFFERENT boards a real scenario, and a fixed path
	# let run A's write land between run B's redirect and B's read, so B
	# parsed A's board (reproduced: B printed A's clean table and returned
	# GUARD_RC=0 on a board that actually carried A32_APP + HE_APP -- the
	# exact #2025 loss through a new door). `mktemp` both makes the path
	# run-unique (its own randomised suffix, no two callers can collide) and
	# creates the file atomically (O_CREAT|O_EXCL under the hood), closing
	# the rm-then-open symlink-race window a predictable rm -f/`>` pair
	# leaves open. A directory `mktemp` cannot create in (unwritable TMPDIR)
	# fails here, which is the same "abort, do not guess" outcome the old
	# rm -f/existence-check pair gave for an unremovable stale file.
	#
	# RETENTION IS DELIBERATE, NOT A LEAK: this file is never removed on
	# any exit path (success or abort) -- it is the "always leaves a record
	# of what was resident immediately before a destructive write" audit
	# trail this guard exists to provide (see the function's own header
	# comment), and a run that PASSED is exactly the run whose pre-write
	# state you may later need to prove. Each run leaves one more
	# ${TMPDIR:-/tmp}/<tag>-atoc-before.<random>; periodically clean
	# TMPDIR by hand (see README.md's Quick start / troubleshooting).
	#
	# The X's MUST be trailing, no suffix after them (no ".log" here).
	# BSD/macOS mktemp requires the placeholder to be the literal end of
	# the template and fails EVERY call with a misleading "File exists"
	# on a mid-template placeholder (`...XXXXXX.log`) that GNU mktemp
	# tolerates -- measured on macOS CI: the guard aborted (exit 5) on
	# every single run, silently dead on that platform, fail-closed. GNU
	# mktemp's own --suffix flag is not the fix either: it does not exist
	# on BSD/macOS mktemp, so it would only move the same break.
	local before
	before=$(mktemp "${TMPDIR:-/tmp}/${tag}-atoc-before.XXXXXX") || {
		echo "!! ABORT ($tag): cannot create the pre-write ATOC transcript in ${TMPDIR:-/tmp}" >&2
		return 5
	}

	# rc tracks whether the query itself succeeded -- defaults to failed
	# (1) so the SE_UART-unset and maintenance-missing branches, which never
	# run a real query, fall straight into "unverified" below rather than
	# silently defaulting to a pass. A query that exits non-zero after
	# emitting partial table rows (e.g. a serial timeout mid-read) must also
	# land here, not decode as "ok" from the transcript text alone.
	local rc=1
	if [ -z "${SE_UART:-}" ]; then
		echo "GUARD: SE_UART is unset -- cannot query the resident ATOC via 'maintenance -opt gettoc'" >"$before" || return 5
	elif [ -x "$SETOOLS_DIR/maintenance" ]; then
		# Confirm the serial device that answers is actually the SES, not the
		# app console (e.g. on an AEN EVK bench unit, /dev/ttyUSB0 is SE-UART,
		# /dev/ttyUSB1 is the app console). BENCH-VERIFIED: a real
		# `getbanner` capture off an AEN EVK bench unit (2026-09-07) reads
		# " SES A1 v1.110.0 Mar  4 2026 19:06:23" after ANSI stripping --
		# docs/debugging-aen.md:548 is only a doc placeholder
		# ("SES <rev> v<version> <build date>"), not a transcript, and is
		# NOT the proof. A gettoc read off the wrong device is not a safe
		# verdict, so a missing/garbled banner also forces the query
		# unverified below.
		local banner banner_ok=1 banner_rc
		banner=$( ( cd "$SETOOLS_DIR" && ./maintenance -b "${SE_UART_BAUD:-57600}" -c "$SE_UART" -opt getbanner ) 2>&1 )
		banner_rc=$?
		# Real capture off an AEN EVK bench unit (2026-09-07), ANSI intact:
		#   ^[[94m SES A1 v1.110.0 Mar  4 2026 19:06:23 ^[[0m
		# Strip the ANSI FIRST, then match -- and the stripped line has a
		# LEADING SPACE (SETOOLS' own padding, not a terminal artifact), so
		# a bare `^SES` anchor rejects every real banner and aborts every
		# run on real hardware. Tolerate leading whitespace.
		printf '%s\n' "$banner" | sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g' \
			| grep -qE '^[[:space:]]*SES [^[:space:]]+ v[^[:space:]]+' || banner_ok=0
		# getbanner exiting non-zero while still printing a well-formed
		# banner line must not read as ok -- same class as the gettoc rc fix
		# above (measured: getbanner rc=3 with a valid banner -> GUARD_RC=0
		# before this).
		[ "$banner_rc" -eq 0 ] || banner_ok=0
		( cd "$SETOOLS_DIR" && ./maintenance -b "${SE_UART_BAUD:-57600}" -c "$SE_UART" -opt gettoc ) >"$before" 2>&1
		rc=$?
		[ "$banner_ok" -eq 1 ] || rc=1
	else
		echo "GUARD: SETOOLS 'maintenance' tool not found in $SETOOLS_DIR -- cannot query the resident ATOC" >"$before" || return 5
	fi
	[ -f "$before" ] || return 5
	echo ">>> resident ATOC before this write ($before):" >&2
	cat "$before" >&2

	# SETOOLS emits ANSI codes on some terminals/versions -- not just SGR
	# colour (`...m`): a real capture also carries a cursor-show sequence
	# (`^[[?25h`), and an erase-in-line (`^[[K`) landing inside a Name cell
	# would otherwise survive an SGR-only strip and read as a foreign entry
	# (false-alarm direction). Strip any CSI sequence (ESC [ ... final-byte),
	# not just the `m`-terminated ones, before the awk table parse (also
	# matches read-update-log-proof.sh:150's own maintenance-read strip).
	# Also drop a trailing CR: SETOOLS on some hosts emits CRLF, and
	# `grep -qix "no atoc found on target device."` below is anchored with
	# `$`, so an untouched CR would make a genuinely blank board's "No ATOC"
	# line fail to match and read as unverified (GUARD_RC=5) instead.
	local stripped
	stripped=$(sed -E 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$before" | tr -d '\r')

	# Table rows look like "|   DEVICE |  CM0+  | 0x... | ... |" (docs/aen-provisioning.md
	# shows a real one) -- the Name column is the literal JSON key of whatever wrote it.
	# BENCH-VERIFIED against a real 9-row getbanner+gettoc capture off
	# an AEN EVK bench unit (2026-09-07): SETOOLS' colour wraps the WHOLE LINE
	# (`^[[94m |    DEVICE|...|`), not just the cell text, so the
	# ANSI-stripped row keeps a LEADING SPACE before the pipe. A bare `/^\|/`
	# anchor (no synthetic test fixture ever exercised this -- the test's
	# ANSI table colours only the cell, not the line) never matched a single
	# real row: `resident` silently computed empty on EVERY real coloured
	# transcript, aborting every run as "unverified" rather than ever
	# actually detecting -- or clearing -- a foreign entry.
	# Carries the CPU column (field 3, e.g. "CM0+"/"M55-HE"/"A32_0") alongside
	# the name, tab-separated -- the DEVICE/SERAM0/SERAM1 baseline exemption
	# below cross-checks it (a same-named row on the wrong CPU is never
	# baseline SE state, just a coincidentally-named app entry).
	local resident=()
	while IFS= read -r n; do
		resident+=("$n")
	done < <(printf '%s\n' "$stripped" | awk -F'|' '
		/^[ \t]*\|/ {
			name = $2; cpu = $3
			gsub(/^[ \t]+|[ \t]+$/, "", name)
			gsub(/^[ \t]+|[ \t]+$/, "", cpu)
			if (name != "" && name != "Name" && name !~ /^-+$/) print name "\t" cpu
		}')

	# Only trust the transcript's text when the query itself actually
	# succeeded (rc=0) -- an error line containing "no atoc" (e.g. "no ATOC
	# response from target") must not read as a genuinely empty board, and
	# anchor to SETOOLS' exact message rather than a bare substring match.
	local query_status=unverified
	if [ "$rc" -eq 0 ]; then
		if printf '%s\n' "$stripped" | grep -qix "no atoc found on target device."; then
			query_status=empty
		elif [ "${#resident[@]}" -gt 0 ]; then
			query_status=ok
		fi
	fi

	# DEVICE plus the two SE firmware banks (SERAM0/SERAM1 -- one marked
	# "* SERAM0" as the currently-booted bank, docs/aen-se-services.md:194)
	# are baseline SE state, never touched by app-write-mram -p: only a
	# System Package update rewrites SERAM (docs/aen-se-services.md
	# section 0.1). Both real captures above show SERAM0/SERAM1 resident
	# alongside completely different app entries, confirming they are
	# independent of whatever app ATOC was last written -- without this
	# exemption the guard flags them as "extra" on EVERY real board,
	# unconditionally, which trains the operator to always pass
	# --replace-atoc (discovered validating the parser against the real
	# gettoc-BEFORE-2entry.txt capture, which must pass clean and instead
	# aborted before this fix).
	#
	# Cross-check the CPU column (both real captures show all three
	# baseline rows as "CM0+", and no app entry ever is): the exemption is
	# on (name, CPU), not name alone -- a row that merely happens to share
	# one of these names on a DIFFERENT core (measured: a synthesized
	# "SERAM1 | M55-HE | ..." row) is an app entry with a colliding name,
	# not SE firmware, and must still trip the guard.
	local extra=() entry name cpu a hit nbase
	for entry in "${resident[@]}"; do
		name="${entry%%$'\t'*}"
		cpu="${entry#*$'\t'}"
		nbase="${name#\* }"
		case "$nbase" in
		DEVICE | SERAM0 | SERAM1)
			[ "$cpu" = "CM0+" ] && continue
			;;
		esac
		hit=0
		for a in "${allowed[@]}"; do
			[ "$name" = "$a" ] && { hit=1; break; }
		done
		[ "$hit" -eq 0 ] && extra+=("$name")
	done

	if [ "$replace_atoc" -ne 1 ]; then
		if [ "$query_status" = unverified ]; then
			echo "!! ABORT ($tag): could not read the resident ATOC via 'maintenance -c \$SE_UART -opt gettoc'" >&2
			echo "   (see $before). A fresh ATOC write REPLACES every app entry not in it, so" >&2
			echo "   writing blind risks silently delisting anything already on this board -- that is" >&2
			echo "   exactly how an AEN EVK bench unit lost its A32 Linux boot chain on 2026-09-07." >&2
			# The remedy depends on which flow got here, and naming the wrong
			# one is its own defect (alp-sdk#2187). On Flow A, $SE_UART IS the
			# transport: a failed query means confirm by hand and override.
			# On Flow D the query is opportunistic -- the flow never needs a
			# serial device -- so a set-but-unanswering $SE_UART is almost
			# always a stale value, and --replace-atoc is exactly the wrong
			# advice: it is the "I checked and still want to replace" opt-out,
			# and no check ran here. Steering an operator onto it builds the
			# habit flash-jlink.sh's own header says must never form.
			if [ "${BENCH_ATOC_FLOW:-A}" = D ]; then
				echo "   SE_UART is exported (${SE_UART:-<unset>}), so this took the query path -- but" >&2
				echo "   the query did not complete (the transcript above says which step failed;" >&2
				echo "   a missing SETOOLS 'maintenance' binary reaches this same abort), and Flow D" >&2
				echo "   does not need an SE-UART at all. The usual cause is a stale or wrong value" >&2
				echo "   rather than a board problem: raw /dev/ttyUSBn paths are enumeration-ordered," >&2
				echo "   and this bench has had its AEN SE-UART/app-console paths measured SWAPPED in" >&2
				echo "   a stale table (#2032/#2064)." >&2
				echo "   Two ways forward:" >&2
				echo "     1. point SE_UART at this slot's real SE-UART -- prefer LG_PLACE, which" >&2
				echo "        resolves it per-slot instead of by a raw path -- then re-run." >&2
				echo "     2. 'unset SE_UART' and re-run with --atoc-unqueryable, if this bench" >&2
				echo "        slot genuinely has no SE-UART wired." >&2
				echo "   NOT --replace-atoc: that flag asserts you checked what is resident, and" >&2
				echo "   on this path nothing was ever read." >&2
			else
				echo "   Confirm by hand what is resident, then re-run with --replace-atoc." >&2
			fi
			return 5
		fi
		if [ "${#extra[@]}" -gt 0 ]; then
			echo "!! ABORT ($tag): this write REPLACES every app ATOC entry not in it -- it does NOT merge." >&2
			echo "   This board also carries: ${extra[*]}" >&2
			echo "   Writing now would SILENTLY DELIST ${extra[*]} -- no error, no SES warning" >&2
			echo "   (this destroyed the A32 Linux boot chain on an AEN EVK bench unit, 2026-09-07)." >&2
			echo "   Re-run with --replace-atoc only once you can restore ${extra[*]}, or if" >&2
			echo "   losing them is genuinely intended." >&2
			return 5
		fi
	fi
	return 0
}

# bench_flowd_atoc_guard <replace-atoc 0|1> <atoc-unqueryable 0|1> <tag> [allowed-entry ...]
#
# GUARD (alp-sdk#2027) -- the Flow D follow-up to bench_atoc_replace_guard
# above. flash-jlink.sh / flash-jlink-hp.sh / flash-jlink-mramxip.sh write
# the SAME replacing ATOC over SWD (`loadbin`) that Flow A writes over
# $SE_UART, but Flow D's whole premise is "J-Link only, no serial device
# required" (alp-sdk#2025/#2026) -- PR #2029 made that an EXPLICIT tradeoff
# instead of a silent one by requiring --atoc-unqueryable before writing when
# there is truly no way to check (see changelog.d/2025.md). This closes the
# other half of that tradeoff, without ever making $SE_UART a hard
# requirement of Flow D:
#
#   $SE_UART exported  -> the resident ATOC genuinely IS queryable on this
#                          bench slot (some are wired for one even though
#                          Flow D never NEEDS it) -- call the SAME shared
#                          guard, never a second, weaker copy of its logic.
#                          Its own internal checks (maintenance binary
#                          present, a well-formed banner, gettoc's own exit
#                          status) still apply: an exported-but-broken
#                          $SE_UART lands in bench_atoc_replace_guard's own
#                          "unverified" path exactly as it would for Flow A,
#                          and still refuses to write (exit 5).
#                          --atoc-unqueryable is IGNORED in this branch --
#                          it is an acknowledgement that no check ran, and
#                          one just did.
#                          What that refusal TELLS the operator differs by
#                          flow (alp-sdk#2187): Flow A is sent to
#                          --replace-atoc, because $SE_UART is its transport
#                          and a human confirming by hand is the only way
#                          past. Flow D is sent to fix-or-unset $SE_UART
#                          instead, because Flow D never needed the serial
#                          device and a stale value is the likely cause --
#                          naming --replace-atoc there would teach the
#                          checked-and-override flag as the cure for a check
#                          that never ran. Selected by $BENCH_ATOC_FLOW,
#                          which this function sets to D for the call.
#   $SE_UART unset      -> unchanged from #2029: --atoc-unqueryable is
#                          required (abort, exit 8, if it is missing), and
#                          either way this prints a one-line statement that
#                          the resident-ATOC check did NOT run and why -- a
#                          skipped check must never look like a passed one.
#
# Returns 0 to proceed; whatever bench_atoc_replace_guard returns (5 on its
# own abort) when $SE_UART is exported; 8 when $SE_UART is unset and
# --atoc-unqueryable was not passed.
bench_flowd_atoc_guard() {
	local replace_atoc="$1" unqueryable="$2" tag="$3"
	shift 3
	if [ -n "${SE_UART:-}" ]; then
		echo "GUARD ($tag): SE_UART is exported -- querying the resident ATOC before writing (alp-sdk#2027)." >&2
		# Tell the shared guard which flow it is speaking for, so its
		# "could not read the resident ATOC" abort names remedies that
		# apply here -- fix or unset $SE_UART -- instead of Flow A's
		# --replace-atoc, which asserts a check that did not run
		# (alp-sdk#2187). `local` scopes it to this call and restores
		# whatever an outer frame had, so it cannot leak into a later
		# Flow A guard in the same shell. It changes NO control flow:
		# the refusal, its exit status and every other branch are
		# untouched -- this selects message text only.
		local BENCH_ATOC_FLOW=D
		bench_atoc_replace_guard "$replace_atoc" "$tag" "$@"
		return $?
	fi
	if [ "$unqueryable" = "1" ]; then
		echo "GUARD ($tag): SE_UART is unset -- proceeding WITHOUT the resident-ATOC check (--atoc-unqueryable, alp-sdk#2027)." >&2
		return 0
	fi
	echo "!! REFUSING TO WRITE ($tag): this Flow D write REPLACES the entire ATOC." >&2
	echo "   SE_UART is not exported, so this script cannot enumerate what is" >&2
	echo "   currently resident before it writes -- any resident boot entry not" >&2
	echo "   named in the config below (an A32 boot chain, an HP app, a diagnostic" >&2
	echo "   image) is silently DELISTED, and the SES prints '[SES] ATOC ok'" >&2
	echo "   afterwards with no warning (issue #2025)." >&2
	echo "   Two ways forward:" >&2
	echo "     1. export SE_UART=<device> if this bench slot has one wired -- the" >&2
	echo "        guard above then queries the resident ATOC automatically (#2027)," >&2
	echo "        exactly like Flow A." >&2
	echo "     2. pass --atoc-unqueryable to acknowledge there is no SE-UART on" >&2
	echo "        this bench slot and proceed without the check." >&2
	return 8
}

# --------------------------------------------------------------------
# Flow D sector-padded MRAM write + fresh-session savebin proof (alp-sdk#2233)
# --------------------------------------------------------------------
#
# TWO MEASURED DEFECTS (2026-09-19, see the issue):
#
#   1. SEGGER's built-in AE822FA0E5597LS0_M55_HE loader always erases and
#      reprograms WHOLE 16 KiB (0x4000) sectors, never just the bytes a
#      `loadbin` names, and never reads a sector's own prior contents first --
#      so every byte of a touched sector OUTSIDE the blob becomes 0xFF the
#      instant a write does not start and end on a sector boundary.
#   2. `verifybin` compares against J-Link's own in-process flash-CACHE, not a
#      fresh chip read -- `Verify successful.` proves nothing about MRAM.
#
# The functions below are the shared machinery every Flow D writer uses to
# fix both: read the CURRENT bytes of every sector a write touches, overlay
# the blob(s) on them (scripts/bench/aen/flowd_sector_pad.py, pure host-side,
# no probe access), `loadbin` the padded image instead of the raw blob so the
# loader's own whole-sector rewrite reproduces the neighbours unchanged, and
# prove it by `savebin`-reading the range back in a FRESH JLinkExe session
# (a new process cannot be served from another process's flash cache) and
# comparing byte-for-byte against that same padded image -- which proves both
# that the blob landed and that the neighbours survived.
#
# `verifybin` MUST NOT gate a Flow D script any more (defect 2) -- a
# transcript may still carry one for its log value, but the pass/fail
# decision comes from bench_flowd_proof() below, never from grepping for
# "Verify successful."/"Verify failed." in a `loadbin` session's own output.
#
# CALL SHAPE for the common case (one write, or several writes that share one
# write session): plan -> read the named sectors -> build the padded
# manifest -> embed its loadbin line(s) in the caller's own CommandFile
# (preserving whatever guard/reset ordering that script already has --
# `, noreset` where the caller must not let `loadbin`'s implicit reset race a
# second write, same as any other loadbin) -> once that session has run, call
# bench_flowd_proof() in a fresh session. Deliberately NOT one monolithic
# "do everything" function: flash-jlink-mramxip.sh's `h` / `exec
# SetSkipProgOnCRCMatch` / dual-noreset / halt-before-programming ordering
# (see its own header) is load-bearing and must not be disturbed to fit a
# one-size call -- it calls bench_flowd_plan/_read_sectors/_build/
# _loadbin_lines directly and splices the loadbin lines into its existing
# CommandFile.
#
# FLOWD_DRY_RUN -- set to any non-empty value to exercise the WHOLE pipeline
# (plan, sector "read", padded-image build, the loadbin line(s), the
# CommandFile a write session would run, and the CommandFile a proof session
# would run) with NO WRITE EVER REACHING A PROBE. This is a THREE-layer
# guarantee, not one flag with one effect (alp-sdk#2233 review blocker 1 --
# an earlier version of this comment claimed the guarantee before all three
# layers exchange existed, which was false: bench_flowd_read_sectors() alone
# skipping its own savebin did NOT stop the WRITE session further down from
# running for real):
#
#   1. Every writer script itself checks FLOWD_DRY_RUN right after it builds
#      its write CommandFile and, if set, prints "DRY RUN -- nothing
#      written" plus the CommandFile and EXITS -- it never reaches the line
#      that would invoke bench_jlink_run() for that CommandFile at all.
#   2. bench_jlink_run() itself independently REFUSES (does not open a
#      probe) to run any CommandFile containing a `loadbin`/`erase` line
#      while FLOWD_DRY_RUN is set -- a backstop for #1, not a substitute.
#   3. bench_flowd_proof() never reports success in this mode -- it returns
#      a distinct non-zero ("nothing proven: dry run") rather than 0, so a
#      caller that DOES still reach it (skipping #1, e.g. a direct call from
#      a test) cannot mistake "dry run, nothing was ever written" for "proof
#      passed".
#
# bench_flowd_read_sectors() synthesizes all-0x00 sector images instead of a
# real savebin session in this mode (clearly logged as synthetic) so the
# HOST-ONLY steps downstream -- build, the loadbin lines, the CommandFiles --
# still run their REAL code path against real files an operator or a test
# can inspect, not a second hand-simulated copy of it. Never set this on a
# real bench host; every helper below still requires a resolved LG_PLACE /
# real bench_jlink_run() for its OTHER (unrelated) JLinkExe touches, e.g. the
# DPIDR preflight every script already carries BEFORE its own FLOWD_DRY_RUN
# check -- FLOWD_DRY_RUN governs only the sector-pad read/write/proof
# sessions, never a script's other guards. EXCEPT erase-storage.sh (alp-sdk#2233
# review round 3, finding 2): that script now treats FLOWD_DRY_RUN exactly
# like its own --dry-run flag, so its DPIDR preflight and ATOC-trailer read
# are ALSO skipped under FLOWD_DRY_RUN, not just its own --dry-run -- see
# that script's own header for why (a blank ATOC-trailer read taken under
# FLOWD_DRY_RUN's synthetic sector-read would otherwise get backed up to
# $BENCH_ROOT/flowd-backup/... as if it were a real pre-write sector).
export FLOWD_DRY_RUN="${FLOWD_DRY_RUN:-}"

# FLOWD_SECTOR_SIZE / FLOWD_WINDOW_LO / FLOWD_WINDOW_HI -- forwarded verbatim
# to flowd_sector_pad.py; override only to point at a different part's
# geometry. Defaults match the AEN E8 application MRAM window (alp-sdk#2233).
export FLOWD_SECTOR_SIZE="${FLOWD_SECTOR_SIZE:-0x4000}"
export FLOWD_WINDOW_LO="${FLOWD_WINDOW_LO:-0x80000000}"
export FLOWD_WINDOW_HI="${FLOWD_WINDOW_HI:-0x8057FFFF}"

# FLOWD_SECTOR_PAD_PY -- resolved next to this file so it works from any
# checkout with no extra config; override only for a test double.
export FLOWD_SECTOR_PAD_PY="${FLOWD_SECTOR_PAD_PY:-$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/flowd_sector_pad.py}"

# bench_flowd_python <args...> -- run the pure host-side helper. Pinned
# PYTHONIOENCODING=utf-8: this subshell's own locale is not guaranteed
# UTF-8 (the IMPLICIT-ENCODING lint -- a Windows host defaults a Python
# child to cp1252, which breaks any non-ASCII path/manifest content).
bench_flowd_python() {
	PYTHONIOENCODING=utf-8 python3 "$FLOWD_SECTOR_PAD_PY" "$@"
}

# bench_flowd_jlink_path <path> -- the path form the JLinkExe BINARY can
# open, which is not always the one this shell sees. On a Windows bench host
# (Git Bash/MSYS driving a native JLink.exe) a Unix-style "/tmp/..." path is
# meaningless to the callee and the run dies with "Failed to open file." --
# the same trap erase-storage.sh's own ZEROS_FOR_JLINK conversion exists for
# (see its comment); this is that same conversion, shared, so every Flow D
# writer gets it for the padded images it now loadbins, not just the one
# script that had already hit it. On Linux/macOS cygpath is absent and the
# path is already right.
bench_flowd_jlink_path() {
	if command -v cygpath >/dev/null 2>&1; then
		cygpath -w "$1"
	else
		printf '%s' "$1"
	fi
}

# bench_flowd_plan <sectors-out-file> <write...> -- <write> is "<path>:<hex-
# address>", repeatable. Writes one sector base (e.g. "0x802E4000") per line
# to <sectors-out-file>: every sector that must be pre-read before a padded
# image can be built. Host-only (no probe), so this always runs for real,
# FLOWD_DRY_RUN or not. Non-zero (message on stderr, nothing written) on any
# refusal -- a blob outside the MRAM window, or two blobs overlapping with
# different bytes.
bench_flowd_plan() {
	local out="$1"
	shift
	local -a wargs=()
	local w
	for w in "$@"; do
		wargs+=(--write "$w")
	done
	bench_flowd_python plan \
		--sector-size "$FLOWD_SECTOR_SIZE" --window-lo "$FLOWD_WINDOW_LO" --window-hi "$FLOWD_WINDOW_HI" \
		"${wargs[@]+"${wargs[@]}"}" >"$out"
}

# bench_flowd_read_sectors <tag> <sector-dir> <sectors-file> -- ONE read-only
# J-Link session (the GENERIC device, JLINK_DEVICE_READ) that `savebin`s
# every sector base named in <sectors-file> (one per line, from
# bench_flowd_plan) to <sector-dir>/<ADDR-no-0x>.bin. Refuses (non-zero) if
# any expected file is missing or the wrong size afterwards -- a partial or
# failed session must not silently hand bench_flowd_build a short read that
# then gets baked into a padded image as if it were real MRAM content.
#
# FLOWD_DRY_RUN: touches no probe. Prints the CommandFile it would have run,
# then synthesizes each expected sector as FLOWD_SECTOR_SIZE bytes of 0x00 --
# clearly logged as synthetic, never mistaken for a real read -- so
# bench_flowd_build downstream still runs its real code path against real,
# correctly-sized files.
bench_flowd_read_sectors() {
	local tag="$1" sector_dir="$2" sectors_file="$3"
	mkdir -p "$sector_dir"

	if [ ! -s "$sectors_file" ]; then
		echo "bench-env: bench_flowd_read_sectors ($tag): nothing to read (empty plan)" >&2
		return 0
	fi

	local cmdfile
	cmdfile="$(mktemp "${TMPDIR:-/tmp}/flowd-read.jlink.XXXXXX")" || return 1
	{
		echo "device $JLINK_DEVICE_READ"
		echo "si SWD"
		echo "speed $JLINK_SPEED"
		echo "connect"
		local base
		while IFS= read -r base; do
			[ -n "$base" ] || continue
			printf 'savebin %s %s %s\n' \
				"$(bench_flowd_jlink_path "$sector_dir/${base#0x}.bin")" "$base" "$FLOWD_SECTOR_SIZE"
		done <"$sectors_file"
		echo "exit"
	} >"$cmdfile"

	if [ -n "$FLOWD_DRY_RUN" ]; then
		echo ">>> FLOWD_DRY_RUN ($tag): sector pre-read CommandFile (not run):" >&2
		cat "$cmdfile" >&2
		local base
		while IFS= read -r base; do
			[ -n "$base" ] || continue
			PYTHONIOENCODING=utf-8 python3 -c "
import sys
with open(sys.argv[1], 'wb') as f:
    f.write(b'\\x00' * int(sys.argv[2], 0))
" "$sector_dir/${base#0x}.bin" "$FLOWD_SECTOR_SIZE"
			echo "    (synthetic, FLOWD_DRY_RUN -- not read from MRAM): $sector_dir/${base#0x}.bin" >&2
		done <"$sectors_file"
		rm -f "$cmdfile"
		return 0
	fi

	local out
	out="$(mktemp "${TMPDIR:-/tmp}/flowd-read.out.XXXXXX")" || {
		rm -f "$cmdfile"
		return 1
	}
	bench_jlink_run -nogui 1 -CommanderScript "$cmdfile" >"$out" 2>&1 || true
	rm -f "$cmdfile"
	# alp-sdk#2233 review round 3, item 12: `$out` used to leak on every call
	# (each pre-read mktemp's a fresh file, forever). Kept on any failure
	# below as evidence; removed just before the final `return 0`.
	bench_jlink_assert_connected "$out" "$tag sector pre-read" || {
		local rc=$?
		echo "bench-env: bench_flowd_read_sectors ($tag): transcript kept for inspection: $out" >&2
		return "$rc"
	}

	# alp-sdk#2233 review item 7: a `savebin` that silently no-ops or fails
	# mid-read must not be trusted just because a file of the right SIZE later
	# exists (a stale file from an earlier run in the same scratch dir would
	# pass that check alone). "Could not read memory" is the one JLinkExe
	# read-failure string already bench-measured and gated on elsewhere in
	# this directory for an M-series memory read (ram-run.sh's own `mem8`
	# gate) -- refuses on any KNOWN failure text, below.
	if grep -qiE 'Could not read memory|Cannot read memory|\*\*\*\* ?Error' "$out"; then
		echo "bench-env: bench_flowd_read_sectors ($tag): the transcript reports a read failure --" >&2
		grep -iE 'Could not read memory|Cannot read memory|\*\*\*\* ?Error' "$out" | head -5 >&2
		echo "           refusing to trust any sector image from this session." >&2
		return 1
	fi

	# alp-sdk#2233 review round 5: the `savebin` SUCCESS line is now
	# bench-measured (E1M-AEN803, serial 2026W36-0001, J-Link V9.50,
	# `savebin <file> 0x80578000 0x8000`):
	#   Reading 32768 bytes from addr 0x80578000 into file...O.K.
	# -- an earlier version of this comment said no such positive string had
	# ever been measured; this is that string. Require ONE per PLANNED
	# savebin (the line count in $sectors_file), on top of (not instead of)
	# the failure-string refusal above and the per-sector existence+size
	# loop below -- a session that reports neither a known failure NOR a
	# success line for every sector is not evidence, whatever files happen
	# to already sit on disk from an earlier run.
	local expected_savebins got_savebins
	expected_savebins=$(grep -c '.' "$sectors_file")
	got_savebins=$(grep -cE 'Reading [0-9]+ bytes from addr 0x[0-9A-Fa-f]+ into file.*O\.K\.' "$out")
	if [ "$got_savebins" -lt "$expected_savebins" ]; then
		echo "bench-env: bench_flowd_read_sectors ($tag): expected $expected_savebins savebin success" >&2
		echo "           line(s), found $got_savebins in the transcript -- refusing to trust any" >&2
		echo "           sector image from this session." >&2
		return 1
	fi

	local base name path got
	while IFS= read -r base; do
		[ -n "$base" ] || continue
		name="${base#0x}"
		path="$sector_dir/$name.bin"
		if [ ! -f "$path" ]; then
			echo "bench-env: bench_flowd_read_sectors ($tag): expected sector read $path is missing" >&2
			return 1
		fi
		got=$(wc -c <"$path" | tr -d ' ')
		if [ "$got" != "$((FLOWD_SECTOR_SIZE))" ]; then
			echo "bench-env: bench_flowd_read_sectors ($tag): $path is $got bytes, expected $((FLOWD_SECTOR_SIZE))" >&2
			return 1
		fi
	done <"$sectors_file"
	rm -f "$out"
	return 0
}

# bench_flowd_build <sector-dir> <out-dir> <write...> -- builds one padded
# image per merged range (blob bytes overlaid on <sector-dir>'s pre-read
# sector content) plus a manifest, via flowd_sector_pad.py build. Sets
# FLOWD_MANIFEST to the manifest path on success. Host-only: runs the same
# code path whether <sector-dir> holds real reads or FLOWD_DRY_RUN's
# synthetic zero-fill -- no dry-run special-casing needed here.
FLOWD_MANIFEST=""
bench_flowd_build() {
	local sector_dir="$1" out_dir="$2"
	shift 2
	local -a wargs=()
	local w
	for w in "$@"; do
		wargs+=(--write "$w")
	done
	mkdir -p "$out_dir"
	# shellcheck disable=SC2034  # consumed by every CALLER of bench_flowd_build
	# (flash-jlink.sh et al.) after sourcing this file -- shellcheck -x follows a
	# `source` forward, not back to the scripts that source THIS file, so it can
	# never see that use from here (same class of cross-file false positive as
	# GD32_DPIDR above, see that comment).
	FLOWD_MANIFEST="$(bench_flowd_python build \
		--sector-size "$FLOWD_SECTOR_SIZE" --window-lo "$FLOWD_WINDOW_LO" --window-hi "$FLOWD_WINDOW_HI" \
		--sector-dir "$sector_dir" --out-dir "$out_dir" "${wargs[@]+"${wargs[@]}"}")" || return $?
	return 0
}

# bench_flowd_prepare_write <tag> <scratch-dir> <write...> -- the common-case
# composition of plan + read + build for a SINGLE write session (one or more
# writes that land in the same CommandFile). Leaves FLOWD_MANIFEST and
# FLOWD_SECTORS_FILE set on success; the caller embeds
# bench_flowd_loadbin_lines' output in its own CommandFile immediately after,
# and (for the race check below) bench_flowd_prewrite_lines' output BEFORE it.
FLOWD_SECTORS_FILE=""
bench_flowd_prepare_write() {
	local tag="$1" scratch="$2"
	shift 2
	mkdir -p "$scratch"
	local sectors_file="$scratch/sectors.txt"
	bench_flowd_plan "$sectors_file" "$@" || return $?
	bench_flowd_read_sectors "$tag" "$scratch/sectors" "$sectors_file" || return $?
	bench_flowd_build "$scratch/sectors" "$scratch/padded" "$@" || return $?
	# shellcheck disable=SC2034  # consumed by every CALLER after sourcing this
	# file, same cross-file false positive as FLOWD_MANIFEST above.
	FLOWD_SECTORS_FILE="$sectors_file"
	return 0
}

# --------------------------------------------------------------------
# Pre-read -> write RACE detection (alp-sdk#2233 review major 4)
# --------------------------------------------------------------------
# The pre-read session (bench_flowd_read_sectors) does not halt the core, and
# neither the HP nor the A32 cores are ever halted by any of this -- so an
# app write to a touched sector BETWEEN the pre-read and the write session's
# own `loadbin` would be silently overwritten by the padded image (which was
# built from the now-stale pre-read), and bench_flowd_proof would still PASS
# afterward, because the padded image is exactly what a correctly-working
# write reproduces.
#
# Since JLinkExe cannot branch mid-CommandFile, the write session itself
# `savebin`s the SAME sectors into a SEPARATE "prewrite" directory right
# before its own load lines (bench_flowd_prewrite_lines, embedded by the
# caller) -- as close to the load as this tool can get. bench_flowd_check_race
# then compares prewrite against the original pre-read ON THE HOST, after the
# session: any difference proves something else touched a to-be-padded sector
# after the pre-read completed, and the load may have clobbered it. This
# cannot detect a race in the OTHER direction (a write landing between the
# prewrite savebin and the load itself, inside the same session) -- that
# window is far smaller (one CommandFile, no host round-trip) and is not
# addressed here; say so plainly rather than imply full coverage.

# bench_flowd_prewrite_lines <sectors-file> <prewrite-dir> -- print one
# `savebin <prewrite-dir>/<ADDR>.bin <addr> <sector-size>` line per sector
# base in <sectors-file>, for the caller to splice into its OWN write
# CommandFile right after `connect` (and `h`, where the script halts) and
# BEFORE its bench_flowd_loadbin_lines output.
bench_flowd_prewrite_lines() {
	local sectors_file="$1" prewrite_dir="$2"
	[ -s "$sectors_file" ] || return 0
	# JLinkExe's `savebin` does not create its destination directory -- unlike
	# every OTHER bench_flowd_* function, which mkdir -p's its own output
	# directory itself, this one is a pure text generator with no probe
	# access, so it takes care of it here too rather than leaving every
	# caller to remember a bare `mkdir -p` before embedding this output
	# (alp-sdk#2233 review round 3, item 13c -- this is an ASSUMPTION/stub
	# observation, not a cited bench transcript: no real-hardware JLinkExe
	# log demonstrating a bare `savebin` into a missing directory has been
	# captured on this bench. Documented JLinkExe behaviour and the stub
	# harness this repo's own tests use both agree `savebin` does not
	# mkdir -p its destination; relabelled from an earlier "measured" claim
	# that overstated its evidence. If this ever IS measured against real
	# hardware, cite the transcript here instead of this note).  Without
	# this mkdir, a missing directory would make every sector read back as
	# "missing prewrite capture", and bench_flowd_check_race would report a
	# RACE on every run, real or not.
	mkdir -p "$prewrite_dir"
	local base
	while IFS= read -r base; do
		[ -n "$base" ] || continue
		printf 'savebin %s %s %s\n' \
			"$(bench_flowd_jlink_path "$prewrite_dir/${base#0x}.bin")" "$base" "$FLOWD_SECTOR_SIZE"
	done <"$sectors_file"
}

# bench_flowd_check_race <tag> <sectors-file> <preread-dir> <prewrite-dir>
# [write-session-out-file] -- host-side (no probe) cmp of every planned
# sector's pre-read image against its prewrite capture from the SAME write
# session. Returns non-zero (and prints "RACE DETECTED", naming and KEEPING
# both copies for restore/inspection) on any difference, or if a prewrite
# capture is missing entirely (the write session's own savebin didn't
# produce it -- treated the same as a detected race, since a race cannot be
# ruled out either). Never deletes either directory itself -- the caller's
# scratch dir owns that.
#
# [write-session-out-file] (alp-sdk#2233 review round 3, item 7), if given:
# the write session's own captured transcript is grepped for the SAME
# read-failure strings bench_flowd_read_sectors() checks, scoped to the
# portion BEFORE the first `Downloading file` (the point loadbin actually
# starts) -- i.e. exactly the prewrite savebin's own output, not anything a
# loadbin/reset further down might also print. A failed prewrite READ (not
# just a byte mismatch) means the race check has no trustworthy "just
# before the load" snapshot to compare against at all -- fail closed, same
# as a missing prewrite capture above.
bench_flowd_check_race() {
	local tag="$1" sectors_file="$2" preread_dir="$3" prewrite_dir="$4" write_out="${5:-}"
	[ -s "$sectors_file" ] || return 0
	local base name pre post raced=0 preload
	if [ -n "$write_out" ] && [ -f "$write_out" ]; then
		preload="$(awk '/Downloading file/{exit} {print}' "$write_out")"
		if printf '%s\n' "$preload" | grep -qiE 'Could not read memory|Cannot read memory|\*\*\*\* ?Error'; then
			echo "!! RACE DETECTED ($tag): the write session's own pre-load savebin (the" >&2
			echo "   prewrite capture) reports a read failure -- treating this as a race," >&2
			echo "   fail closed, since there is no trustworthy just-before-the-load snapshot:" >&2
			printf '%s\n' "$preload" | grep -iE 'Could not read memory|Cannot read memory|\*\*\*\* ?Error' | head -5 >&2
			raced=1
		fi
	fi
	while IFS= read -r base; do
		[ -n "$base" ] || continue
		name="${base#0x}"
		pre="$preread_dir/$name.bin"
		post="$prewrite_dir/$name.bin"
		if [ ! -f "$post" ]; then
			echo "!! RACE DETECTED ($tag): no prewrite capture for sector $base ($post" >&2
			echo "   is missing) -- a race for this sector cannot be ruled out." >&2
			raced=1
			continue
		fi
		if ! cmp -s "$pre" "$post"; then
			echo "!! RACE DETECTED ($tag): sector $base changed between the pre-read and the" >&2
			echo "   write session's own pre-load savebin -- something else wrote to this" >&2
			echo "   sector after the pre-read and before the load; the padded image was built" >&2
			echo "   from the now-stale pre-read, so the load may have clobbered fresh data." >&2
			echo "   pre-read (stale):  $pre" >&2
			echo "   prewrite (recent): $post" >&2
			echo "   BOTH ARE KEPT -- restore from $prewrite_dir before trusting this board." >&2
			raced=1
		fi
	done <"$sectors_file"
	[ "$raced" -eq 0 ]
}

# bench_flowd_loadbin_lines <manifest> [noreset 0|1] -- print one `loadbin
# <padded-image> <address>[, noreset]` line per manifest entry (address
# order), for the caller to splice into its own CommandFile in place of the
# raw `loadbin <blob> <address>` line(s) it used to write directly. Image
# paths go through bench_flowd_jlink_path so a Windows JLinkExe can open
# them. <manifest> empty/unset prints nothing (nothing to embed).
bench_flowd_loadbin_lines() {
	local manifest="$1" noreset="${2:-1}"
	[ -n "$manifest" ] || return 0
	local addr image
	while IFS=$'\t' read -r addr image; do
		[ -n "$addr" ] || continue
		if [ "$noreset" = "1" ]; then
			printf 'loadbin %s %s, noreset\n' "$(bench_flowd_jlink_path "$image")" "$addr"
		else
			printf 'loadbin %s %s\n' "$(bench_flowd_jlink_path "$image")" "$addr"
		fi
	done < <(PYTHONIOENCODING=utf-8 python3 -c "
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    manifest = json.load(f)
for e in manifest:
    print(e['address'] + '\t' + e['image'])
" "$manifest")
}

# bench_flowd_proof <tag> <manifest> <read-dir> -- the read-back proof
# (alp-sdk#2233 defect 2): a FRESH read-only J-Link session (a new JLinkExe
# process, so nothing here can be served from another process's flash cache)
# `savebin`s every manifest range to <read-dir>, then flowd_sector_pad.py
# proof `cmp`s each byte-for-byte against its padded image. PASS/FAIL prints
# per range; returns 0 only if every range PASSed.
#
# THIS IS NOT A PERSISTENCE PROOF (alp-sdk#2233 review item 10). A read
# straight back from the part, in a FRESH debug session under the GENERIC
# device profile ($JLINK_DEVICE_READ, alp-sdk#2233 review round 3, item 13a
# -- NOT the same session, and NOT the part-number profile the write itself
# used), proves the bytes landed and their neighbours survived THIS write --
# it does NOT prove the write survives a cold power cycle. flash-jlink-mramxip.sh's
# own header (search "ACCEPTANCE ON THIS PATH IS A COLD-CYCLE READBACK") records
# a measured case where `Verify successful.` was followed by a cold-cycle
# REVERT; nothing here changes that -- a cold-cycle read remains the only
# end-to-end persistence evidence, on top of (not instead of) this proof.
#
# FLOWD_DRY_RUN: touches no probe, and NEVER reports success -- returns 12
# ("nothing proven: dry run") unconditionally. Every real writer script exits
# right after printing its write CommandFile when FLOWD_DRY_RUN is set (see
# each script's own dry-run branch), so in practice this function is never
# reached from one in that mode; this return value exists so a DIRECT caller
# (a test, a future script) can never mistake "dry run, nothing was ever
# written" for "proof passed" -- alp-sdk#2233 review blocker 1c: the previous
# `return 0` here was read by callers as a real PASS.
bench_flowd_proof() {
	local tag="$1" manifest="$2" read_dir="$3"
	[ -n "$manifest" ] || {
		echo "bench-env: bench_flowd_proof ($tag): no manifest -- nothing to prove" >&2
		return 1
	}
	mkdir -p "$read_dir"
	# alp-sdk#2233 review item 6: delete any STALE read-back files left from a
	# previous call before this session's savebin runs -- otherwise a savebin
	# that silently no-ops (probe hiccup, wrong address) leaves yesterday's
	# file in place and the cmp below would compare against THAT, not against
	# nothing, which could accidentally PASS a proof that read nothing at all.
	rm -f "$read_dir"/*.bin 2>/dev/null || true

	local cmdfile
	cmdfile="$(mktemp "${TMPDIR:-/tmp}/flowd-proof.jlink.XXXXXX")" || return 1
	{
		echo "device $JLINK_DEVICE_READ"
		echo "si SWD"
		echo "speed $JLINK_SPEED"
		echo "connect"
		PYTHONIOENCODING=utf-8 python3 -c "
import json, sys
with open(sys.argv[1], 'r', encoding='utf-8') as f:
    manifest = json.load(f)
def line(addr, size):
    name = addr[2:]
    print('savebin ' + sys.argv[2] + '/' + name + '.bin ' + addr + ' ' + hex(size))
for e in manifest:
    line(e['address'], e['size'])
    # GUARD SECTORS (alp-sdk#2233 review item 11): the neighbour sectors just
    # outside the padded range also need a fresh read-back, so proof() can
    # catch collateral damage outside the range, not just inside it.
    for g in e.get('guards', []):
        line(g['address'], g['size'])
" "$manifest" "$(bench_flowd_jlink_path "$read_dir")"
		echo "exit"
	} >"$cmdfile"

	if [ -n "$FLOWD_DRY_RUN" ]; then
		echo ">>> FLOWD_DRY_RUN ($tag): post-write proof-read CommandFile (not run):" >&2
		cat "$cmdfile" >&2
		echo ">>> FLOWD_DRY_RUN ($tag): would then run: bench_flowd_python proof --manifest $manifest --read-dir $read_dir" >&2
		echo ">>> FLOWD_DRY_RUN ($tag): NOTHING PROVEN -- dry run, no probe opened, no comparison made." >&2
		rm -f "$cmdfile"
		return 12
	fi

	# SETTLE DELAY + BOUNDED CONNECT RETRY (alp-sdk#2233 review item 8). Every
	# caller invokes this proof right after `RSetType 2; r; g` (or, on
	# flash-update-log-dual.sh/flash-update-log-firewall-probe.sh, after a
	# `loadbin`'s own implicit reset) -- AP[3] (the M55 debug AP) can be absent
	# until the SES has actually finished re-booting the image, matching the
	# `-- 8/8 halts ... 0/8 halts after a reset` measurement flash-jlink-mramxip.sh's
	# own header records for the identical AP[3]-after-reset window. The OLD
	# post-boot RAM-console reads across these scripts used a flat `sleep 3`
	# for the same reason; this keeps that settle delay and ADDS a bounded
	# retry -- but ONLY on a failed CONNECT (bench_jlink_assert_connected's
	# job is exactly "did we reach the target", nothing about content). A
	# byte MISMATCH from a session that DID connect is never retried -- that
	# is flowd_sector_pad.py proof's PASS/FAIL verdict, called exactly once
	# below, after a connect has already succeeded.
	sleep 3
	local out rc attempt
	attempt=1
	while :; do
		out="$(mktemp "${TMPDIR:-/tmp}/flowd-proof.out.XXXXXX")" || {
			rm -f "$cmdfile"
			return 1
		}
		bench_jlink_run -nogui 1 -CommanderScript "$cmdfile" >"$out" 2>&1 || true
		# alp-sdk#2233 review round 3, BLOCKER: `rc=$?` used to sit AFTER a
		# separate `if cmd; then break; fi` statement. When a compound `if`
		# takes no branch (condition false, no else), ITS OWN exit status is
		# 0 -- NOT the condition command's -- so that `rc=$?` always read 0,
		# and exhausting all 3 retries fell through to `return "$rc"` with
		# rc=0: a proof whose every connect attempt FAILED still returned
		# success. Verified: `if false; then :; fi; echo $?` prints 0.
		# Capture the assertion's OWN status directly, on the same line, so
		# nothing can sit between the command and reading `$?`.
		bench_jlink_assert_connected "$out" "$tag post-write proof read (attempt $attempt/3)"
		rc=$?
		if [ "$rc" -eq 0 ]; then
			# alp-sdk#2233 review round 5: require ONE bench-measured savebin
			# SUCCESS line (see bench_flowd_read_sectors' identical check,
			# same bench measurement) per `savebin` actually IN THIS
			# CommandFile -- not a retried check (a connected session with
			# an incomplete read is a content problem, not a "did we reach
			# the target" one; same reasoning as the byte-mismatch case
			# above, which is also never retried).
			local expected_savebins got_savebins
			expected_savebins=$(grep -cE '^savebin ' "$cmdfile")
			got_savebins=$(grep -cE 'Reading [0-9]+ bytes from addr 0x[0-9A-Fa-f]+ into file.*O\.K\.' "$out")
			if [ "$got_savebins" -lt "$expected_savebins" ]; then
				echo "bench-env: bench_flowd_proof ($tag): expected $expected_savebins savebin success" >&2
				echo "           line(s), found $got_savebins in the transcript -- refusing to trust" >&2
				echo "           this read-back." >&2
				echo "           transcript kept for inspection: $out" >&2
				rm -f "$cmdfile"
				return 1
			fi
			rm -f "$out"
			break
		fi
		if [ "$attempt" -ge 3 ]; then
			rm -f "$cmdfile"
			# alp-sdk#2233 review round 3, item 12: every earlier attempt's
			# $out is removed below (nothing to learn from a retry that was
			# superseded); THIS one -- the terminal failure -- is kept as
			# evidence for whoever reads the abort message.
			echo "bench-env: bench_flowd_proof ($tag): giving up after $attempt/3 connect attempts --" >&2
			echo "           transcript kept for inspection: $out" >&2
			return "$rc"
		fi
		echo "bench-env: bench_flowd_proof ($tag): connect attempt $attempt/3 failed -- the SES may still be" >&2
		echo "           mid-boot (AP[3] not yet up); retrying after a short settle." >&2
		rm -f "$out"
		attempt=$((attempt + 1))
		sleep 2
	done
	rm -f "$cmdfile"

	bench_flowd_python proof --manifest "$manifest" --read-dir "$read_dir"
}
