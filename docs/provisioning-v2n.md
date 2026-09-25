# Provisioning an E1M-X V2N / V2N-M1 SoM

`scripts/provision_som.py plan | run | status` provisions one V2N-family
module (bundle `family` `v2n` or `v2n-m1`) from blank silicon to a recorded
ledger row. The flat `provision_som.py --bundle ...` form described in
[provisioning.md](provisioning.md) is unchanged and still serves the other
families.

Everything bench-specific stays in the **private** repository and is passed
in by path: the bench description (`--bench bench.yaml`: console, power,
debug probe, Linux host, Flash Writer location, I2C bus numbers), the DDR
tier markers (`--tier-markers`), the expected PMIC registers
(`--pmic-expect`) and the ledger itself (`--ledger-root`). Nothing in the
public tree names a bench, a unit serial or a memory/storage part number.
The bench runbook (switch positions, cabling, per-bench commands) lives in
the private repository.

## Fixed decisions

1. **No SD boot ROM path.** A plain microSD is not a boot source on this
   SoC. The flow is: SCIF download mode → the Flash Writer's `EM_W` puts a
   *transient* eMMC-boot BL2 (`bl2_mmc`) and the FIP into eMMC **boot1** →
   the unit boots U-Boot from eMMC → U-Boot boots the release wic from the
   microSD (U-Boot patch 0008 enables SDHI1 as `mmc1`) → **Linux performs
   every production write** (xSPI, eMMC boot1, eMMC user area, EEPROM, GD32,
   secure page).
2. **`mfg_date` is the Monday of the serial's ISO week**
   (`YYYYWww-NNNN` → `date.fromisocalendar(YYYY, ww, 1)`). A `--mfg-date`
   that differs is recorded as an override.
3. **Secure Data Page: write + verify now, lock later.** `run --lock` is a
   separate invocation behind the preconditions below.
4. **Known power-chip defect units are bench-only.** When the ACT88760
   register `0x10` reads its known-bad value, the tool applies the volatile
   workaround, records `act88760_gpio4_defect: yes` (never downgraded by a
   later run), defaults `disposition` to `bench-only`, and the ship check
   refuses the unit. The tool never sets a shippable disposition.

## Commands

| subcommand | touches hardware | writes |
|---|---|---|
| `plan` | only with `--bench`, read-only probes | nothing |
| `run` | yes | only with `--execute`; the lock only with `--lock` |
| `status` | no | nothing |

`run` without `--execute` runs every read-only probe and prints, per step,
the exact commands it would issue. `--lock` is never implied by `--execute`.

```bash
python scripts/provision_som.py plan --sku <SKU> --bundle <bundle-dir> \
    --ledger-root <private>/ledger --tier-markers <private>/metadata/ddr-tier-markers.json
python scripts/provision_som.py run --execute --sku <SKU> --serial <serial> \
    --bundle <bundle-dir> --bench <private>/bench/<bench>.yaml \
    --ledger-root <private>/ledger --tier-markers <...> --pmic-expect <...> --gd32-fw <dir>
python scripts/provision_som.py status --sku <SKU> --serial <serial> --ledger-root <private>/ledger
```

Step selection: `--only`, `--from`, `--skip`, `--force-step` (`preflight`
always runs). `--build-dir` builds an unsigned bundle from a deploy
directory for bench work; such a unit is recorded `bench-only` and the ship
check refuses it.

## Package `scripts/provision/`

| module | responsibility |
|---|---|
| `bench.py` | Console (serial / TCP), Power (SCPI / labgrid / manual), Probe (J-Link / wrapper script), Operator prompts, `load_bench()`; the one `expect()` loop |
| `uboot.py` | U-Boot / BL2 console interaction, banner parsing, XMODEM `loadx` fallback |
| `scif_writer.py` | boot-ROM SCIF download, Flash Writer `EM_W` / `EM_SECSD` / `EM_DCID`, bin → S-record |
| `linux_target.py` | SSH runner, console login, MTD / eMMC / I2C helpers, the read-only census |
| `gates.py` | offline checks: SKU and DDR tier triangles, FIP rail string, FDT, artefact hashes, the N24S128 frame table, the `mfg_date` rule |
| `steps.py` | the step machine and the state file |
| `ledger_out.py` | `unit.yaml` merge, markdown section, logs, xlsx regen, ship check |

