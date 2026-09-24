### Added — U-Boot sequences the V2N-M1 DEEPX 0.75 V core rail before releasing `M1_RESET` (#2045)

The V2N-M1 SoM's DEEPX DX-M1 core rail (DA9292 CH2, 0.75 V) had no sequencing
step anywhere: `board_late_init()` released `M1_RESET` and enabled the PCIe
mux purely on the on-module EEPROM manifest match, with nothing bringing CH2
up first. The DA9292-AROVx OTP variant this SoM uses boots with
`CH2_VSTEP=1` (doubled voltage range, `PMC_CTRL_01` reset `0x80`); releasing
`M1_RESET` before CH2 is confirmed at 0.75 V and power-good risked an
over-voltage event on the DEEPX core rail the instant the chip started
drawing current, not merely a brownout.

`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`
adds `alp_deepx_rail_bringup()`, run in `board_late_init()` before the
existing `alp_deepx_pcie_bringup()` (patch 0001). It programs CH2 to 0.75 V
over DM I2C bus 8 (RIIC8/BRD_I2C, DA9292 at 0x1E), verifies every write by
read-back, is idempotent across a warm reboot (skips re-programming a
channel already correctly set, so it never clears a validly-up `CH2_EN`),
then only enables the channel and releases P64 (`DEEPX_CORE_0P75_EN`) after
polling P65 (`DEEPX_PWR_EN_REQ`) and confirming `CH2_PG` with no UV/OV/OC.
`alp_deepx_pcie_bringup()` now runs only when the rail step succeeds —
`M1_RESET` is never released with the rail down. Gated on the new
`CONFIG_ALP_E1M_DEEPX_RAIL` (default `n`, wired via `deepx-rail.cfg` for the
V2M MACHINEs only) OR the existing EEPROM-manifest check, so either path
alone is sufficient and V2N base images touch nothing on the DA9292.

**Maintainer decision carried through in full:** Cortex-A55/Linux is the
sole master of RIIC8/BRD_I2C; the CM33 must never master it or claim
P64/P65. This retires the CM33-side attempt at the same job:

- `src/zephyr/v2n_power_mgmt.c` / `.h`, `CONFIG_ALP_SDK_V2N_POWER_MGMT`, and
  the V2N supervisor's BRD_I2C transport
  (`CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID`/`_I2C_ADDR`/`_I2C_BITRATE_HZ`,
  `alp_z_v2n_supervisor_brd_i2c_acquire`/`_release`) are deleted outright —
  the CM33's only GD32 transport is SPI now.
- `chips/da9292`'s driver stays, but `da9292_v2n_m1_enable_deepx_rail()` /
  `da9292_v2n_base_init()` are documented diagnostic/read-only-only; nothing
  in alp-sdk may call them as part of a boot flow.
- The generated CM33 Zephyr board files (`e1m_v2n101_m33_sm`,
  `e1m_v2m101_m33_sm`) no longer claim RIIC8: `&i2c8` is `status =
  "disabled"` and the `alp-i2c0` alias is gone
  (`metadata/e1m_modules/v2n/supervisor-links.yaml`'s `brd_i2c` link flips
  to `status: disabled`).
- `metadata/pinmux/v2n.yaml` attributes `DEEPX_CORE_0P75_EN` (P64) and
  `DEEPX_PWR_EN_REQ` (P65) to `core: "a55"` so a CM33 board file can't claim
  them.
- `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2m-deepx.dtsi` drops
  its `deepx-m1-reset-release-hog` (U-Boot is the sole `M1_RESET` owner);
  the P80/P95 PCIe-mux hogs are unchanged.
- `examples/v2n/v2n-rtc-multi-alarm` is retired — it addressed the RTC over
  CM33 I2C, now forbidden (the on-module RV-3028-C7 is Linux's `/dev/rtc0`,
  per the RIIC8-ownership fix that also lands on this branch).
- The kernel patch
  `0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-on.patch` (retitled, was
  `...-RSCI7-RIIC8-on.patch`) drops its `riic_8_ckm` `DEF_MOD_CRITICAL`
  hunk — Linux is RIIC8's real consumer now, so `clk_disable_unused`
  correctly leaves it alone on its own. The five `rsci_7_*` hunks (SCI7
  SPI, still CM33-owned) are unchanged.

