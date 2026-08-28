#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# One pass over the V2N/V2M measurements that several v0.17.0 issues are waiting
# on.  Every step is READ-ONLY: it powers the board, reads identifiers, and
# writes nothing to flash, fuses or devicetree.
#
# WHY THIS EXISTS: those issues are not blocked on analysis, they are blocked on
# four numbers nobody has read off the hardware.  Each was previously "needs a
# bench" with no statement of what to actually measure, so a bench session
# rediscovered the question instead of answering it.  Run this, paste the
# output, and the issues can be closed or re-scoped from evidence.
#
#   #1369 / #1440  the GD32 SW-DP IDR -- two values are in circulation
#                  (0x0BE12477 vs 0x6BA02477) and NEITHER is attested on a GD32
#   #1230          E1M-V2M101 dram_mbit / flash_mbit vs the actual module
#   #1163          BRD_I2C 0x48: a TMP112 and a TPS628640 are both declared there
#
# PREREQUISITES -- verified absent on 2026-08-28, so check them first:
#
#   1. The V2N-M1 board must be CONNECTED to its DPS-150 output.  It was not:
#      the supply read `power: on, voltage: 15.0, current: 0.0` while SWD
#      independently reported `VTref=0.000V`.  Trust the CURRENT reading, not the
#      `power` field -- zero current with the output live means nothing is drawing.
#   2. The GD32 J-Link must be ATTACHED.  Only one probe was present
#      (USB 3-4.1, serial 000600107451 = the V2N CM33 DAP).  Without the GD32
#      probe, step 1 below cannot run at all -- which IS the #1369 blocker.
#
# Usage (on alplab-gw):
#     bash scripts/bench/v2n/measure-blocked-issues.sh 2>&1 | tee /tmp/v2n-measure.log
set -uo pipefail

PLACE="${PLACE:-e1mx-v2n-m1-01}"
export LG_COORDINATOR="${LG_COORDINATOR:-100.64.0.1:20408}"
export PATH="$HOME/.local/bin:$PATH"
PWRENV="${PWRENV:-$HOME/board-farm/labgrid/pwr-only.yaml}"

say() { printf '\n=== %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }

# ---------------------------------------------------------------- reservation
say "reservation"
labgrid-client -p "$PLACE" acquire || warn "acquire failed (already held?)"
acq=$(labgrid-client -p "$PLACE" show 2>/dev/null | sed -n 's/^  acquired: //p' | head -1)
printf '    acquired: %s\n' "${acq:-NONE}"
if [ -z "$acq" ] || [ "$acq" = "None" ]; then
    warn "NOT HELD -- refusing to touch the board.  Someone else may be on it."
    exit 3
fi
cleanup() { say "release"; labgrid-client -p "$PLACE" release || true; }
trap cleanup EXIT

# ---------------------------------------------------------------------- power
say "power (V2N Vin is 15.0 V -- NOT the AEN's 16.0 V)"
LG_ENV="$PWRENV" labgrid-client -p "$PLACE" power on || warn "power on failed"
sleep 8
tele="/run/alplab/$PLACE.json"
if [ -r "$tele" ]; then
    cat "$tele"; echo
    cur=$(python3 -c "import json,sys;print(json.load(open('$tele')).get('current'))" 2>/dev/null)
    printf '    current draw: %s A\n' "$cur"
    case "$cur" in
        0|0.0|0.00|None)
            warn "CURRENT IS ZERO -- the board is not drawing from the supply."
            warn "That is the 2026-08-28 state: physically disconnected."
            warn "Reconnect it before trusting anything below."
            ;;
    esac
fi

# ------------------------------------------------- 1. GD32 SW-DP IDR (#1369)
say "1. GD32 SW-DP IDR  (#1369, #1440)"
# JLinkExe selects ONLY by serial and has no USB-port selector, and the GD32
# probe enumerates with the SAME OEM-cloned serial 603000869 as the AEN E8
# probe.  So a bare -SelectEmuBySN is ambiguous and can land on a different
# board.  OpenOCD can pin by USB path, which is why it is used here.
printf '    probes present:\n'
for d in /sys/bus/usb/devices/*/; do
    v=$(cat "$d/idVendor" 2>/dev/null)
    [ "$v" = "1366" ] && printf '      %s serial=%s\n' \
        "$(basename "$d")" "$(cat "$d/serial" 2>/dev/null)"
done
if command -v openocd >/dev/null 2>&1; then
    for path in 3-4.2 3-4.1; do
        printf '    -- USB path %s\n' "$path"
        timeout 30 openocd \
            -c "adapter driver jlink" \
            -c "adapter usb location $path" \
            -c "transport select swd" \
            -c "adapter speed 1000" \
            -c "init; dap info 0; shutdown" 2>&1 \
          | grep -iE "IDCODE|IDR|DPIDR|Info : .*dap|Error" | sed 's/^/       /'
    done
    echo "    EXPECTED: the V2N CM33 DAP answers one value and the GD32 another."
    echo "    Record WHICH PATH gave WHICH value -- that pairing is the whole point"
    echo "    of #1369, and neither circulating value is currently attested."
else
    warn "openocd not found -- install it, or the probe cannot be pinned by USB path"
fi

# ------------------------------------------- 2. V2M memory sizes (#1230)
say "2. E1M-V2M101 dram_mbit / flash_mbit  (#1230)"
cat <<'NOTE'
    Needs the board booted to Linux.  From its console:

        # DRAM the kernel actually got:
        grep MemTotal /proc/meminfo
        dmesg | grep -iE "Memory:|DRAM|memblock"
        # eMMC/flash size:
        lsblk -b -o NAME,SIZE,TYPE
        cat /sys/block/mmcblk0/size        # in 512-byte sectors

    Compare against metadata/e1m_modules/E1M-V2M101.yaml's dram_mbit/flash_mbit.
    Report BOTH numbers; the issue says they contradict the module, and the
    metadata is what every downstream consumer trusts.
NOTE

# ------------------------------------------- 3. BRD_I2C 0x48 (#1163)
say "3. BRD_I2C 0x48 -- TMP112 vs TPS628640  (#1163)"
cat <<'NOTE'
    BLOCKED BY #1226 on the current board revision: i2c-8 is electrically wedged
    ("i2c i2c-8: SCL is stuck low, exit recovery"), and even the ACT88760 PMIC at
    0x25/0x26 -- definitely populated, since it is powering the SoC running the
    scan -- fails to ACK.  An EMPTY i2cdetect grid on this bus is a BUS FAULT,
    not evidence of absence, and reading it as absence is the specific mistake
    #1226 warns about.

    Only if the bus is confirmed healthy (the PMIC ACKs at 0x25/0x26):

        i2cdetect -y -r 8

    Then read the device at 0x48 and identify it:
        # TMP112: 16-bit temperature in reg 0x00, config in 0x01
        i2cget -y 8 0x48 0x00 w
        # TPS628640: a buck, different register map entirely
    Report the raw bytes, not your conclusion -- the point of the issue is that
    one of the two declarations is wrong, and the bytes decide which.
NOTE

say "done -- paste this log onto the issues it answers"