## Step machine

Every step has a read-only `probe()` (`Satisfied` / `Unsatisfied` /
`Unknown`) and a `run()` whose every mutation goes through
`ctx.mutate()`, which only records the description in a dry run. The runner
walks the steps in order: probe; `Satisfied` → skip; otherwise run and
**re-probe** — a step is `done` only when the re-probe is `Satisfied`
(operator steps, `bootstrap` and `cold_boot_test`, whose result cannot be
observed right away, count as done after a clean run). The first failure
stops the run, but **`record` still runs**, so what was learnt (the census,
a defect) reaches the ledger.

Progress is kept in `ledger/<SKU>/<serial>.state.json` in the private
ledger: per-step status and evidence, the tool revision, the bundle
sha256 and every override. The state file is evidence, not authority: a
step recorded `done` whose probe now says `Unsatisfied` runs again, and a
state recorded against a **different bundle or tool revision** is moved to
`superseded` so every step runs again.

## Steps

| step | what it does |
|---|---|
| `preflight` | offline gates: bundle schema, artefact sha256 / size / role set (`bl2`, `bl2_mmc`, `fip`, `system_image`), xSPI and boot1 size limits, family, SKU triangle, DDR tier triangle, FIP rail string (`v2n-m1`), FDT present in the wic `/boot`, N24S128 frame-table self-check |
| `detect` | power cycle and classify the console: SCIF ROM, BL2, U-Boot, Linux login, silent |
| `dsw1_scif` | operator: boot switch to SCIF download |
| `bootstrap` | Flash Writer: `EM_W` boot1 sector `0x1` ← `bl2_mmc`, sector `0x300` ← `fip`; `EM_SECSD` EXT_CSD `[177]=0x02` (BOOT_BUS_CONDITIONS), `[179]=0x08` (PARTITION_CONFIG); `EM_DCID` |
| `dsw1_emmc_insert_sd` | operator: boot switch to eMMC, insert the release microSD; U-Boot must autoboot |
| `boot_sd_linux` | U-Boot boots the wic from microSD; log in, find the host, confirm the root is on the SD. Fallback `--transfer xmodem`: `loadx` + `gzwrite` the wic from the U-Boot prompt |
| `write_xspi` | from Linux: `bl2` → `mtd0`, `fip` → `mtd1`; md5 readback. A FIP whose erase would reach the CM33 image at `mtd1` + `0x1A0000` is refused |
| `write_emmc_boot` | release `bl2_mmc` + `fip` into `mmcblk<N>boot1`, md5 readback, EXT_CSD via mmc-utils |
| `write_rootfs` | stream the wic into the eMMC user area (refused while Linux runs from the eMMC), `fsck -n`, `/boot/<dtb>` present |
| `census` | read-only: every auto ledger key the unit can provide |
| `eeprom_manifest` | preconditions, 128-byte manifest in 8 × 16-byte page writes at `0x50`, readback, cold cycle, re-read; only then the staged blob is promoted to `<serial>.manifest.bin` |
| `gd32_flash` | DP-ID gate (`0x0BE12477` only), `loadbin` × 3, verify with `savebin` in fresh probe sessions, bridge ACK at `0x70` |
| `dxm1_npu_flash` | `v2n-m1` only, **skipped by default** (`--enable-dxm1-flash`): DX-M1 SPI-NAND over the UART recovery path. **BENCH-PENDING** -- see below |
| `pmic_verify` | compare registers against `--pmic-expect` |
| `secure_page` | write the 64-byte Secure Data Page, read back, compare. Never locks |
| `dsw1_xspi_remove_sd` | operator: boot switch to xSPI, remove the microSD |
| `cold_boot_test` | `--cold-cycles N`: clean BL2, DRAM tier, rail line (`v2n-m1`), login, `SYS_LSI_MODE`, I2C scans |
| `clkgen_verify` | the on-SoM 5L35023B (`BRD_I2C`, `0x69`) OTP image against U-Boot's fixup |
| `hil_smoke` | optional `tests/hil/run_smoke.py` |
| `record` | merge auto keys into `<serial>.unit.yaml` (manual keys never touched), append `<serial>.md`, logs, xlsx, ship check |

