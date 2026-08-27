<!-- Last verified: 2026-08-06 against alp-sdk.  Both expected-output
     tables are now checked against the BUS the code actually opens, not
     just per-chip addresses: BOARD_I2C_SENSORS resolution traced through
     include/alp/boards/alp_e1m_evk_routes.h and
     include/alp/boards/alp_e1m_x_evk_routes.h, then cross-referenced
     against every device metadata/boards/e1m-evk.yaml `i2c_devices:` and
     metadata/boards/e1m-x-evk.yaml declare on that same bus, plus
     metadata/e1m_modules/E1M-V2N101.yaml `i2c_devices:` (`brd_i2c:` vs
     `e1m_i2c0:`) for the V2N SoM-level parts.  The closing
     `metadata/e1m_modules/<SKU>.yaml`'s `i2c_devices:` pointer names the
     AEN family's real per-chip source, via E1M-AEN801.yaml's own
     `i2c_devices:` block (`brd_i2c:` vs `e1m_i2c0:`).  The V2N port
     instructions are re-verified against a real
     `scripts/alp_project.py --emit zephyr-conf` run: a
     `som.sku`-only swap of the i2c-scanner's board.yaml fails closed at
     each of preset/pins/cores (ALP-B007, then an unknown-pad error, then
     an unknown-core-id error) until all three are updated together. -->

# 02 -- I²C bus scan

Walks `examples/peripheral-io/i2c-scanner/` -- a 7-bit-address scan over an I²C
bus that prints the addresses that ACK.  Useful for confirming on-
module chip populations + smoke-testing a new board's I²C
wiring.

## What it teaches

* How to open an `alp_i2c_t` against the board's primary I²C bus,
  addressed portably through `<alp/board.h>`'s `BOARD_I2C_SENSORS`
  alias rather than a hardcoded bus number.
* The "ACK probe" idiom -- a 1-byte read whose ACK answers "is
  something at this address?"  (Not a zero-byte write -- see below.)
* Why the SDK doesn't try to identify the chip at each address
  (use the chip driver's `_init` for that -- the scanner just
  reports addresses, not identities).

## Code path

```c
#include "alp/peripheral.h"
#include "alp/board.h"     /* BOARD_I2C_SENSORS cross-EVK alias */

int main(void) {
    (void)alp_init();

    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = BOARD_I2C_SENSORS,   /* E1M EVK: ALP_E1M_I2C0 */
        .bitrate_hz = 100000u,
    });
    if (!bus) return -1;

    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        /* 1-byte read, not a zero-byte write: some controllers --
         * e.g. the DesignWare i2c_dw on Alif Ensemble -- put nothing
         * on the bus for a zero-length transfer, so no device ever
         * ACKs and the scan finds nothing.  A 1-byte read (data
         * discarded) is the portable probe across backends. */
        uint8_t scratch;
        if (alp_i2c_read(bus, addr, &scratch, 1) == ALP_OK) {
            printf("0x%02X ACK\n", addr);
        }
    }
    alp_i2c_close(bus);
}
```

## Expected output on E1M-AEN801 + E1M-EVK

`BOARD_I2C_SENSORS` is one shared bus, so the scan ACKs more than just
the on-module chip -- the E1M-EVK carrier populates thirteen more
devices on the same `ALP_E1M_I2C0` (`metadata/boards/e1m-evk.yaml`
`i2c_devices:`).  Expect the full set below, not just the on-module
one, or the troubleshooting step at the bottom of this page will send
you chasing a part that was never on this bus to begin with.

On-module (E1M-AEN801 `i2c_devices:` `e1m_i2c0:` block):

```
0x50 ACK   -- 24C128 EEPROM
```

