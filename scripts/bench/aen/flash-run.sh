#!/usr/bin/env bash
# scripts/bench/aen/flash-run.sh <build-dir> [post_boot_read_bytes_hex]
#
# Cross-platform scope: Linux-side bench helper (sources bench-env.sh;
# drives the Alif SETOOLS over the SE-UART + JLinkExe, both Linux
# binaries on this bench). Runs under WSL2 on Windows (with USB serial
# passed through). See docs/aen-bench-bringup.md.
#
# FLOW A -- PROPER FLASH validation (not RAM-run): write the ITCM-linked image to
# MRAM via SETOOLS as a signed direct-ATOC, let the SES boot it, then read the RAM
# console over SWD (J-Link attach only -- NO loadbin/setpc; the SES booted the app).
#   - loadAddress 0x58000000 = the M55-HE ITCM global alias the SES loads to.
#   - the app must keep CONFIG_RAM_CONSOLE so the result is observable over SWD
#     (the app UART is not on USB on this bench).
#
# SETOOLS is license-gated and is NOT redistributed by alp-sdk: export SETOOLS_DIR
# (and obtain SETOOLS from Alif) before running this. Export SE_UART to the SE-UART
# serial device (host-specific). See README.md.
set -e

# shellcheck source=scripts/bench/aen/bench-env.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/bench-env.sh"

BD="$1"
SIZE="${2:-0x500}"
bench_require_setools || exit $?
if [ -z "${SE_UART:-}" ]; then
	echo "flash-run: SE_UART is unset — export it to the SE-UART serial device" >&2
	echo "           (Linux: /dev/ttyUSB*, macOS: /dev/cu.usbserial-*, Windows/WSL: passed-through COM)." >&2
	exit 2
fi
SET="$SETOOLS_DIR"
OBJ="$(bench_tool_prefix)" || exit $?
JLINK="$(bench_jlink_exe)" || exit $?
NAME=$(basename "$BD")
BIN="$BD/zephyr/zephyr.bin"
ELF="$BD/zephyr/zephyr.elf"
BUF=0x$($OBJ-nm "$ELF" | awk '/ ram_console_buf$/{print $1}')

# 1. stage the image + a per-app signed-ATOC config (keeps the factory DEVICE cfg)
cp -f "$BIN" "$SET/build/images/$NAME.bin"
cat > "$SET/build/config/$NAME.json" <<JSON
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "ALP-HE":  { "disabled": false, "binary": "$NAME.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HE", "loadAddress": "0x58000000", "flags": ["load", "boot"] }
}
JSON

cd "$SET"
echo ">>> FLASH $NAME  (ram_console_buf=$BUF)" >&2
./app-gen-toc -f "build/config/$NAME.json" >/tmp/gentoc.log 2>&1 || { echo "gen-toc FAILED"; tail /tmp/gentoc.log; exit 1; }
# 2. write to MRAM over the SE-UART (SES auto-enters maintenance, burns, resets+boots)
./app-write-mram -c "$SE_UART" -p >/tmp/wrmram.log 2>&1 || true
if grep -q "Done" /tmp/wrmram.log; then echo "MRAM write: Done ($(grep -oE '[0-9]+\.[0-9]+ seconds' /tmp/wrmram.log | tail -1))"; else echo "MRAM write FAILED:"; tail -5 /tmp/wrmram.log; exit 1; fi

# 3. SES has booted the app; attach J-Link read-only and dump the RAM console
cat > /tmp/flash-read.jlink <<EOF
connect
halt
mem8 $BUF, $SIZE
qc
EOF
$JLINK -device "$JLINK_DEVICE_READ" -if SWD -speed "$JLINK_SPEED" -nogui 1 -CommanderScript /tmp/flash-read.jlink 2>/tmp/fr.err > /tmp/fr.out || true
echo "----- $NAME RAM console (MRAM-flashed, SES-booted) -----"
awk '/^[0-9A-Fa-f]+ = / { for (i=3;i<=NF;i++){ if ($i !~ /^[0-9A-Fa-f][0-9A-Fa-f]$/) continue; b=strtonum("0x"$i); if(b==0){nul++; if(nul>4)exit; next} nul=0; if(b==10||b==13){printf "\n";continue} if(b>=32&&b<127)printf "%c",b } }' /tmp/fr.out
echo; echo "--------------------------------------------------------"
