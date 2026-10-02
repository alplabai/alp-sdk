# SPDX-License-Identifier: Apache-2.0
"""DX-M1 NPU firmware programming over the ROM's UART (XMODEM) path (V2M only).

On-target orchestration for the ``dxm1_npu_flash`` step (steps.Dxm1NpuFlash):
bundle artefacts, the release-DTB swap that lets the DX-M1 UART0 appear on
the SoC, the background ``dxflash.py`` run with the PA6 reset pulse, and the
ROM-output classification (a wrong BOOT_CFG strap reads differently from a
transfer failure). The reboot / power cycle between the stages is the
step's (it owns the console); nothing here touches the PSU.

Everything here is a function of a LinuxTarget, so tests drive it with a fake.
The firmware binaries are license-gated and never committed: they ride in the
alp-sdk-internal release bundle (``dxm1_*`` component roles, see
metadata/schemas/som-release-bundle-v1.schema.json). See docs/provisioning-v2n.md.
"""

from __future__ import annotations

import re
import shlex
import time
from pathlib import Path

from provision import linux_target as lt
from provision.bench import BenchError

# bundle component roles (all optional; the step is skipped without them)
ROLE_FW = "dxm1_fw"                   # fw_no_pmic_gpio.bin
ROLE_UART_BOOT = "dxm1_fw_uart_boot"  # fw_uart_boot_no_pmic_gpio.bin
ROLE_DXFLASH = "dxm1_dxflash"         # dxflash.py
ROLE_DTB = "dxm1_dtb"                 # dxuart2 DTB: sci1-dx pinctrl, serial@12801000 okay, pcie@13400000 disabled
ROLE_DXCLI = "dxm1_dxcli"             # dxcli.py, only needed to erase a NAND that already holds boot2nd
REQUIRED_ROLES = (ROLE_FW, ROLE_UART_BOOT, ROLE_DXFLASH, ROLE_DTB)

# V2N pinctrl: P75 = DX-M1 UART0 mux (sysfs 477), PA6 = M1_RESET (sysfs 502)
DEFAULT_GPIO_CHIP = "10410000.pinctrl"
DEFAULT_UART_MUX_LINE = 61
DEFAULT_RESET_LINE = 86

RESET_AFTER_S = 2.0       # dxflash must be listening before the reset
RESET_HOLD_S = 0.5
DXFLASH_TIMEOUT_S = 600.0   # a good run is 400 s+: dxflash sits out a 240 s window after SENT fw.bin
EXIT_GRACE_S = 30.0         # success markers seen: how long the process may take to exit by itself
POLL_S = 3
PROC = "[/]tmp/dxflash.py"   # bracketed so pkill/pgrep never match their own shell
_clock = time.monotonic     # tests drive a fake clock through these two
_sleep = time.sleep
# the proven manual dx_update.sh step 1 (boot2nd CLI over ttySC1): erase, read back, dump;
# "@N" is dxcli's per-command timeout in seconds
DXCLI_COMMANDS = ("sf_erase 0 1000000@240", "sf_read 0x40000000 0x200000 0x10@5", "md 0x40000000 4")
BLANK_RE = re.compile(r"40000000: ffffffff")
DXCLI_TIMEOUT_S = 480.0

REMOTE = {ROLE_FW: "/tmp/dx_fw.bin", ROLE_UART_BOOT: "/tmp/dx_uart_boot.bin",
          ROLE_DXFLASH: "/tmp/dxflash.py", ROLE_DTB: "/tmp/dxuart2.dtb", ROLE_DXCLI: "/tmp/dxcli.py"}
LOG, RC = "/tmp/dxflash.log", "/tmp/dxflash.rc"
UART_SYSFS = "/sys/bus/platform/devices/12801000.serial"   # exists only with serial@12801000 okay