The SoM's three other I²C parts -- OPTIGA Trust M (`0x30`), TMP112
(`0x48`) and RV-3028-C7 (`0x52`) -- do NOT ACK here.  They sit on
`brd_i2c`, a separate slave-only LPI2C0 housekeeping bus the M55 cannot
master (`metadata/e1m_modules/E1M-AEN801.yaml` `i2c_devices:`
`brd_i2c:`; `docs/bring-up-aen.md` §5.1).  A `0x48` in a scan of this
bus is U32 INA236B (+V_CAM0 rail) on PRE-RESPIN carriers, not the
TMP112.

E1M-EVK board-populated (`metadata/boards/e1m-evk.yaml` `i2c_devices:`):

```
0x20 ACK   -- TCA6408A U35 alt I/O expander (BOM variant -- ACKs INSTEAD
              OF 0x72, not alongside it: R112/R145 are mutually exclusive)
0x40 ACK   -- INA236 U21, +3V3 rail current monitor
0x41 ACK   -- INA236 U31, +1V8 rail current monitor
0x42 ACK   -- INA236 U33, +VIO rail current monitor
0x47 ACK   -- BMP581 U14 barometer
0x49 ACK   -- INA236 U34, +V_CAM1 rail current monitor
0x4A ACK   -- INA236 U30, +5V rail current monitor
0x4B ACK   -- INA236 U32, +V_CAM0 rail current monitor
0x4D ACK   -- TAS2563 U27 smart amp (low-address unit)
0x4E ACK   -- TAS2563 U28 smart amp (high-address unit)
0x68 ACK   -- BMI323 U13 IMU (post-respin boards)
0x69 ACK   -- ICM-42670 U12 IMU (collides with BMI323 on pre-respin
              boards, which mis-strap U13 to 0x69 too -- BENCH-CONFIRMED
              2026-06-16)
0x71 ACK   -- TCAL9538 U37 PCIe I/O expander
0x72 ACK   -- TCAL9538 U35 main I/O expander (BOM default -- see the
              0x20 note above)
```

## On V2N's on-board sensor bus

`BOARD_I2C_SENSORS` is portable, but it is not the same physical bus on
every board.  On E1M-V2N101 + E1M-X-EVK it resolves to
`XEVK_I2C_BUS_SENSORS` -> `ALP_E1M_X_I2C0`
(`include/alp/boards/alp_e1m_x_evk_routes.h:67,128`) -- a different
controller from the SoM's own `brd_i2c` housekeeping bus that the PMICs,
clock generator, secure element, and GD32 supervisor sit on (see the
next section).  Porting to V2N takes three edits to `board.yaml`, not
one -- `som.sku` alone is not enough and fails closed rather than
silently doing the wrong thing (verified: each intermediate state below
is a real `alp_project.py --emit zephyr-conf` error, not a guess):

```yaml
som:
  sku: E1M-V2N101       # was E1M-AEN801
preset: e1m-x-evk        # was e1m-evk -- e1m-evk only hosts alif-ensemble/nxp-imx9 (ALP-B007)
pins:
  - { e1m: E1M_X_I2C0, macro: XEVK_I2C_BUS_SENSORS }   # was E1M_I2C0 / EVK_I2C_BUS_SENSORS -- e1m-x-evk's e1m_routes: has no E1M_I2C0 pad
cores:
  m33_sm:                # was m55_hp -- E1M-V2N101's topology: only exposes a55_cluster/m33_sm
    app: ./src
    peripherals: [i2c]
```

With all three changed, the same `src/main.c` scans `ALP_E1M_X_I2C0`.
Expected:

