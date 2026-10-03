"""RV-3028-C7 clock set + backup switchover, through the kernel's rtc-rv3028 driver.

The driver (Linux 6.1) owns every EEPROM access: it enters EERD, edits the register and
commits the byte to the configuration EEPROM itself. This module only asks it to:

* trickle charger: not here. It is the devicetree property ``trickle-resistor-ohms`` on the
  ``rtc@52`` node (the driver writes TCE + TCR at probe); this module only reads it back.
* backup switchover mode: ``RTC_PARAM_SET`` / ``RTC_PARAM_BACKUP_SWITCH_MODE`` on /dev/rtc0
  (``rv3028_param_set``); no devicetree property exists for it in 6.1.

Register 0x37 ("EEPROM Backup"), RV-3028-C7 Application Manual Rev. 1.4 (Nov 2021), section
"EEPROM BACKUP REGISTER, 37h" (p. 38-39) and 4.2 / 4.3 (p. 45-48):
    bit 5    TCE   trickle charger enable (0 = disabled, default)
    bits 3:2 BSM   00 / 10 = switchover disabled (00 default), 01 = Direct (DSM),
                   11 = Level Switching Mode (LSM, switch when VDD < 2.0 V and VBACKUP > 2.0 V)
    bits 1:0 TCR   series resistance 00 = 3 k, 01 = 5 k, 10 = 9 k, 11 = 15 k (3 k default)
"""
from __future__ import annotations

import shlex
import time

from provision import functest
from provision import linux_target as lt
from provision.bench import BenchError

REG_BACKUP = 0x37
BSM_DISABLED, BSM_DSM, BSM_LSM = 0, 1, 3          # register field values (bits 3:2)
BSM_NAMES = {0: "disabled", 1: "direct", 2: "disabled", 3: "level"}
TCR_OHMS = (3000, 5000, 9000, 15000)
MAX_ERROR_S = 30                                  # RTC vs host UTC, same bound as rtc_max_error_s

# include/uapi/linux/rtc.h: RTC_PARAM_SET = _IOW('p', 0x14, struct rtc_param) = 0x40187014,
# struct rtc_param { u64 param; u64 uvalue; u32 index; u32 pad; } = 24 bytes,
# RTC_PARAM_BACKUP_SWITCH_MODE = 2, RTC_BSM_LEVEL = 2 (the driver maps it to BSM = 0b11).
_BSM_LEVEL_PY = ("import fcntl,os,struct;fd=os.open('/dev/rtc0',os.O_RDONLY);"
                 "fcntl.ioctl(fd,0x40187014,struct.pack('QQII',2,2,0,0))")


def decode(reg: int) -> dict[str, str]:
    """Register 0x37 -> the three ledger-facing values."""
    return {"rtc_rv3028_reg_0x37": f"{reg:#04x}",
            "rtc_backup_switch_mode": BSM_NAMES[(reg >> 2) & 3],
            "rtc_trickle": f"{TCR_OHMS[reg & 3] // 1000} kOhm" if reg & 0x20 else "disabled"}


def read_backup(t, bus: int, addr: int) -> int:
    return lt.i2c_get(t, bus, addr, REG_BACKUP)


def enable_backup(t, bus: int, addr: int) -> int:
    """Level switching mode on, through the driver. Skipped when it is already on (the driver
    commits to the EEPROM on every write, which has a finite endurance). Returns register 0x37."""
    reg = read_backup(t, bus, addr)
    if (reg >> 2) & 3 != BSM_LSM:
        t.run(f"python3 -c {shlex.quote(_BSM_LEVEL_PY)}")
        reg = read_backup(t, bus, addr)
        if (reg >> 2) & 3 != BSM_LSM:
            raise BenchError(f"backup switchover not enabled after RTC_PARAM_SET: reg 0x37 = {reg:#04x}")
    return reg


def set_time(t, epoch: int) -> None:
    """System time from the provisioning host (UTC), then ``hwclock -w`` (RTC from system time)."""
    t.run(f"date -u -s @{int(epoch)}")
    if t.run("hwclock --systohc --utc", check=False).rc != 0:     # no hwclock on the image
        functest.rtc_set(t)                                       # the same ioctl, from the host clock


def rtc_epoch(t) -> int:
    """The RTC's time as the driver reads it; BenchError when the clock is unset (power-on flag)."""
    out = t.run("cat /sys/class/rtc/rtc0/since_epoch", check=False)
    if out.rc != 0 or not out.stdout.strip().isdigit():
        raise BenchError("RTC unreadable (the power-on flag is up: nothing has set the clock)")
    return int(out.stdout.strip())


def verify_time(t, host_epoch: float | None = None) -> int:
    """Read the clock back; returns |RTC - host| in seconds, BenchError beyond MAX_ERROR_S."""
    err = abs(rtc_epoch(t) - int(time.time() if host_epoch is None else host_epoch))
    if err > MAX_ERROR_S:
        raise BenchError(f"RTC is {err} s from the host clock after setting it")
    return err
