# aen-bmi323-regcheck -- BMI323 register read-back settlement

On-silicon **diagnostic instrument**, not a driver fix, for the E1M-AEN801 /
E1M-AEN803 (Alif Ensemble E8, M55-HE), bench RAM-run via J-Link.
`chips/bmi323/bmi323.c` is untouched by this app.

## The problem it settles

On E1M-AEN803 serial 2026W36-0002, `examples/peripheral-io/i2c-device-hub`
reports:

```
[devhub] BMI323 @0x68 id=0x43 accel{-32768,-32768,-32768} cfg_rc=0 rs=0 STALE/RESET DATA
```

`bmi323_set_accel()`'s `acc_mode` write is datasheet-correct (ACC_CONF
bits[14:12] = `0b100`, BST-BMI323-DS000-13 Rev 1.7 pp.21-22, 61, 88-91) and
the compiled object provably contains it, yet every accel axis reads
`0x8000` -- the documented invalid/no-sample sentinel (Rev 1.7, Table 2 p.9).

`id=0x43` proves nothing about writes: CHIP_ID's reset value is already
`0x0043` (Rev 1.7, Table 36 p.61), readable straight out of POR with no
reset at all. A part that ACKs every write without applying any of them
reads back an identical CHIP_ID. Nobody had ever read a register back after
configuring it -- this app does.

## What it does

Configures the accelerometer through the **same public driver calls**
`i2c-device-hub` uses -- `bmi323_init()`, `bmi323_set_accel(ODR_100_HZ,
FS_2G)`, the same wait -- then reads back three registers, in a mandatory
order, before ever touching the data registers:

1. `ACC_CONF` (0x20) -- the discriminator: did the acc_mode write land?
2. `ERR_REG` (0x01) -- bit0 `fatal_err`, bit5 `acc_conf_err`.
3. `STATUS` (0x02) -- bit7 `drdy_acc`. bit0 (`por_detected`) reads 0 here,
   always -- see below, that's expected, not a bug in this app.
4. Only *then* `bmi323_read_accel()`.

**Order is not negotiable -- for `drdy_acc`.** `STATUS` bits are
clear-on-read (Rev 1.7 p.66), and reading `ACC_DATA_X..Z` *also* clears
`drdy_acc`, independently of a `STATUS` read (Rev 1.7 p.23: "The flag
STATUS.drdy_acc is cleared when any of the registers ACC_DATA_X to
ACC_DATA_Z is read."). Reading the data registers first would destroy the
evidence this app exists to capture.

`por_detected` (`STATUS` bit0) is a different story. It is *not* this app's
own `STATUS` read that consumes it: `bmi323_init()`
(`chips/bmi323/bmi323.c`, #2035) already runs Bosch's own
device-initialisation status test and reads `STATUS` itself, before this
app ever gets a turn. The bit is clear-on-read (BST-BMI323-DS000-13
Rev 1.7 p.66), so it is gone from the part the moment `init()` reads it --
this app's own `STATUS` reads only ever see it as 0, whatever `init()`
actually observed. (Measured on this bus: one raw `STATUS` read came back
`0x0021`, bit0 set; the next came back `0x00a0`, bit0 clear -- clear-on-read
caught in the act.) What `init()` saw is still available, because `init()`
stashes it before deciding whether to fail on it: `bmi323_was_por_detected()`
reports that stashed value, and this app calls it right after
`bmi323_init()` -- that printed line, not `STATUS`'s own bit0, is the
trustworthy `por_detected` reading.

`bmi323_init()`'s return code is also split (#2035): `ALP_ERR_NOT_READY`
means its own `por_detected` read came back clear (the soft reset was never
confirmed); `ALP_ERR_IO` means a `CHIP_ID` read failure/mismatch or
`ERR_REG.fatal_err`. This app decodes both.

`ACC_CONF` is also sampled once *before* `bmi323_set_accel()` runs (a
genuine before/after), and `STATUS` is sampled a second time ~100 ms after
the first ~17 ms sample -- free evidence for "the wait is too short" versus
"the write never took effect".

## How registers are read

`chips/bmi323/bmi323.c`'s `reg_read_u16()` is `static` -- the driver has no
public raw-register accessor -- so this app talks to the part directly via
`alp_i2c_write_read()` at `EVK_I2C_ADDR_BMI323` (0x68). The BMI323 read
protocol prepends **two dummy bytes** ahead of the requested data on every
read (BST-BMI323-DS000-13 Rev 1.7, Table 53 p.204): for one 16-bit register
this app requests **4 bytes** and keeps only the last 2. Getting that
discard count wrong produced a false `0x0000` CHIP_ID reading earlier in
this investigation (see `bmi323_init()`'s comment in
`chips/bmi323/bmi323.c`) -- see `src/main.c`'s `read_reg16()`.

## ACC_CONF interpretation

| Value | Meaning |
|---|---|
| `0x4008` | The write landed; driver exonerated, fault is elsewhere. |
| `0x0028` | Reset value (Rev 1.7 p.88) -- no write landed, or the part reset afterwards. |
| `0x1008` | Pre-fix encoding (`0b001<<12`) -- would mean a stale binary (already ruled out here). |
| byte-swapped | A write-path (or read-back) endianness bug. |

## Verdict

`RESULT PASS` means every read-back **transaction** completed (`ALP_OK`)
and its value was printed + decoded -- even if the decoded value proves the
accelerometer is broken; this app's job is to measure, not to vindicate the
sensor. `RESULT FAIL` means a read-back itself failed at the bus level
(NACK/timeout), printed with its `rc`.

## Bus

BMI323 (U13) sits on the carrier bus -- SoC I2C0, portable alias `alp-i2c0`
(`ALP_E1M_I2C0` / `EVK_I2C_BUS_SENSORS`) -- at 7-bit address 0x68
(`EVK_I2C_ADDR_BMI323`), confirmed on E1M-AEN803 serial 2026W36-0002 with no
0x69 collision on this respin batch. The board layer already enables this
bus; this app's overlay carries only the bench ITCM retarget.

## Build (bench)

```
ZEPHYR_BASE=<zephyr-base> west build \
  -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-bmi323-regcheck -- \
  "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
  -DEXTRA_DTC_OVERLAY_FILE=examples/aen/aen-bmi323-regcheck/boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay
```

RAM-run app (load into ITCM over J-Link); console is the RAM console
(`ram_console_buf`) or the E1M edge UART0 depending on the bench, see
`prj.conf`. Look for the single `RESULT PASS` / `RESULT FAIL` line.