```
0x40 ACK   -- INA236A U21, +3V3 rail current monitor
              (include/alp/boards/alp_e1m_x_evk.h:80)
0x41 ACK   -- INA236A U31, +1V8 rail current monitor
              (include/alp/boards/alp_e1m_x_evk.h:81)
0x47 ACK   -- BMP581 U14 barometer
              (include/alp/boards/alp_e1m_x_evk.h:50)
0x48 ACK   -- INA236B U32, +VCAM2 rail current monitor
              (include/alp/boards/alp_e1m_x_evk.h:82-83)
0x49 ACK   -- INA236B U34, +VCAM3 rail current monitor
              (include/alp/boards/alp_e1m_x_evk.h:84-85)
0x4A ACK   -- INA236B U30, +5V rail current monitor
              (include/alp/boards/alp_e1m_x_evk.h:86)
0x4D ACK   -- TAS2563 U27 smart amp, left channel
              (metadata/boards/e1m-x-evk.yaml:261)
0x4E ACK   -- TAS2563 U28 smart amp, right channel
              (metadata/boards/e1m-x-evk.yaml:262)
0x50 ACK   -- 24C128 EEPROM, the SoM's `e1m_i2c0:` block
              (metadata/e1m_modules/E1M-V2N101.yaml:56-59)
0x68 ACK   -- BMI323 U13 IMU (alternate)
              (include/alp/boards/alp_e1m_x_evk.h:48)
0x69 ACK   -- ICM-42670 U12 IMU (canonical primary)
              (include/alp/boards/alp_e1m_x_evk.h:49)
0x72 ACK   -- TCAL9538 I/O expander (one of U35/U37 -- the header
              doesn't say which; the OTHER one has no committed
              address, see below)
              (include/alp/boards/alp_e1m_x_evk.h:51)
```

Ten of the twelve addresses above come from the `XEVK_I2C_ADDR_*`
macros (`include/alp/boards/alp_e1m_x_evk.h:48-52,80-86`), BENCH-
CONFIRMED on E1M-X-V2N silicon (`include/alp/boards/alp_e1m_x_evk.h:25-27`);
the TAS2563 pair (0x4D/0x4E) instead comes from the board's `audio:`
metadata (`metadata/boards/e1m-x-evk.yaml:261-262`).  The X-EVK's
second TCAL9538 (`metadata/boards/e1m-x-evk.yaml:54`, designators
U35/U37) has no `XEVK_I2C_ADDR_*` macro -- its address is the one
genuinely uncommitted value here; treat it, and only it, as TBD.
Separately, current silicon also ACKs 0x42 and 0x43 as INA236 (mfg-ID
"TI") even though the schematic BOM lists only the five monitors above
(`include/alp/boards/alp_e1m_x_evk.h:70-74`) -- a documented board
anomaly pending the next respin, not a sixth/seventh rail monitor.

## On V2N's BRD_I2C (a different bus, different code)

BRD_I2C is the SoM's RIIC8 housekeeping bus -- DA9292, ACT88760, OPTIGA,
TMP112, the clock generator, and the GD32 supervisor all sit here
(`metadata/e1m_modules/E1M-V2N101.yaml:41-55`), but `BOARD_I2C_SENSORS`
does not reach it (see above).  On the V2N M33 Zephyr target it's
numeric bus 0, opened directly rather than through a `<alp/board.h>`
alias (`examples/v2n/v2n-brd-i2c-bringup/src/main.c:350-353`) -- this
tutorial's code does not scan it.  Use
[`examples/v2n/v2n-brd-i2c-bringup`](../../examples/v2n/v2n-brd-i2c-bringup/)
instead: it opens bus 0 directly and probes each device with its real
chip driver.

If a documented address doesn't show up, the chip is missing or
mis-strapped -- compare against `metadata/e1m_modules/<SKU>.yaml`'s
`i2c_devices:` block (V2N/V2M/AEN all carry one; each entry carries its
own `address_7bit`, split into `brd_i2c:` and `e1m_i2c0:` sub-buses);
or, for a carrier-populated part, the board's own
`metadata/boards/<board>.yaml` (`i2c_devices:` on E1M-EVK; on
E1M-X-EVK, `audio:` for the TAS2563 pair, or
`include/alp/boards/alp_e1m_x_evk.h`'s `XEVK_I2C_ADDR_*` block for the
rest).

## See also

* [`examples/peripheral-io/i2c-scanner/`](../../examples/peripheral-io/i2c-scanner/)
* [Tutorial 04 -- intra-family portability](04-cross-family-portability.md)
* [Tutorial 08 -- runtime board detection](08-runtime-board-detection.md)