# ROM console while the straps are wrong (BOOT_CFG not mode 0): it keeps trying PCIe
# boot and never offers XMODEM.
STRAP_FAIL_RE = re.compile(r"pcie boot\(\d/3\) failed \(0x1\) PWD:")
XMODEM_RE = re.compile(r"(?im)xmodem|(?:^|\s)C+(?:\s|$)")
STRAP_MSG = ("DX-M1 ROM printed 'pcie boot(n/3) failed (0x1) PWD:' and never offered XMODEM: the "
             "BOOT_CFG straps are not mode 0. E1M IO17, IO19 and IO20 must be LOW at the DX-M1 "
             "reset. The X-EVK pulls them up, so the carrier needs the rework (1k to GND on each); "
             "the tool cannot measure the straps. Rework the carrier, then re-run dxm1_npu_flash.")


class StrapError(Exception):
    """The ROM output of a wrongly strapped DX-M1: operator action, not a retry."""


def gpio_config(raw: dict) -> tuple[str, int, int]:
    """(chip label, P75 line, PA6 line) from bench.yaml ``dxm1:`` over the board defaults.
    ValueError for the DEEPX 0.75 V rail lines, non-int or equal lines, before any export."""
    d = raw.get("dxm1") or {}
    chip = d.get("gpio_chip") or DEFAULT_GPIO_CHIP
    mux = d.get("uart_mux_line", DEFAULT_UART_MUX_LINE)
    rst = d.get("reset_line", DEFAULT_RESET_LINE)
    for key, line in (("uart_mux_line", mux), ("reset_line", rst)):
        if not isinstance(line, int) or isinstance(line, bool):
            raise ValueError(f"bench.yaml dxm1.{key} must be an int gpiochip line number, got {line!r}")
        if line in lt.DXM1_REFUSED_GPIO_LINES:
            raise ValueError(f"bench.yaml dxm1.{key} = {line} is {lt.DXM1_REFUSED_GPIO_LINES[line]} "
                             "(the DEEPX 0.75 V rail): gpiolib reconfigures a pin on read and would "
                             "kill the rail; refusing")
    if mux == rst:
        raise ValueError(f"bench.yaml dxm1.uart_mux_line == dxm1.reset_line ({mux}); "
                         "these must be distinct gpiochip lines")
    return chip, mux, rst


def push(t, local: Path, remote: str) -> str:
    """Copy one file to the target and verify its md5 there (returns it)."""
    t.put(local, remote)
    want = lt._host_md5(local)
    if (got := t.md5(remote)) != want:
        raise BenchError(f"{remote}: copy on the target ({got}) does not match {local.name} ({want})")
    return want


def unexport_gpios(t, chip: str, lines: tuple[int, ...]) -> str:
    """Best-effort `unexport` of the DX-M1 lines (a failure must not leave P75 / PA6
    exported high). A line the pinctrl driver owns refuses; that is reported, not raised."""
    try:
        base = lt._pinctrl_chip_base(t, chip)
    except BenchError as e:
        return f"GPIOs left exported ({e})"
    bad = [str(base + n) for n in lines
           if t.run(f"echo {base + n} > /sys/class/gpio/unexport", check=False).rc != 0]
    return f"GPIO {', '.join(bad)} could not be unexported" if bad else "P75/PA6 unexported"


def install_dtb(t, dtb_name: str, new: Path, st: dict | None = None) -> str:
    """Back the release DTB up, install ``new`` (already pushed to REMOTE[ROLE_DTB])
    over it, sync. Returns the release md5. ``st["release"]`` is set the moment the
    backup is verified, BEFORE the live DTB is overwritten, so a failed copy over it
    can still be healed by the caller.

    A ``<dtb>.release`` that is already there is an interrupted earlier run (a finished
    run removes it after restoring): its content IS the release DTB, so it is restored
    first -- never backed up over."""
    live, bak = f"/boot/{dtb_name}", f"/boot/{dtb_name}.release"
    if t.run(f"test -e {shlex.quote(bak)}", check=False).rc == 0:
        t.run(f"cp -p {shlex.quote(bak)} {shlex.quote(live)} && sync")
        if t.md5(live) != t.md5(bak):
            raise BenchError(f"{live} does not match {bak} after healing an interrupted run")
    release = t.md5(live)
    if release == lt._host_md5(new):
        raise BenchError(f"{live} already is the dxuart2 DTB and there is no {bak}: restore the "
                         "release DTB by hand, the tool cannot know which one is the release")
    t.run(f"cp -p {shlex.quote(live)} {shlex.quote(bak)} && sync")
    if t.md5(bak) != release:
        raise BenchError(f"backup {bak} does not match {live}")
    if st is not None:
        st["release"] = release
    t.run(f"cp {shlex.quote(REMOTE[ROLE_DTB])} {shlex.quote(live)} && sync")
    if t.md5(live) != lt._host_md5(new):
        raise BenchError(f"{live} does not hold the dxuart2 DTB after the copy")
    return release