Every Linux step finds the eMMC by its sysfs type, never by an index: SD and
eMMC numbering is not stable across kernels and boot sources.

### DDR tier triangle

The private marker file maps each tier label to a byte pattern found in the
BL2 and each SKU to its tier (`sku_tier`: label + Mbit). The gate requires
the SKU's label == the label found in **every** BL2 == the bundle's
`memory_tier.label` (when given), and the SKU's Mbit == preset `dram_mbit`
== bundle `memory_tier.dram_mbit` == the U-Boot `DRAM:` banner (rounded to
a power of two; a 4 GiB unit prints `3.9 GiB`). Two tiers of the same size
exist, so the Mbit legs alone cannot catch a wrong-tier BL2.
`--allow-tier-mismatch REASON` overrides a disagreement (never an
unreadable BL2 or an unknown SKU); the override is recorded and blocks
shipping.

### N24S128 identity header (`0x58`)

The first pointer byte is a selector; a write with the wrong selector can
permanently change the part. Only these frames can be built (by
`gates.identity_frame()`), and the Linux helper refuses any other `0x58`
transfer:

| op | frame | used by |
|---|---|---|
| secure page write | `0x00 0x00` + 64 bytes | `secure_page` |
| secure page read | `0x00 0x00`, read 64 | verify, census, lock preconditions |
| unique ID read | `0x02 0x00`, read 16 | census |
| lock status read | `0x04 0x00`, read 1 | preconditions, census |
| lock | `0x04 0x00 0xFF` | `run --lock` only |

Selector `0x06` (device configuration) is only ever read, as one combined
`0x06 0x00` + read-1 transfer; a unit test asserts no frame writes it.

### `clkgen_verify`: the 5L35023B clock generator (`0x69`)