The U-Boot patch shipped here (`0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
is the `git format-patch`-regenerated version, not this branch's first
draft — three real defects a synthetic dry-run apply could not catch were
found and fixed before landing:

- the P65 (`DEEPX_PWR_EN_REQ`) input read was reading the P output-latch
  register (last value *written*), not the live pin level. Reads it from
  the dedicated PFC terminal-input register (`PFC_BASE+0x0820+n`) instead
  -- bench-confirmed against a real input bit.
- every program-phase abort now forces P64 low / `M1_RESET` asserted via
  a shared `alp_deepx_rail_safe_off()` helper, not only the enable-phase
  failures that already did this inline.
- **Hardening:** `CH2_VSTEP` is re-checked immediately before the
  `CH2_EN` write (not just earlier in the program phase) and aborts if
  it is ever found set -- the exact over-voltage condition this whole
  sequence exists to prevent.
- The PG-timeout failure message now also names a possible EN2-gates-CH2
  hardware wiring question (`CH2 may require EN2/P64 high before PG --
  see bring-up doc`) -- see `docs/bring-up-v2n-m1.md` §2 for what to
  check if it appears on real silicon.

**Two unrelated BRD_I2C address corrections, bench-confirmed on the same
pass (E1M-V2M103, `alp i2c scan`/`i2cdetect`):** the public macro
`CLK_5L35023B_I2C_ADDR_DEFAULT` (`include/alp/chips/clk_5l35023b.h`)
changed `0x68u` -> `0x69u` — the fitted part straps `I2C_addr[1:0] = 01`,
not the previously assumed `00`; and `metadata/chips/tmp112.yaml`'s
address entry changed `0x48` -> `0x40` — the fitted TMP112DIDPWR
(X2SON-5) straps `ADD0 = GND` to `0x40`, the same class of bug already
fixed for AEN (#1978). Both are the actual strapped address, not a
board defect; every example and doc citing either address is updated in
the same slice.

**Compile proof:** `0004` applies cleanly (verified on `alplab-gw`, git
`apply --check` + `apply`) on top of the CORRECTED `0002` (md5
`c546f00cabca346e335febd21ecbc440`, PR #2280) and the meta-renesas
PMIC-I2C-removal patch, in that order, against `renesas-u-boot-cip`
`bcf29d98`. It does NOT apply against this branch's own uncorrected
`0002` -- that patch fails to apply outright on a clean `bcf29d98`
checkout (its "gigadevice" hunks are genuinely broken, not merely
conflicting with `0004`), confirming #2280 must land first. The md5
changed from the prior `59f81f3f9ea3812d99bd3c476ede2b86` only by a
present-tense rewrite of the commit-message prose (retired-module and
bug-history phrasing); the diff hunks are byte-identical, so this
compile proof still covers the patch content unmodified.

**`deepx_lpddr_0v85` strap resolved: `0x48` (#1163, #1845).**
Bench-measured 2026-09-24 on E1M-V2M103: `0x48` ACKs on `BRD_I2C`
only after `P64` (`DEEPX_CORE_0P75_EN`) is driven high, and its VOUT
register (`0x5A`) reads 0.85 V, matching the role; `0x44`
(`deepx_ddr5_vdd`) and `0x4F` (`deepx_ddr5_vddq`) were confirmed
present on the same scan. `metadata/e1m_modules/E1M-V2M101.yaml` /
`E1M-V2M102.yaml` / `E1M-V2M103.yaml` change `address_7bit: "TBD"` ->
`"0x48"` for that role; `docs/bring-up-v2n-m1.md` and
`docs/soms/v2n-m1.md` are updated to match and no longer warn against
hardcoding `0x48`.

**CH2 sequence independently bench-proven from Linux, 2026-09-24
(E1M-V2M103).** Before touching U-Boot, the exact register sequence
`0004` implements was run by hand from Linux userspace over RIIC8
(`i2cset`/`i2cget` on `/dev/i2c-8`) and produced the expected result
end to end: `CTRL_01` (`0x07`) `0x81` -> `0x01` (clear `CH2_VSTEP` +
`CH2_EN` together), `VOUT_CH2` (`0x0C`/`0x0D`) programmed to `0x96`
and read back, `EVENT_00` cleared, then `CTRL_01 = 0x03` (`CH2_EN`)
-> `STATUS_00 = 0x03` (`CH2_PG` set, no UV/OV/OC), DMM confirms
0.75 V on the rail; driving `P64` high then surfaces the DEEPX bucks
(`0x44`, `0x48`, `0x4F`) on `BRD_I2C`. This confirms the DA9292
register map, the 0.75 V programming values, and the physical wiring
in `0004`'s sequence are all correct -- the failure below is
U-Boot-side only.

**U-Boot-side RIIC driver bug found, and fixed on its own merits.** The
first on-silicon run of `0004` (chain-loaded) failed at the very
first DA9292 read: `ALP: DA9292 DEV_ID=0x00 (want 0xea) REV_ID=0x00
CFG_REV=0x00 ret=-110 -- abort`. While chasing that, a genuine
RIIC(rzg2l)/R9A09G056 U-Boot driver defect was found (the Linux-side
proof above already rules out a rail-sequence or wiring problem):
`drivers/i2c/rzg2l_riic.c`'s `riic_xfer()` ran every `struct i2c_msg`
of a multi-message transfer as its own independent START..STOP bus
transaction. `dm_i2c_read()` always builds a register read as two
messages -- a write of the register offset, then the paired read --
and `riic_read_common()` already performs the WHOLE combined
write-offset + repeated-START + read operation by itself, using
`priv->offset`. So the un-patched `riic_xfer()` loop ran msg[0]
through `riic_write_common()` first, sending the offset byte as its
own full START..STOP transaction, and then `riic_read_common()` sent
it AGAIN as part of its own transaction -- the offset went out twice,
as two separate bus transactions with a spurious STOP between them,
never the single repeated-START transaction `dm_i2c_read()` intends.
This is a real, independently-confirmed protocol defect (every
register read on any of the SoC's nine RIIC instances goes through
the same `riic_xfer()` path) and plausibly contributes to a desync
severe enough to time out a cold read and leave `ICCR2` `BBSY` stuck
set, wedging every later transaction with `-EBUSY`. It was **not
proven** to be the *sole* cause of the bench failures when first found
-- see the measured root cause below, found on the next bench pass
after `0005` alone did not clear the failure. Writes are unaffected
either way -- `dm_i2c_write()` always builds one
combined message, so `0004`'s own DA9292 writes already ran correctly;
only its reads (`PMC_DEV_ID`, `STATUS_01`, `CTRL_01`, `VOUT_CH2`,
`EVENT_00`/`01`, the `STATUS_00` poll) could hit this bug.

Patch `0005-i2c-rzg2l_riic-combined-register-read.patch` (regenerated
via `git format-patch` after a maintainer review, not hand-edited)
fixes `riic_xfer()` on its own merits, beyond the original narrow
fix: the fast path now recognises the write-offset-then-read message
pair for any 1-4 byte offset (packed big-endian into `priv->offset`,
matching what `riic_send_mem_addr()` already supports), not just one
byte -- this also fixes the on-module 24C128 EEPROM's 2-byte-offset
reads on RIIC0, previously left on the original double-transaction
loop. The fallback per-message loop's bare-read case (a lone read
message with no preceding offset write) now passes `alen=0` to
`riic_read_common()` instead of resending a stale `priv->offset` left
over from an unrelated earlier transaction. `riic_check_busy()` gains
a one-shot recovery on a BBSY timeout -- clock SDA free via
`ICCR1.CLO` if a slave is holding it low, then run the same
`ICE=0+IICRST` full reset/re-init `riic_probe()` performs on cold
init, and retry once (still returns `-EBUSY` if the bus stays wedged)
-- since `riic_ops` implements no `dm_i2c` bus-recovery op today
(`"i2c reset"` -> `"Not supported by the driver"`). And
`riic_wait_for_icsr2()`'s timeout diagnostic (which bit was awaited,
`ICSR2`, `ICCR2`) now prints unconditionally instead of only in DEBUG
builds, so a bench run shows which wait timed out without a debug
rebuild. This is a generic RIIC driver fix (every one of the SoC's
nine RIIC instances routes register reads through the same
`riic_xfer()`), not specific to RIIC8, so it stays added
unconditionally for `rzv2n-family` rather than gated the way `0004`'s
Kconfig knob is.

**Measured root cause found, 2026-09-24 (E1M-V2M103, U-Boot
`bcf29d98`).** `0005` alone did not clear the failure: the next bench
pass still saw `i2c md 0x1e 0x19 1` on RIIC8 fail, this time with
`ICSR2=0x0a` (`STOP|AL`, arbitration lost) on the *repeated START*, a
different failure mode from either symptom `0005` targeted. Two
fixes, each **individually bench-proven sufficient** from the U-Boot
prompt:

- **SoC-internal pull-ups on P06/P07.** Linux already programs `PUPD_H`
  (port 0) `= 0x03030000` (pull-up on P06/P07) while U-Boot leaves the
  register at its POR value of `0` -- no pull-up at all. Writing
  `0x03030000` from the U-Boot prompt made the read return reliably.
- **A slower SCL.** Setting `ICMR1` `CKS=5` (write `0x50`) also made
  reads work. Root cause: `riic_set_clock()`
  (`drivers/i2c/rzg2l_riic.c`, one CKS/`ICBRH`/`ICBRL` table shared by
  every SoC this driver builds for) is tuned for a 50 MHz RIIC input
  clock, but R9A09G056/057 (RZ/V2N, RZ/V2H) run RIIC off a 100 MHz
  input instead (Linux uses `ICBRL`/`ICBRH` `0x0d`/`0x07` at `CKS=3`
  for its own 400 kHz bus on the same clock) -- so U-Boot's "100 kHz"
  config (`CKS=3`, unmodified) actually runs the bus at roughly double
  rate, bench-measured `~220 kHz`.

Also measured on the same pass, unrelated to either fix: PFC port0
(`@0x10410480`) `= 0x11000000` (P06/P07 func1) identically in both
U-Boot and Linux, and RIIC0 (the on-module EEPROM bus) works -- the
EEPROM there is simply blank, not a bus fault.

`0006-rzv2n-dev-i2c-rzg2l_riic-p06-p07-pullup-clock-fix.patch` carries
both fixes plus two smaller, unconditional hardening additions in the
same file targeting the identical AL failure signature: `riic_read_common()`
now polls `ICCR1` `SDAI=1` (SDA released, bounded `~100 us`) after the
offset-write `TEND` wait and before the repeated START -- issuing the
restart while a slave still holds SDA low from its ACK is exactly the
race that lost arbitration on the bench; and `riic_check_busy()` now
clears stale `ICSR2` `AL|STOP|NACKF|START` (sticky bits a prior
transaction's abort can leave set, misread as an immediate event on
the *next* transaction) before the busy-bus check, running the same
`IICRST` recovery `0005` added for the `BBSY`-stuck case when `AL`
specifically is found set, rather than leaving it merely cleared.
Neither of the two hardening additions was separately bench-validated
in isolation -- they are defensive, not new proven fixes. The clock
fix is gated to R9A09G056/057 builds only (`#if defined(CONFIG_R9A09G057)
|| defined(CONFIG_R9A09G056)`); the other SoCs sharing this driver
(R9A09G047, R9A08G045S) are untouched, their actual RIIC input clock
not established here.

The contradicting DT comment in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`
(`&pinctrl`, `i2c0_pins`/`i2c8_pins` block) is corrected to match: it
previously asserted flatly that the module has no external pull-ups
on RIIC0/RIIC1/RIIC8; that population is actually **TBD** (not
confirmed against the schematic/BOM), not a known-absent fact. The
comment now says so, and states that the SoC-internal pull-up on
RIIC8/BRD_I2C specifically is bench-confirmed **required** regardless
of what that population turns out to be -- without it, RIIC8 fails
with arbitration-lost in U-Boot.

**Rebuilt and link-verified with the reworked fix, 2026-09-24** (fresh
`bcf29d98` + PMIC-I2C-removal + `0001` + `0002` (md5
`c546f00cabca346e335febd21ecbc440`) + `0003` (4 GB tier) + `0004`
(md5 `269fc80793f33514871704d3c1f4fc73`) + reworked `0005`, `0001`
through `0004` applied to the working tree, `0005` verified via `git
apply --check` then `git apply` against a separate, fresh `bcf29d98`
worktree and confirmed byte-identical to the working tree's copy;
`rzv2n-dev_defconfig` + `no-dirty-version.cfg` + `gigadevice-xspi.cfg`
+ `deepx-rail.cfg` + `fdtfile-v2m.cfg`; WSL Ubuntu-22.04,
`aarch64-linux-gnu` gcc 11.4.0): build `rc=0`, `strings | grep '^ALP:'`
still finds all 25 DA9292/DEEPX/riic-recovery strings linked (LTO did
not drop any of them), `CONFIG_SYS_SDRAM_SIZE` = `(0x100000000u -
DRAM_RSV_SIZE)` and the control DT's `memory@48000000` = `<0x0
0x48000000 0x0 0xF8000000>` -- both correct for the 4 GB tier.
`u-boot.bin`: md5 `9c05679f0f9bc108164f68bef4435b95`, size 764240
bytes (`0xBA950`), crc32 `0xde3a8bdd`. BENCH-PENDING: unverified on
silicon.

**v4 rebuilt with `0006` added, 2026-09-24** (fresh `bcf29d98`
worktree, independent of the working tree above: PMIC-I2C-removal +
`0001` + `0002` + `0003` (4 GB tier) + `0004` + `0005` + `0006`, each
applied via `git apply --check` then `git apply`, in that order, all
seven succeeding cleanly with zero fuzz; `rzv2n-dev_defconfig` +
`no-dirty-version.cfg` + `gigadevice-xspi.cfg` + `deepx-rail.cfg` +
`fdtfile-v2m.cfg`; WSL Ubuntu-22.04, `aarch64-linux-gnu` gcc 11.4.0):
build `rc=0`, both changed files (`board/renesas/rzv2n-dev/rzv2n-dev.c`,
`drivers/i2c/rzg2l_riic.c`) compiled with no warnings. `strings |
grep -c '^ALP:'` = `26` (was `25` in the v3 build above; `0006` adds
exactly one new `printf` -- the stale-AL recovery message -- LTO did
not drop it; the PUPD write and the SDAI poll have no strings of their
own). `CONFIG_SYS_SDRAM_SIZE` = `(0x100000000u - DRAM_RSV_SIZE)` and
the control DT's `memory@48000000` = `<0x0 0x48000000 0x0
0xF8000000>` -- unchanged from v3, still correct for the 4 GB tier.
`u-boot.bin`: md5 `c2b3bbb114a2c509678ccfeb21acb57f`, size 764328
bytes (`0xBA9A8`), crc32 `0xac1df277`. `0006` patch file itself: md5
`8a739735b7cfcdfa4a16febe0aa5dab4`.

**v4 flashed and booted end to end, 2026-09-24 (E1M-V2M103) -- DEEPX
rail run PASSED on silicon.** Not just the interactive register pokes
above: this compiled `u-boot.bin` ran `board_late_init()` for real and
brought CH2 up on its own. Console: `ALP: DA9292 programmed
CTRL_01=0x01 VOUT_CH2=0x96/0x96` then `ALP: DEEPX rail 0.75V up
(PG)`. Read back after boot: `CTRL_01` `0x03`, `VOUT_CH2` `0x96/0x96`,
`STATUS_00` `0x03` (`CH2_PG` set, no UV/OV/OC), `STATUS_01` `0x00`;
the DEEPX bucks surfaced on `BRD_I2C` once `P64` went high, all
present and matching their known roles: `0x44` `0x82`, `0x48` `0x5a`,
`0x4F` `0x14`. The PCIe step (`alp_deepx_pcie_bringup()`, patch
`0001`) did not run on this pass -- the on-module EEPROM manifest is
blank (see `0004`'s Linux-side cross-check above), so the
EEPROM-manifest gate never opens; `CONFIG_ALP_E1M_DEEPX_RAIL` on its
own is enough to run the rail step, but the PCIe path still needs a
programmed manifest, unrelated to this patch. The rail's
BENCH-PENDING marker is retired; PCIe stays BENCH-PENDING until a
programmed EEPROM is available to test against.

**Long-read tail-byte corruption found and fixed, 2026-09-24
(E1M-V2M103, RIIC8/BRD_I2C).** A follow-on read past the single-
register pokes above -- `i2c md 0x1e 0x00.1 0x10` (16 bytes from the
DA9292, covering the already-verified `VOUT_CH2` pair) -- came back
`03 00 02 00 3f 07 01 03 35 aa a0 b8 96 16 7f 22`: byte offsets `0x0D`
and `0x0E` read `0x16`/`0x7F` instead of the independently-verified
`0x96`/`0xFF`, bit 7 stuck clear on both, every other byte (including
offset `0x0C` = `0x96` and the true last byte, offset `0x0F` =
`0x22`) correct. Reproducible. A 1-byte and a 4-byte read of the same
registers (`i2c md 0x1e 0x0d.1 1`, `0x0e.1 1`, `0x0c.1 4`) both
returned correctly, so the defect is specific to longer transfers, not
the register values or the bus itself.

Root cause: `drivers/i2c/rzg2l_riic.c` `riic_i2c_raw_read()` (the
receive path both `riic_read_common()`'s offset-then-read and every
bare read in `riic_xfer()`'s fallback loop go through). `ICMR3.WAIT`
is set once in `riic_init_setting()` and held for the RIIC instance's
whole life; with `WAIT` held, the read of `ICDRR` for a byte is what
releases the wait and commits whatever `ICMR3.ACKBT` (ack/nack) and
`ICCR2.SP` (stop-request) state is current at that instant onto the
bus for the byte just consumed. The previous shape read `ICDRR` for
the final byte in a split do-while-loop-plus-post-loop structure and
only set `ACKBT`/`SP` for that byte AFTER the read that consumed it --
one byte-time too late to affect the transfer already in flight, per
the RIIC master-receive flowchart. Every multi-byte read was therefore
ACKing (not NACKing) its true final byte and requesting `STOP` only
after the fact, including the 1-byte case (same post-hoc-set bug,
same missing NACK, just not visibly harmful there on this bench pass).
`drivers/i2c/rz_riic.c` `riic_receive_data()` -- a separate,
already-correct RIIC driver carried in the same tree for other
Renesas parts -- does this the right way: set `ACKBT`/`SP`, then read
`ICDRR`, on the iteration that consumes the final byte. Fixed in
`riic_i2c_raw_read()` by collapsing the buggy split loop into one
unified loop matching that same set-then-read ordering. Not proven to
be the singular cause of the specific bit-7-clear pattern above (no
scope trace of the affected bytes was captured), but it is an
independently real protocol-ordering defect matching the manual's
documented sequence and the proven-correct sibling driver in the same
file, fixed on those merits. Source-derived; not re-verified on
silicon this session (no board access) -- flagged for the next bench
pass to confirm the 16-byte read comes back clean.

Folded into `0006-rzv2n-dev-i2c-rzg2l_riic-p06-p07-pullup-clock-fix.patch`
in place (same filename, regenerated via `git format-patch` from a
fresh `bcf29d98` + PMIC-I2C-removal + `0001`-`0005` tree with the
prior `0006` content re-applied and this fix added on top, byte-copied
into the tree -- not hand-edited) rather than a new numbered patch,
since it is the same file, the same failure class (RIIC8/BRD_I2C
register reads), and the same bench investigation as the rest of
`0006`. New `0006` patch file: md5 `574ca8c093b17602eadab7a5830d35b0`.

**v5 rebuilt with the tail-byte fix, 2026-09-24** (fresh `bcf29d98`
worktree, independent of `ub-v4`: PMIC-I2C-removal + `0001` + `0002` +
`0003` (4 GB tier) + `0004` + `0005` + the new `0006`, each applied
via `git apply --check` then `git apply`, in that order, all seven
succeeding cleanly with zero fuzz; same config fragments as v4 --
`rzv2n-dev_defconfig` + `no-dirty-version.cfg` + `gigadevice-xspi.cfg`
+ `deepx-rail.cfg` + `fdtfile-v2m.cfg`; WSL Ubuntu-22.04,
`aarch64-linux-gnu` gcc 11.4.0): build `rc=0`. `strings | grep -c
'^ALP:'` = `26`, unchanged from v4 -- the fix reorders existing
register-set calls, it adds no new `printf`. `CONFIG_SYS_SDRAM_SIZE` =
`(0x100000000u - DRAM_RSV_SIZE)` and the built DT's `memory@48000000`
= `<0x0 0x48000000 0x0 0xF8000000>`, both still correct for the 4 GB
tier. `u-boot.bin`: md5 `cb1e030f8f39b0d06a55a9255a7a024d`, size
764328 bytes (`0xBA9A8`, unchanged from v4 -- the fix nets to the same
code size), crc32 `0xc61d6328`. BENCH-PENDING: unverified on silicon
-- v4's DEEPX rail pass above did not exercise this fix (found on a
later, longer read); the next bench pass should retry the 16-byte
`i2c md 0x1e 0x00.1 0x10` and confirm bytes `0x0D`/`0x0E` come back
`0x96`/`0xFF`.

**Bench matrix, 2026-09-24 (E1M-V2M103, RIIC8/BRD_I2C, DA9292 `0x1E`):
the `0005`-reorder fix above was REFUTED as the cause of the bit-7
loss; the real mechanism is a bus-speed margin issue, not a protocol-
ordering one.** A v4 build (without the `riic_i2c_raw_read()` reorder)
and the v5 build (with it) produced IDENTICAL corruption on the same
long read -- the reorder is a real, independently-correct protocol fix
kept on its own merits (it matches the RIIC master-receive flowchart
and the proven-correct `drivers/i2c/rz_riic.c riic_receive_data()`),
but it does not explain or fix the bit-7 loss. A systematic sweep at
`CKS=3/4/5` (plus `ICMR3.WAIT` held off and `ICBRL` forced to `0xff`,
both still corrupt) found:

- `CKS=4` (the value `0006` shipped with through v5, this changelog's
  own "restored nominal rate"): single bursts longer than ~12 bytes
  lose bit 7 on bytes with bit 7 set from about position 13 onward --
  position-dependent, not register-dependent (a 21-byte burst: offset
  `0x0d` `0x96`->`0x16`, `0x0e` `0xff`->`0x7f`, `0x12`/`0x13`
  `0xff`->`0x7f`, `0x14` `0xac`->`0x0c`). 8-byte reads are clean even
  at `CKS=4`.
- `CKS=5`: a 16-byte AND a single 32-byte `i2c read` burst are both
  completely clean.
- `CKS=3`: fails outright (STOP detected, `-110`) -- recovery kicks
  in, not a usable fallback.
- `ICMR3.WAIT` off (`0x30`) and `ICBRL` forced to `0xff`: still
  corrupt -- rules out `WAIT`/`ICBRL` as the mechanism.

Hardware context for the matrix: `BRD_I2C` carries roughly 13
populated branches; external pull-ups R287/R288 (2.2 kOhm to
`VDD1G_1P8`) ARE fitted per the netlist, in addition to the SoC-
internal pull-up `0006` already adds. Root mechanism of the bit-7 loss
at `CKS=4` is still NOT confirmed -- Linux drives the same physical
bus at 400 kHz, IRQ-driven, with clean reads, which does not obviously
square with "just needs a slower clock" -- a scope capture of the
affected bytes is still pending. `CKS=5` is carried on this bench
matrix (clean on every length tried; `CKS=4` is not), not on a proven
timing model.

**`0006` recut to `CKS(5)`/`CKS(3)` (STANDARD/FAST, was `CKS(4)`/
`CKS(2)`) plus two smaller diagnostics, 2026-09-24, same source tree
(`bcf29d98` + PMIC-I2C-removal + `0001`-`0005` applied, prior `0006`
re-applied and amended in place, regenerated via `git format-patch`,
not hand-edited).** `riic_set_clock()`'s R9A09G056/057 branch now
takes CKS one step past the doubled-clock correction as deliberate
margin for the loaded bus (see the matrix above), not a second clock-
doubling fix. Per the driver's own bit-rate formula, `fSCL = (P0phi /
2^CKS) / (m + n + 2)` with `m`/`n` = `ICBRH`/`ICBRL` bits `[4:0]` (the
`0xE0` upper bits are a fixed must-write-1 pattern, not part of `m`/
`n`), ignoring `tr`/`tf` (bus-loading-dependent, not computable here):
at 100 MHz input with `m = n = 23` (STANDARD, register write value
`ICBRH = ICBRL = 0xE0 | 23 = 0xF7`), `CKS(4)` computes to
`(100e6/16)/48 ~= 130.2 kHz` (the value that still loses bit 7 on this
bus) and `CKS(5)` to `(100e6/32)/48 ~= 65.1 kHz` (this patch's value,
bench-clean). For FAST rate (`m = 20`, `n = 19`), `CKS(2)` computes to
`(100e6/4)/41 ~= 609.8 kHz` and `CKS(3)` (this patch's value) to
`(100e6/8)/41 ~= 304.9 kHz`; FAST rate is not itself part of the bench
matrix (only STANDARD/RIIC8 was exercised on the bench), carried by
the same margin reasoning only. Two further diagnostics in the same
file: the `riic_read_common()` SDAI poll before the repeated START is
raised from its initial ~100 us bound to ~1 ms and now prints elapsed
time + `ICCR1` on expiry (the original bound was too tight to observe
anything useful on the matrix runs above); `riic_wait_for_icsr2()`'s
existing unconditional timeout printf gains `ICCR1` alongside `ICSR2`/
`ICCR2`, so any wait timeout also shows SDAI (bit 0), not only the
repeated-START path. New `0006` patch file md5:
`bcfa876cdbfe4fde5fdb000bba235ca5`.

The `meta-alp-sdk/recipes-bsp/u-boot/u-boot_%.bbappend` comment for
`0006` and this changelog entry are rewritten to match: the `0005`-
style reorder is described as a real but bench-refuted-as-root-cause
protocol fix, the actual fix is the `CKS(5)`/`CKS(3)` bus-speed
margin, and the root mechanism is flagged unconfirmed pending a scope
capture. The DT comment in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`
(`&pinctrl`, `i2c0_pins`/`i2c8_pins` block) is also corrected: it
previously left RIIC8/BRD_I2C's external pull-up population as TBD
alongside RIIC0/RIIC1's; RIIC8 specifically is now confirmed
populated (R287/R288, 2.2 kOhm to `VDD1G_1P8`, per the netlist) --
RIIC0/RIIC1 stay TBD. The SoC-internal pull-up `0006` adds on RIIC8 is
still bench-confirmed required in addition to those external ones, not
instead of them.

**v6 rebuilt with the recut `0006`, 2026-09-24** (fresh `bcf29d98`
worktree, independent of v4/v5: PMIC-I2C-removal + `0001` + `0002` +
`0003` (4 GB tier) + `0004` + `0005` + the recut `0006`, each applied
via `git apply --check` then `git apply`, in that order, all seven
succeeding cleanly with zero fuzz; same config fragments as v4/v5 --
`rzv2n-dev_defconfig` + `no-dirty-version.cfg` + `gigadevice-xspi.cfg`
+ `deepx-rail.cfg` + `fdtfile-v2m.cfg`; WSL Ubuntu-22.04,
`aarch64-linux-gnu` gcc 11.4.0): build `rc=0`. `strings | grep -c
'^ALP:'` = `26`, unchanged from v4/v5 -- the SDAI-timeout and
`ICCR1`-in-timeout diagnostics added this pass use the same `%s:`-
prefixed format the existing `riic_wait_for_icsr2()` timeout message
already used, not the `ALP:`-prefixed style this count tracks, so they
add no new matches. `CONFIG_SYS_SDRAM_SIZE` = `(0x100000000u -
DRAM_RSV_SIZE)` and the built DT's `memory@48000000` still reflects
the 4 GB tier, unchanged from v4/v5. `u-boot.bin`: md5
`314ff00766eec301cd5e25fe4b9f1a3e`, size 764408 bytes (`0xBA9F8`),
crc32 `0xaee82e39`. BENCH-PENDING: unverified on silicon -- the next
bench pass should retry the 21-byte and 32-byte `i2c md`/`i2c read`
bursts on RIIC8 that motivated `CKS(5)` and confirm both come back
byte-for-byte clean.