def restore_dtb(t, dtb_name: str, release_md5: str) -> None:
    """Put the release DTB back, verify its md5, drop the backup."""
    live, bak = f"/boot/{dtb_name}", f"/boot/{dtb_name}.release"
    t.run(f"cp -p {shlex.quote(bak)} {shlex.quote(live)} && sync")
    got = t.md5(live)
    if got != release_md5:
        raise BenchError(f"release DTB restore FAILED: {live} md5 {got} != {release_md5}; "
                         f"the unit boots the dxuart2 DTB (PCIe off) until {bak} is copied back")
    t.run(f"rm -f {shlex.quote(bak)} && sync", check=False)


def check_dxuart2_booted(t) -> None:
    """After the warm reboot: the unit really runs the dxuart2 DTB (serial@12801000 okay,
    pcie@13400000 disabled). A md5 of /sys/firmware/fdt would not do: U-Boot fixes the tree
    up (chosen, memory) before the kernel sees it. Driving P75 / PA6 and erasing the NAND
    on the release DTB would hit a UART that is not there."""
    if t.run(f"test -d {UART_SYSFS}", check=False).rc != 0:
        raise BenchError(f"the dxuart2 DTB did not boot: {UART_SYSFS} is absent after the warm reboot "
                         "(serial@12801000 is not enabled)")
    if dev := lt.dxm1_pcie_device(t):
        raise BenchError(f"the dxuart2 DTB did not boot: the DX-M1 PCIe device ({dev}) is still enumerated "
                         "(pcie@13400000 should be disabled)")


def erase_nand(t, chip: str, mux: int, rst: int) -> str:
    """dx_update.sh step 1: P75 + PA6 high (SCI1 to the DX-M1, reset released), then the
    boot2nd CLI through dxcli.py: sf_erase 0 1000000, sf_read the first RTOS bytes, md.
    Only for a NAND whose boot2nd is running, so NO PROMPT is a failure here (the DX-M1
    needs a cold cycle), as in the script."""
    lt.dxm1_drive_high(t, chip, mux)
    lt.dxm1_drive_high(t, chip, rst)
    args = " ".join(shlex.quote(c) for c in DXCLI_COMMANDS)
    r = t.run(f"python3 -u {REMOTE[ROLE_DXCLI]} {args}", timeout=DXCLI_TIMEOUT_S, check=False)
    out = (r.stdout + r.stderr).replace("\r", "")
    if "NO PROMPT" in out:
        raise BenchError("dxcli: NO PROMPT from the DX-M1 boot2nd CLI; cold-cycle the unit (PSU off, "
                         f"MIN_OFF_S, on) and re-run: ...{out.strip()[-200:]}")
    if r.rc != 0:
        raise BenchError(f"dxcli sf_erase rc={r.rc}: ...{out.strip()[-300:]}")
    if not BLANK_RE.search(out):
        raise BenchError("dxcli sf_erase returned 0 but the RTOS slot is not blank (expected "
                         f"'40000000: ffffffff'): ...{out.strip()[-300:]}")
    return "NAND erased, RTOS slot blank (sf_erase 0 1000000)"


def markers_ok(log: str) -> bool:
    """dxflash's own success lines: the update ended 0, the firmware CRC is good and the
    DX-M1 jumped to its rtos (or dxflash printed its final ### DONE); a bad CRC never passes."""
    return ("update_firmware end. 0" in log and "good CRC" in log and "bad CRC" not in log
            and ("jump to rtos" in log or "### DONE" in log))