The on-SoM Renesas 5L35023B (`BRD_I2C` / Linux `i2c-8` on the reference
bench, 7-bit `0x69`) ships with a fixed factory OTP image that cannot be
re-burned in-system. U-Boot patch 0007 (#2293) rewrites reg `0x21` and
`0x24` every boot (a volatile fixup, not an OTP change); the step reads reg
`0x00..0x24` **one byte at a time** (`i2cget`, never a combined
`i2ctransfer` read -- this part bit-slips on those), compares against the
OTP image with `0x21`/`0x24` expected at their post-fixup values, and
confirms the boot console showed U-Boot's own `ALP: 5L35023B clock:` line.
**A boot log without that line means the unit's U-Boot lacks patch 0007
(#2293)** -- the step keeps failing (a production unit without the fixup is
a real defect, not a soft warning it can look past). Ledger facts:
`clkgen_otp_raw` (all 37 bytes as read, hex), `clkgen_i2c_addr`.

### `dxm1_npu_flash`: the DX-M1 NPU (V2M only) -- BENCH-PENDING

The DX-M1 must be strapped for SPI-NAND/UART recovery boot per the internal
hardware notes before this step can do anything real, so it is **skipped by
default** and needs `--enable-dxm1-flash`. **This path has never run on
silicon and cannot succeed on the first V2M bench unit yet** (its DX-M1
does not start its reference clock) -- do not pass `--enable-dxm1-flash` on
that unit. When enabled, from Linux it drives V2N `P75` high (UART mux to
the DX-M1 UART0, default low; held for the whole transfer -- a plain
foreground `gpioset` would set the line then exit and release it), pulses
`PA6` (DX-M1 reset: low 100 ms, high), and -- with the NAND empty, the
ROM's XMODEM fallback ('C' prompt @115200) -- runs the vendor `uart_boot`
(aarch64) twice against `-d <dev>`: the bootloader stage
(`-f fw_uart_boot.bin -b 115200`), then the application firmware
(`-F fw.bin -U -b 115200`); the `P75` hold is released afterward.
Verification is a cold boot followed by a DEEPX PCIe **endpoint**
enumerating under `/sys/bus/pci/devices` -- the root port alone
(`0000:00:00.0`) never counts; an optional `dxm1.pcie_vendor_id` in
bench.yaml narrows the match further once DEEPX publishes the DX-M1's PCI
IDs. **Never runs `sf_erase`** (vendor docs disagree on its size) and
**never touches V2N `P64`/`P65`** (the DEEPX 0.75 V rail -- `gpiolib`
reconfigures a pin on read and would kill it; see #2288). The vendor
`uart_boot` tool and firmware binaries are DEEPX files and are never
committed here: their paths come from `bench.yaml`
`dxm1.{uart_boot,fw_uart_boot,fw}` (alongside
`dxm1.{gpio_chip,uart_mux_line,reset_line,uart_device}`), each `TBD (null)`
until a bench has them. `uart_mux_line`/`reset_line` are **within-chip
gpiochip line numbers** (`port * 8 + pin`, e.g. `P75` = 61, `PA6` = 86 --
not the legacy sysfs `/sys/class/gpio/gpio<N>` numbering, which adds a
per-SoC base offset), must be distinct ints, and the tool refuses `52`/`53`
(`P64`/`P65`, the DEEPX rail) outright. The step verifies both firmware
files' md5 and the vendor `uart_boot` binary's own md5 against pinned
values before touching hardware, and records `dxm1_fw_uart_boot_md5`,
`dxm1_fw_md5`, `dxm1_fw_version`, `dxm1_uart_boot_tool_md5` in the ledger.

Example `bench.yaml` shape (every value `TBD (null)` until a bench has one):

```yaml
dxm1:
  gpio_chip: null          # e.g. "gpiochip0"
  uart_mux_line: null      # int, within-chip line number (P75 = 61)
  reset_line: null         # int, within-chip line number (PA6 = 86); != uart_mux_line
  uart_device: null        # e.g. "/dev/ttySC1"
  uart_boot: null          # path to the vendor uart_boot binary
  fw_uart_boot: null       # path to the pinned fw_uart_boot.bin
  fw: null                 # path to the pinned fw.bin
  pcie_vendor_id: null     # optional, e.g. "0xXXXX" (real ID TBD); narrows the PCIe endpoint match
```

### Lock preconditions (`run --lock`)

All must hold: `secure_page` and `cold_boot_test` done for this bundle;
`<serial>.manifest.bin` committed and byte-equal to the array; the Secure
Data Page re-read equals `<serial>.secure-page.staged.bin`; Lock Status
bit 1 clear; the unit passes the ledger ship check (disposition `ship`, no
defect, no override, not a `--build-dir` unit); and the operator retypes the
serial. After the lock frame, only a re-read with bit 1 set counts.

## Hazards

- **The lock is permanent.** Nothing undoes it; that is why it is a separate
  invocation with a retyped serial.
- **`mtd1` + `0x1A0000` holds the CM33 image.** The FIP write never erases
  into it.
- **The eMMC user-area write destroys the running root** if Linux booted from
  eMMC; `write_rootfs` refuses that case.
- **SD boot is not verified on silicon yet.** With U-Boot patch 0008 any
  inserted card passes `mmc dev 1`; the boot command takes the SD branch only
  when the card also has `boot/Image` on partition 2, else it boots the eMMC.
  If the carrier SDIO mux is driven by a blank GD32 the card may be
  unreachable (#2282); U-Boot then silently boots the eMMC and
  `boot_sd_linux` refuses because the root is not on the SD.
- **The EEPROM array is written only when blank** or equal to
  `--reprovision-from`, and never while the identity header is locked.
- **The 5L35023B cannot be re-burned in-system.** `clkgen_verify` only reads;
  a bad OTP image means a bad unit, not a fixable one.
- **`dxm1_npu_flash` is bench-pending and defaults off.** It has never run on
  silicon and cannot succeed on the first V2M bench unit yet (its DX-M1 does
  not start its reference clock). Do not pass `--enable-dxm1-flash` until the
  mechanism is bench-verified; the DX-M1 UART mux and reset lines are
  otherwise untested on real hardware.

## Testing

Every module has pytest tests with fakes (`tests/scripts/provision_fakes.py`
and `tests/scripts/test_provision_*.py`); no hardware is needed.
