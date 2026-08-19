# GD32 bridge — firmware tree overview

The **GD32 bridge** is the firmware that runs on the GigaDevice
GD32G553MEY7TR companion MCU on every E1M-X V2N / V2N-M1 SoM.  It
gives the Renesas RZ/V2N host a uniform on-module supervisor surface
(GPIO fan-out, eight PWM channels, the ADC bank that doesn't fit on
the Renesas pinmux, the DA9292 INT/TW fault-pin forward, the
secure-element reset, …) over either of two parallel buses.

The **wire protocol** is specified in
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md); this doc
covers the **firmware-tree** side -- where the source lives, how to
build it, how to flash it, and what state the implementation is in.

> **Pre-flashed by Alp; rebuild is optional and fully open.** The
> GD32G553 ships flashed by Alp with the bridge firmware, so for normal
> use the customer does nothing — the Renesas host talks to a working
> supervisor out of the box.  Like the CC3501E bridge, the GD32 firmware is
> **open**: the source lives in this repo (`firmware/gd32-bridge/`) and
> the GigaDevice library
> is a public submodule, so rebuilding or customizing needs no gated
> download — see **Build** below.

## At a glance

| Aspect              | Today (2026-06-04)                                                                |
|---------------------|-----------------------------------------------------------------------------------|
| Firmware tree       | [`firmware/gd32-bridge/`](../firmware/gd32-bridge/)                                                 |
| Toolchain           | Arm GNU Toolchain (`arm-none-eabi-gcc`), Cortex-M33 + thumb                       |
| Build system        | CMake (separate from the Zephyr-side `west build`)                                |
| HAL                 | Stub default; `BRIDGE_HAL_BACKEND=gd32` consumes the GigaDevice firmware library via the [`alplabai/gd32g5x3-firmware-library`](https://github.com/alplabai/gd32g5x3-firmware-library) submodule at `vendors/gd32_firmware_library/upstream/` (run `git submodule update --init` once after cloning) |
| Protocol coverage   | `PING`, `GET_VERSION`, `GET_BUILD_ID` working end-to-end without HW dependency    |
| Transport coverage  | SPI1 slave (25 MHz full-DMA, silicon-validated) + I2C0 slave in `hal/transport_hw_gd32.c` (gd32 backend) |
| Datasheet           | GD32G553 datasheet + user manual (held in the vendor datasheet) |
| Flash size on chip  | 512 KB (per datasheet)                                                            |
| RAM size on chip    | 128 KB                                                                            |

## Versioning

Three independent version axes — track them separately:

| Axis | Where | Bumps when |
|------|-------|-----------|
| **Firmware release** | `firmware-version.txt` (semver, baked in via CMake) | each firmware release — names the tag + prebuilt blob; the device surfaces it through `GET_BUILD_ID` as `<ver>+<sha>` |
| **Wire protocol** | `PROTOCOL_VERSION_*` in `src/protocol.h` (host: `<alp/chips/gd32g553.h>`) | the wire format changes; `GET_VERSION` returns it and the host refuses a mismatched MAJOR |
| **Build-id** | git short-SHA in the `GET_BUILD_ID` reply | every build — pins the exact source behind a release |

A firmware release can ship without a protocol bump, and vice-versa.

## Build

```bash
cd firmware/gd32-bridge
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
cmake --build build
```

Output: `build/gd32-bridge.elf`, `.hex`, `.bin`.

* `BRIDGE_HAL_BACKEND=stub` (default) -- builds without the
  GigaDevice firmware library; every HW-touching opcode reports
  `BRIDGE_HW_ERR_NOTIMPL` which the protocol layer maps to wire
  `STATUS_IO`.  Useful for smoke-testing the protocol round-trip in
  a hardware-less unit-test environment.
* `BRIDGE_HAL_BACKEND=gd32` -- builds against
  [`vendors/gd32_firmware_library/`](../vendors/gd32_firmware_library/).
  The wrapper consumes the GigaDevice **GD32G5x3 Firmware Library**
  via a git submodule pointing at
  [alplabai/gd32g5x3-firmware-library](https://github.com/alplabai/gd32g5x3-firmware-library)
  (a verbatim mirror of v1.5.0 under SLA-GD0001 v1.1).  Run
  `git submodule update --init --recursive vendors/gd32_firmware_library/upstream`
  once after cloning, then the bridge build picks it up
  automatically.  See
  [`vendors/gd32_firmware_library/README.md`](../vendors/gd32_firmware_library/README.md)
  for the licence-redistribution constraints + the version-bump procedure.

## Source layout

```
firmware/gd32-bridge/
├── CMakeLists.txt                   (default monolithic build; -DBRIDGE_OTA_PARTITIONED=ON
│                                     emits bootloader + slot-A/B apps instead)
├── README.md
├── toolchain/
│   ├── arm-none-eabi.cmake          (Arm GNU Toolchain)
│   ├── gd32g553_flash.ld            (monolithic full-flash linker)
│   ├── gd32g553_bootloader.ld       (32 KB Path-A bootloader)
│   └── gd32g553_app_slot.ld.in      (slot-relocated app template; .ramfunc in RAM)
├── hal/
│   ├── bridge_hw.h                  (HAL surface consumed by protocol.c)
│   ├── bridge_hw_stub.c             (host-test backend, ops return NOTIMPL)
│   ├── gd32/                        (real GigaDevice peripheral HAL, one TU
│   │                                 per peripheral: init.c gpio.c trng.c
│   │                                 tmu.c vref.c adc.c adc_stream.c dac.c
│   │                                 qenc.c pwm.c pwm_capture.c counter.c
│   │                                 timer_sync.c power.c se_reset.c
│   │                                 + gd32_common.h)
│   ├── transport_hw_gd32.c          (SPI1 + I2C0 slave silicon bring-up, full-DMA SPI)
│   └── fmc_ota.c                    (RAM-resident dual-bank FMC erase/program)
├── src/
│   ├── main.c                       (entry point, WFI loop)
│   ├── protocol.c                   (shared command-handler table)
│   ├── protocol.h
│   ├── transport_spi.c              (SPI-slave receive + reply staging)
│   ├── transport_i2c.c              (I2C-slave receive + reply staging)
│   ├── ota.c / ota.h / ota_layout.h (OTA Path-A state machine + A/B metadata)
│   ├── crc32.c / crc32.h            (IEEE CRC-32 shared by app + bootloader)
│   ├── boot/boot_main.c             (bootloader: slot pick + CRC validate + jump)
│   └── bootloader/                  (0xF0..0xFF dispatch into ota.c)
├── tools/
│   └── gen_ota_metadata.py          (factory A/B metadata record for first-flash)
└── tests/
    ├── gen_protocol_vectors.py      (CRC + wire-vector generator)
    └── protocol_vectors.txt         (CRC + wire vectors shared with host)
```

The single design rule that makes this tree work:
**both transports call the same `protocol_dispatch()`** in
`src/protocol.c`.  Adding an opcode is therefore a one-place
change.

## Flashing

| Method                                | Status today      | Notes                                                                                                                                  |
|---------------------------------------|-------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| External SWD probe (J-Link, ST-Link)  | **Supported.**    | SWDIO + SWCLK accessible on the V2N module's programming header.                                                                       |
| In-system upgrade over SPI / I2C      | **Implemented, gated — silicon-validated 2026-06-04.** | Application-bootloader path; the `0xF0..0xFF` opcodes route through `src/bootloader/` into the OTA state machine in [`src/ota.c`](../firmware/gd32-bridge/src/ota.c) (FMC backend `hal/fmc_ota.c`). Destructive flashing is armed only with `-DBRIDGE_OTA_PARTITIONED`; default builds reply `STATUS_NOSUPPORT` (can't brick the running image). The armed build emits the partitioned set (32 KB bootloader + slot-A/B apps); first-flash also needs the factory metadata record from [`tools/gen_ota_metadata.py`](../firmware/gd32-bridge/tools/gen_ota_metadata.py) at `0x08008000`. Validated end-to-end on the bench: stream → verify → commit → boot new slot → rollback (protocol v0.6). See [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10 Path A. |
| Host-driven SWD bit-bang from V2N     | **Scaffolded.**   | Renesas-side software SWD controller drives `GD32_SWDIO` + `GD32_SWCLK` (routed back to V2N pads per the 2026-05-12 HW decision); universal recovery + factory first-flash.  Driver lives at [`chips/gd32_swd/`](../chips/gd32_swd/) (`driver_status: partial` until exercised on real silicon).  See [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10 Path B. |

### Who flashes it, and when

Alp Lab flashes the GD32 in production; a customer's normal field update
arrives over the in-system OTA path above.  Direct SWD flashing exists
for one customer case only: **recovering a bricked bridge**, with Alp
Lab-supplied binaries.

The four V2N / V2M SoM presets state exactly that, but as **two** keys,
not three -- `flash_method` and `flash_args` are absent from all four
entries, on purpose:

```yaml
helper_firmware:
  - name:          gd32_bridge
    chip:          gd32g553
    flash_policy:   recovery_only        # who may flash it locally, and when
    update_channel: alp_ota_spi_bridge   # how it is updated in the field
```

`flash_policy` and `update_channel` stay independent keys exactly as
before -- `flash_policy` answers who may reach a local flash path *if
one is ever added*, and the schema requires it on every helper entry
whether or not a `flash_method` exists.  It is `flash_policy:
recovery_only`, not `update_channel`, that decides this entry's fate:
`tan flash` declines it before it ever reaches the `flash_method`
check (`helper_flash_gate` in `python/tan/core/flash_plan.py`, called
ahead of `_flash_entry`'s own `flash_method` check in
`python/tan/commands/flash_cmd.py` -- tan-cli#611) with:

```text
flash: helper 'gd32_bridge' is programmed by Alp Lab in production
and is customer-flashable only to recover a bricked device, with Alp
Lab-supplied binaries; skipping. Field updates arrive over
update_channel: alp_ota_spi_bridge. To recover a bricked device
deliberately, re-run with `--helper gd32_bridge --recover`.
```

status `skipped`, rc `-1` -- never a run failure.  `tan` DOES expose
`--helper gd32_bridge --recover` and names it in its own skip message
above, but because the entry declares no `flash_method` that re-run
also skips without writing (same `_flash_entry` path, past the policy
gate) -- so bricked-bridge recovery is still an out-of-`tan` SWD
procedure today.  As measured against tan-cli tag `v0.6.0-rc1`, tan
still ships the `swd_probe` backend
(`plan_swd_probe` in `python/tan/core/flash_plan.py`); both `--recover`
(`python/tan/commands/flash_cmd.py`, the `recover` CLI option) and
`ALP_FLASH_REQUIRE_DPIDR` (`python/tan/commands/flash_cmd.py`'s
`REQUIRE_DPIDR_ENV`, documented at tan-cli `docs/setools.md`'s
"`ALP_FLASH_REQUIRE_DPIDR=1`" section) exist in tan-cli today; they
are inapplicable to this entry only because the preset declares no
`flash_method`, not because either is dead code.  Only the metadata
stopped naming `swd_probe`, and moving the GD32/CC3501E SWD
programming path out of alp-sdk entirely is tracked separately in
#1370.

**Recovering a bricked bridge today is an out-of-`tan` SWD procedure**:
attach a J-Link (or compatible SWD probe) directly to the GD32's
programming header and flash an Alp Lab-supplied binary with
`JLinkExe` or an equivalent flasher, using the same `gd32g553` target /
`GD32G553MEY7TR` device string / `0x08000000` flash base that the
`flash_args` block removed by #1439 used to carry (partitioned images
also need the factory A/B metadata record at `0x08008000` -- see
[`firmware/gd32-bridge/README.md`](../firmware/gd32-bridge/README.md)).
Note the SW-DP ID guard is **unarmed** on that
procedure: `metadata/chips/gd32_swd.yaml` currently arms its
wrong-board guard with `0x6BA02477`, but that value is not a GD32
reading -- it is the bench-measured SW-DP ID of the V2N CM33 DAP, a
third J-Link on this rack (`scripts/bench/aen/bench-env.sh:145-147`,
measured 2026-08-08, `Found Cortex-M33 r0p4`; the `e1mx-v2n-m1-01`
probe table, `CHANGELOG.md:3364` and `CHANGELOG.md:3367`), tracked as
#1440.  An
unattributed `0x0BE12477` elsewhere in this repo is the only GD32
candidate on record, and it too **has not been measured on a GD32
with a probe attached**.  See #1369 for the open issue tracking that
measurement, and #1440 for the `0x6BA02477` mislabeling above.

**This disagrees with `docs/tutorials/07-recovering-a-bricked-bridge.md`'s
"IDCODE caveat" section**, which tables `0x0BE12477` as a bench-measured
fact ("A healthy, correctly-wired GD32 answers `0x0BE12477`").  Do not
silently pick a winner between the two documents: whether `0x0BE12477`
was ever read off a GD32 with a probe attached is #1440's and #1369's
open question, and it needs silicon to close, not doc surgery.  Until it
does, **this section governs the J-Link/external-probe recovery-flash
decision** (the alternative to this tutorial's on-SoM bit-bang route,
`chips/gd32_swd/` -- SWDIO/SWCLK/NRST on P70/P71/P74, no J-Link, no
cloned serial) -- it is the one an operator follows immediately before a
write that can reach the wrong board, which is exactly the moment
treating an unattested value as a pass condition would matter.  The
tutorial's table is the weaker claim: it reproduces
`scripts/bench/aen/bench-env.sh`'s `GD32_DPIDR` export, and that export
formerly carried its own "BENCH-VERIFIED" banner covering `GD32_DPIDR`
too; that banner cited `docs/aen-bench-bringup.md`, which does not
mention the GD32 at all, and is now hedged
(`scripts/bench/aen/bench-env.sh:148-151`) -- so the tutorial's "fact"
traces back to a since-hedged, uncited assertion, not an independent
measurement.

**Required step on the alplab-gw bench: read the DPIDR by hand before
flashing, and abort on a match to either of two known-wrong boards.**
The GD32 probe (USB path `3-4.2`) and the AEN E8 probe (`3-4.4.3`)
enumerate the same J-Link serial `603000869`, and `JLinkExe` selects
an adapter only by serial -- with no port selector and no armed
DPIDR guard on this out-of-`tan` path, probe choice for the cloned
pair is ambiguous by construction
(`scripts/bench/aen/bench-env.sh:138-143`).  Setting the shell
variable `JLINK_SN` has **no effect on a hand-run `JLinkExe`
invocation**: `JLinkExe` takes a probe selector only from a
`-SelectEmuBySN <sn>` command-line flag or a `SelectEmuBySN` line
inside its CommanderScript, never from the environment.  (The env var
works in `scripts/bench/aen/flash-jlink-mramxip.sh` only because that
script itself converts it -- `SEL="${JLINK_SN:+SelectEmuBySN
$JLINK_SN}"`, line 67 -- and splices `$SEL` in as the first
CommanderScript line, line 121; there is no equivalent conversion on
this hand-run recovery path, so `export JLINK_SN=603000869` alone
does nothing here.)

Follow this order, exactly -- it is step 1's physical detach of the
AEN E8, not the read-write pairing itself, that removes the
cloned-serial ambiguity for every invocation that follows:

1. **Detach the AEN E8 probe from the bench first**, physically, for
   the duration of the recovery flash.  A read taken while the AEN E8
   is still attached proves nothing about which of the two
   `603000869` probes will answer the *next* invocation -- probe
   choice is arbitrary per `JLinkExe` run, which is this section's own
   premise. Detaching the AEN E8 is what removes the cloned-serial
   ambiguity for every invocation from here on.

   Identify the two probes at the connector before pulling either
   one: run `lsusb -t` and match USB path `3-4.4.3` to the AEN E8
   against `3-4.2` for the GD32 (both enumerate under the shared
   J-Link serial `603000869`; those USB paths are recorded at
   `CHANGELOG.md:2736` and `CHANGELOG.md:3339`, not in
   `bench-env.sh`).  After detaching, re-run `lsusb -t` and confirm
   exactly one `603000869` probe still enumerates before moving on to
   step 2.

   **Re-plugging the AEN E8 at any point before the write in step 4
   completes voids this procedure.**  Do not resume by re-reading --
   physically detach it again and restart from this step.
2. Select the probe explicitly, by serial, with the identical
   CommanderScript line on every remaining invocation. Run this exact
   read-only preflight (matches `flash-jlink-mramxip.sh`'s "0b. SAFETY
   GATE" shape, but with the GD32's own selector and a generic
   Cortex-M33 device -- not the AEN part profile):

   ```text
   SelectEmuBySN 603000869
   si SWD
   speed 4000
   device Cortex-M33
   connect
   exit
   ```

   `SelectEmuBySN 603000869` also excludes the V2N CM33 DAP
   (`600107451`, a different serial) by construction; it is the
   AEN-vs-GD32 ambiguity within the shared `603000869` serial that
   step 1's physical detach resolves, not this selector.
3. Read the transcript by eye and **require a `Found SW-DP with ID
   0x...` line to be present**.  This preflight is fail-closed, not
   fail-open: **abort if that line is absent**, for any reason --
   a connect failure, the gated-DAP `Could not find core in CoreSight
   setup` state, a rejected command line, or any other transcript
   with no SW-DP ID line at all is a STOP, not a silent permit to
   proceed, mirroring `bench_jlink_assert_aen_dpidr`'s own
   abort-unless-seen shape (`scripts/bench/aen/bench-env.sh:181-185`)
   rather than aborting only on a positive match to a known-wrong ID.
   When the line is present, abort before any write if the ID matches
   `AEN_DPIDR` (`4C013477` -- bench-verified AEN E8) **or**
   `V2N_CM33_DPIDR` (`6BA02477` -- the V2N CM33 DAP,
   `scripts/bench/aen/bench-env.sh:154`; see #1440) -- both are an
   unconditional STOP, not merely "not the AEN E8".  Do not treat
   `GD32_DPIDR` (`0BE12477` -- a claimed-but-unattested GD32 value;
   see #1369) as a pass condition, and **do not proceed on any ID the
   operator cannot positively attribute to the GD32**: an ID that
   matches neither abort value is necessary but not sufficient to
   proceed, since it has not itself been proven to be the GD32, and an
   unrecognized ID is itself grounds to abort and investigate, not to
   guess.  `bench-env.sh` formerly carried a "BENCH-VERIFIED" label on
   `GD32_DPIDR` citing `docs/aen-bench-bringup.md`, which does not
   mention the GD32 at all; that label is now hedged
   (`scripts/bench/aen/bench-env.sh:148-151`), so rely on the manual
   read, not the `GD32_DPIDR` value, to prove the probe is not on a
   known-wrong board.
4. **Flash immediately** after a passing read, using the same
   `SelectEmuBySN 603000869` line as the first line of the flash
   CommanderScript -- the identical selector, not merely the same
   serial typed again.  Nothing else -- no re-plug, no other
   `JLinkExe` invocation -- may land between the read in step 3 and
   this write; a transcript from an earlier invocation does not
   constrain a later one, since probe selection is per-invocation.
5. **Re-read and re-compare (repeat steps 1-3) after any re-plug of
   either probe, or after any intervening `JLinkExe` invocation**
   between the last passing read and the write.  A stale "it read
   clean earlier" does not carry across a re-plug or another
   invocation.  Repeating step 1 means physically detaching the AEN
   E8 again and confirming with `lsusb -t` that exactly one
   `603000869` probe enumerates before re-reading -- re-selecting the
   probe (step 2) or re-reading (step 3) alone does not clear a
   re-plug of the AEN E8, since only the fresh detach removes the
   ambiguity those later steps depend on.

Do **not** gate this on `scripts/bench/aen/bench-env.sh`'s
`bench_jlink_assert_aen_dpidr` -- that helper is written to protect
AEN-targeted writes: it *passes* (returns 0) when the transcript shows
the AEN E8's DPIDR and *aborts* (returns 4) when it shows the GD32's,
which is exactly inverted for this recovery flow.  No helper for the
GD32-recovery direction (the inverse of `bench_jlink_assert_aen_dpidr`)
exists yet.  There is currently no armed wrong-board guard on this
recovery path; treat the AEN-detach, `SelectEmuBySN` selection, and
DPIDR-read steps above as manual and mandatory until #1369 lands a
measured GD32 DPIDR to arm a real guard against.

`update_channel: alp_ota_spi_bridge` is deliberately a different value
from the CC3501E's `alp_ota_spi_otp`: this channel streams into the
slot-A/B application bootloader with commit + rollback, it does not
program an OTP the GD32 does not have.

## Cross-link

* Wire spec: [`gd32-bridge-protocol.md`](gd32-bridge-protocol.md).
* Host-side driver header: [`<alp/chips/gd32g553.h>`](../include/alp/chips/gd32g553.h).
* Host-side driver source: [`chips/gd32g553/gd32g553.c`](../chips/gd32g553/gd32g553.c).
* GD32 pad allocation map: [`metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`](../metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv).