def kill_dxflash(t) -> bool:
    """Stop a dxflash.py that is still running and wait until it is gone, so nothing
    tears the GPIOs down under a live flasher. True when it is gone, False when it
    survived even pkill -9. The bracket keeps pkill/pgrep from matching the shell that
    carries this very command line."""
    t.run(f"pkill -f '{PROC}'", check=False)
    for _ in range(10):
        if t.run(f"pgrep -f '{PROC}'", check=False).rc != 0:
            return True
        _sleep(1)
    t.run(f"pkill -9 -f '{PROC}'", check=False)
    _sleep(1)
    return t.run(f"pgrep -f '{PROC}'", check=False).rc != 0


def _kill_or_raise(t) -> None:
    if not kill_dxflash(t):
        raise BenchError("flasher still alive after pkill -9: dxflash.py was not stopped, the GPIOs are "
                         "left exported and the unit needs a cold cycle")


def run_dxflash(t, chip: str, mux: int, rst: int) -> tuple[int | None, str, bool]:
    """P75 + PA6 high, dxflash.py in the background, PA6 pulse after 2 s, then wait on its
    log markers (a good run takes 400 s or more: dxflash always sits out a 240 s log window
    after SENT fw.bin). Returns (real exit code or None when it was killed, log, timed_out).
    A flasher that is still running when we give up (timeout, bad CRC, or lingering after
    its success markers) is killed, and waited for, before this returns."""
    lt.dxm1_drive_high(t, chip, mux)
    lt.dxm1_drive_high(t, chip, rst)
    cmd = (f"python3 {REMOTE[ROLE_DXFLASH]} {REMOTE[ROLE_UART_BOOT]} {REMOTE[ROLE_FW]} "
           f">{LOG} 2>&1; echo $? >{RC}")
    t.run(f"rm -f {LOG} {RC}; nohup sh -c {shlex.quote(cmd)} </dev/null >/dev/null 2>&1 &")
    t.run(f"sleep {RESET_AFTER_S:g}")
    lt.dxm1_reset_pulse(t, chip, rst, RESET_HOLD_S)
    deadline = _clock() + DXFLASH_TIMEOUT_S
    marked_at, timed_out = None, False
    try:
        while True:
            rc = t.run(f"cat {RC} 2>/dev/null", check=False).stdout.strip()
            log = t.run(f"cat {LOG}", check=False).stdout
            if rc.isdigit():
                return int(rc), log, False
            if "bad CRC" in log:
                break
            if markers_ok(log):
                marked_at = _clock() if marked_at is None else marked_at
                if _clock() - marked_at > EXIT_GRACE_S:
                    break
            if _clock() > deadline:
                timed_out = True
                break
            _sleep(POLL_S)
    except BenchError:
        kill_dxflash(t)
        raise
    _kill_or_raise(t)
    return None, t.run(f"cat {LOG}", check=False).stdout, timed_out


def rc_evidence(rc: int | None, log: str, timed_out: bool) -> str:
    """The ledger string for dxflash's exit: the real code, or why it was killed."""
    if rc is not None:
        return str(rc)
    if markers_ok(log):
        return "killed after its success markers"
    return "killed on timeout" if timed_out else "killed (bad CRC)"


def classify(rc: int | None, log: str, timed_out: bool = False) -> str:
    """The success summary; StrapError for a wrongly strapped DX-M1's ROM output;
    BenchError for any other failure. Success = dxflash's success markers and an exit
    code of 0 (None: it printed them but had not exited yet and was stopped)."""
    ok = markers_ok(log)
    # Deliberately BEFORE timed_out: success markers + a clean (or killed-after-markers)
    # exit are what count, even if the clock also ran out.
    if ok and rc in (0, None):
        return "dxflash: update_firmware end. 0, firmware CRC good"
    if STRAP_FAIL_RE.search(log) and not XMODEM_RE.search(STRAP_FAIL_RE.sub("", log)):
        raise StrapError(STRAP_MSG)
    if timed_out:
        raise BenchError(f"dxflash.py did not finish in {DXFLASH_TIMEOUT_S:g} s (killed): ...{log.strip()[-400:]}")
    why = "end marker + CRC seen" if ok else "no 'update_firmware end. 0' with good CRC / jump to rtos"
    code = "killed" if rc is None else rc
    raise BenchError(f"dxflash.py failed: exit code {code}, {why}: ...{log.strip()[-400:]}")
