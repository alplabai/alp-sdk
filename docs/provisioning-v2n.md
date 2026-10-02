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
4. **An ACT88760 GPIO4 OTP of `0x88` is an early-unit condition with a known
   workaround, not a defect.** Most units' factory OTP already holds register
   `0x10` at `0x08`; a few early units (e.g. E1M-V2M103 2026W38-0001) have OTP
   `0x88`, which holds GD32_NRST in reset until released (maintainer decision
   2026-09-29). U-Boot's `board_late_init` releases it every boot (a no-op on
   an already-`0x08` unit). When the tool itself still finds `0x10 == 0x88`
   (`census`, `gd32_flash`), it applies the same volatile release (`0x10 =
   0x08`, lost at power-off) and records how the release happened in
   `act88760_gpio4_workaround`: `none` (OTP already `0x08`), `u-boot` (an
   0x88-OTP unit U-Boot released on its own by the last cold boot), or
   `provision (volatile 0x08)` (the tool had to release it). `cold_boot_test`
   re-reads `0x10` after every cold boot and records
   `act88760_gpio4_after_boot`; the ship check refuses a unit only when that
   read is still `0x88` -- i.e. the shipped image did not release the GD32 on
   its own. A legacy `act88760_gpio4_defect: yes` key from before this
   decision is informational only and no longer blocks shipping.

## Commands

| subcommand | touches hardware | writes |
|---|---|---|
| `plan` | only with `--bench`, read-only probes | nothing |
| `run` | yes | only with `--execute`; the lock only with `--lock` |
| `status` | no | nothing |

`status` exits 0 after printing; `--require-shippable` makes a blocked ship
check, or a state `run` would supersede (another tool revision or, with
`--bundle`, another bundle), exit 1. It prints `STALE` for the latter.

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

`run --hw-rev <key>` sets this unit's hardware revision when it differs from
the bundle/preset default (a batch can mix revisions). The key must be
status `production` in the family's `hw-revisions.yaml` (exact case, e.g.
`r2`); anything else exits rc 2 and lists the production keys.

### SSH and unit identity

The tool SSHes into the unit with `StrictHostKeyChecking=no`,
`UserKnownHostsFile=/dev/null`, `ConnectTimeout=5`, `LogLevel=ERROR` and
`BatchMode=yes`: bench units get reused DHCP addresses and the SD and eMMC
images carry different host keys by design, so a host key identifies nothing
here.

The real risk is mutating the wrong unit behind a stale IP, so identity is the
eMMC CID. `bootstrap` records `emmc_cid_raw` (from `EM_DCID`) and `census`
records it again from sysfs. Only `Ctx.need_linux()`, the gateway every
Linux-mutating step goes through, checks: it reads the eMMC CID over SSH (found
by sysfs `device/type`, like census) and compares it with the recorded one. The
read is fresh on every call (nothing is cached, so a power cycle or DHCP renewal
is caught), and `secure_page_lock` calls `need_linux()` again immediately before
the irreversible lock write. Probes, `detect`, `linux_up()` and the console are
not checked.

The recorded CID is the first found of: `emmc_cid_raw` in any finished step of
the current state, in a `superseded` state (the CID is hardware and survives a
bundle or tool revision change), then `emmc_cid_raw` in the committed
`<serial>.unit.yaml`. A `--accept-cid-change` anchor (`cid_anchor` in the state
file) outranks all of them.

The compare covers MID..MDT (first 15 bytes, lower-cased, whitespace stripped)
with the reserved bits 119:114 masked. The last byte is not compared: the writer
does print the CRC field, but `scif_writer.parse_cid` rebuilds the register with
the reserved bits cleared and the end bit forced to 1, and kernel host drivers
differ in the last byte they return (SDHCI drops the CRC7 + end-bit byte). A
mismatch is refused:

    <host> answers with eMMC CID <seen>, but unit <serial> recorded <expected>:
    another unit is behind this address (stale DHCP lease?). Fix the address in
    bench.yaml and re-run, or --accept-cid-change REASON if the eMMC was replaced.

Limitation: first contact with no recorded CID is unchecked. On the normal
blank-unit path `bootstrap`'s `EM_DCID` records the CID, so every later step is
checked.

`run --accept-cid-change REASON` is the escape for a legitimately replaced eMMC:
it adopts the CID seen over SSH as the new anchor. The `emmc_cid_change`
override (blocks shipping like the other overrides) is recorded only when a
recorded CID existed and differed, not on first contact.

## Package `scripts/provision/`

| module | responsibility |
|---|---|
| `bench.py` | Console (serial / TCP), Power (SCPI / labgrid / manual), Probe (J-Link / wrapper script), Operator prompts, `load_bench()`; the one `expect()` loop |
| `uboot.py` | U-Boot / BL2 console interaction, banner parsing, XMODEM `loadx` fallback |
| `scif_writer.py` | boot-ROM SCIF download, Flash Writer `EM_W` / `EM_SECSD` / `EM_DCID`, bin → S-record |
| `linux_target.py` | SSH runner, console login, MTD / eMMC / I2C helpers, the read-only census |
| `gates.py` | offline checks: SKU and DDR tier triangles, FIP rail string, FDT, artefact hashes, the N24S128 frame table, the `mfg_date` rule |
| `steps.py` | the step machine and the state file |
| `payload_store.py`, `store_swd_probe.py` | the provisioning SD's payload store: on-board hash-checked reads, the console SWD probe that uses it |
| `bmap_writer.py` | the python3 range writer `write_rootfs` runs on the board |
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
a GPIO4 workaround applied) reaches the ledger.

Progress is kept in `ledger/<SKU>/<serial>.state.json` in the private
ledger: per-step status and evidence, the tool revision, the bundle
sha256 and every override. The state file is evidence, not authority: a
step recorded `done` whose probe now says `Unsatisfied` runs again, and a
state recorded against a **different bundle or tool revision** is moved to
`superseded` so every step runs again.

## Power safety

The RTL8211F(I) PHY needs both 3.3 V and its 1.0 V rail at 0 V when the 3.3 V
source is toggled, with a period over 100 ms (datasheet Rev 1.7, Table 53 notes
1-2). The tool therefore enforces, below any `bench.yaml` setting: an OFF dwell
of at least **10 s** (`power.off_s` below that is raised to 10 s with a
warning; default 15 s) and at least **5 s** ON before any OFF, counted from the
last ON this process issued or, for a fresh process, assumed to be just now.
Every PSU command is logged with a monotonic timestamp in the step log.

### Clean shutdown before every cut

A hard power cut while the microSD root was mounted corrupted its ext4 journal
(`JBD2: journal transaction ... is corrupt`, kernel panic on the next boot). Before
every tool-driven power cut `clean_shutdown` tries to halt a running Linux. It does
not promise that: it records what it did, and some cases are not covered.

Where it runs: `detect`, `boot_to_linux` (so `boot_sd_linux`, the `eeprom_manifest`
and `gd32_flash` cold cycles, every `cold_boot_test` cycle, `poweroff_and_cold_boot`),
the `bootstrap` cycle, the xmodem path of `boot_sd_linux`, and, before the operator is
asked to switch power off or pull the card, `dsw1_emmc_insert_sd` and
`dsw1_xspi_remove_sd` (their prompts say whether the unit was halted).

How it reaches the unit, in this order:

1. This boot's console login: one console line `sync; echo "ALPS<nonce>:$?"; poweroff`.
2. SSH to the pinned (`bench.yaml` `linux.host`) or discovered host, but only after that
   host has written a nonce to its `/dev/console` and the nonce was read on this unit's
   serial console (a stale DHCP lease can point at another unit). Over SSH:
   `sync; echo ALPSYNC:$?; poweroff`.
3. A console whose state is unknown (no login of this tool on this power-on, no proven
   SSH host): Ctrl-C only, read 2 s. A `=> ` prompt means U-Boot, no shutdown needed.
   Otherwise a console login (5 s); on a shell, path 1. Never a bare Enter (U-Boot would
   repeat its last command). A unit known to sit in the SCIF ROM or the Flash Writer
   gets nothing sent: this tool's parsers send those only CR-terminated lines and never
   `0x03`, so Ctrl-C there is untested.

Then it waits up to 120 s for the console halt line (`reboot: Power down` or
`System halted`), plus 5 s more before it gives up. It is done once per power-on.
The PSU current is not a halt signal. The OFF dwell is unchanged, so a clean poweroff
followed by it is still a cold boot, and `cold_boot_test` keeps its `--cold-cycles` (3).

Every cut is recorded as the step evidence key `power_cut` (and in the step log and the
plan log), one entry per cut:

| value | meaning |
|---|---|
| `clean` | `poweroff` sent and the halt line seen (`(late)` if it came in the extra 5 s); `sync rc=` says what the first `sync` returned |
| `fallback` | no halt line. Over SSH a second `sync` ran and its rc (or error) is recorded; on the console no second command is sent (the first may still be running, and a new command would Ctrl-C it) |
| `blind` | no console shell and no SSH path proven to be this unit. If Linux is up the cut is hard; the entry says so |
| `not-needed` | PSU already off, U-Boot prompt, SCIF ROM / Flash Writer, or already halted on this power-on |

Not covered: a kernel that is still booting (no shell yet, so the probe finds nothing and
the cut is `blind`), a console shell that does not answer, a unit that is hung, and the
manual or labgrid power kinds (their prompts and commands are still preceded by this
shutdown, but nobody checks what the operator does). The `reboot: Power down` line was
seen on this image on 2026-10-02 in a manual session, not by this code; the halt-line
text, the 120 s bound and all of the above are not yet confirmed by a run of this code on
a bench.

## Steps

| step | what it does |
|---|---|
| `preflight` | offline gates: bundle schema, artefact sha256 / size / role set (`bl2`, `bl2_mmc`, `fip`, `system_image` -- enforced here, not by the bundle schema, because flat-flow bundles such as som-0.2.0 legitimately carry no `bl2_mmc`), xSPI and boot1 size limits, family, SKU triangle, DDR tier triangle, FIP rail string (`v2n-m1`), FDT present in the wic `/boot`, N24S128 frame-table self-check |
| `detect` | clean shutdown if Linux is up, then power cycle and classify the console: SCIF ROM, BL2, U-Boot, Linux login, silent. The ROM banner `SCI Download mode (Due to parameter error)` is the ROM's fallback to SCI download, taken when the selected boot source has no valid image (blank xSPI/eMMC, or DSW1 not in mode 3). It still offers `-- Load Program to SRAM`, but it is **refused**: "boot ROM fell back to SCI download because the selected boot source has no valid image (blank xSPI/eMMC, or DSW1 not in mode 3); set DSW1 to mode 3 for a clean SCIF bootstrap". Only `(Normal SCI boot)` proceeds |
| `dsw1_scif` | operator: boot switch to SCIF download |
| `bootstrap` | reuses the live SCIF ROM state `detect` left in this run (no second power cycle); cycles only if no ON has happened since. Flash Writer: `EM_W` boot1 sector `0x1` ← `bl2_mmc`, sector `0x300` ← `fip`; `EM_SECSD` EXT_CSD `[177]=0x02` (BOOT_BUS_CONDITIONS), `[179]=0x08` (PARTITION_CONFIG); `EM_DCID` |
| `dsw1_emmc_insert_sd` | operator: boot switch to eMMC, insert the release microSD; U-Boot must autoboot |
| `boot_sd_linux` | U-Boot boots the wic from microSD; log in, find the host, confirm the root is on the SD, then the live SoM check: every non-optional on-module I2C device the SoM preset declares must ACK (the GD32 excepted) before any destructive step runs. Fallback `--transfer xmodem`: `loadx` + `gzwrite` the wic from the U-Boot prompt. No IP is not a failure here: the checks run over the console and `gd32_flash` runs next |
| `gd32_flash` | applies the ACT88760 GPIO4 volatile release if still at the OTP default, DP-ID gate (`0x0BE12477` only), `loadbin` × 3, verify with `savebin` in fresh probe sessions, bridge ACK at `0x70`. With no network (the `boot_sd_linux` evidence says why: `network: none (gbeth DMA reset failed on <ports>)` or `none (no carrier)`) it pushes the SWD tools and the three images over the console (base64, md5-checked on the board), then cold-cycles and re-checks the IP; SSH is used when it is up |
| `write_xspi` | from Linux: `bl2` → `mtd0`, `fip` → `mtd1`; md5 readback. A FIP whose erase would reach the CM33 image at `mtd1` + `0x1A0000` is refused |
| `write_emmc_boot` | release `bl2_mmc` + `fip` into `mmcblk<N>boot1`, md5 readback, EXT_CSD via mmc-utils |
| `write_rootfs` | stream the wic into the eMMC user area (refused while Linux runs from the eMMC), `fsck -n`, read-only mount (`-o ro,noload`, so the ext4 journal is never replayed), `/boot/<dtb>` present. With a `system_image_bmap` in the bundle only the mapped ranges are written and verified, see below |
| `census` | read-only (one throwaway file in `/tmp`): every auto ledger key the unit can provide, plus the keys listed under "Census keys" below |
| `eeprom_manifest` | preconditions, 128-byte manifest in 8 × 16-byte page writes at `0x50`, readback, cold cycle, re-read; only then the staged blob is promoted to `<serial>.manifest.bin` |
| `dxm1_npu_flash` | `v2n-m1` bundles that carry the DX-M1 set: firmware over the ROM UART path, probe = PCIe `0x0000` + `dxrt-cli -s` version, see below |
| `pmic_verify` | compare registers against `--pmic-expect` |
| `secure_page` | write the 64-byte Secure Data Page, read back, compare. Never locks |
| `dsw1_xspi_remove_sd` | operator: boot switch to xSPI, remove the microSD |
| `cold_boot_test` | `--cold-cycles N`: clean BL2, DRAM tier, rail line (`v2n-m1`), login, `SYS_LSI_MODE`, ACT88760 reg `0x10` after boot, I2C scans (GD32 exempted only while `0x10` still reads `0x88`). An `end0` without carrier (PHY latch, #2582) gets one extra cold cycle, noted as `end0_no_carrier_retries` in the step evidence (not a ledger key); it fails if `end0` is still down |
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

### `dxm1_npu_flash`: the DX-M1 NPU firmware (V2M only)

Programs the DX-M1's SPI-NAND through the ROM's UART (XMODEM) path, so no
manual procedure is left. It applies to a `v2n-m1` bundle that carries the
DX-M1 programming set; without it (or on another family) the step is
`skipped` with the reason. The firmware is license-gated and is **never in
this repository**: the files ride in the alp-sdk-internal release bundle as
optional components, `flash_target` `dxm1`
(`metadata/schemas/som-release-bundle-v1.schema.json`):

| role | file | notes |
|---|---|---|
| `dxm1_fw` | application firmware (`fw_no_pmic_gpio.bin`) | carries the expected firmware `version` (e.g. `2.4.0`), required |
| `dxm1_fw_uart_boot` | UART bootloader stage (`fw_uart_boot_no_pmic_gpio.bin`) | |
| `dxm1_dxflash` | the `dxflash.py` helper | |
| `dxm1_dtb` | dxuart2 device tree (pinctrl `sci1-dx`, `serial@12801000` okay, `pcie@13400000` disabled, alias `serial1`) | |
| `dxm1_dxcli` | `dxcli.py` | only needed to erase a NAND that already holds boot2nd |

**Precondition (hardware, not measurable by the tool):** the DX-M1
BOOT_CFG straps must be mode 0, i.e. E1M `IO17`, `IO19` and `IO20` low. The
X-EVK pulls them up, so the carrier needs a rework (1k to GND on each). A
wrong strap is recognised from the ROM output (`pcie boot(1/3) .. (3/3) failed
(0x1) PWD:` with no XMODEM `C`) and the step fails with a message naming the
strap rework.

**Probe (already satisfied):** the DX-M1 PCIe device
`/sys/bus/pci/devices/0000:01:00.0/device` reads `0x0000` (firmware running;
`0x0001` is the ROM's own PCIe boot) **and** `dxrt-cli -s` reports the
bundle's firmware version. **And** the `dxm1_fw_md5` last recorded for this unit (state file, else ledger
`unit.yaml`) equals the bundle's `dxm1_fw` md5: two firmware variants can report the
same version. No record, or a different md5, runs the step (a NAND that already
holds firmware takes the `sf_erase` path first, the proven `dx_update.sh` step 1).
`--force-step dxm1_npu_flash` forces a run.

**Run**, serial, over the unit's network target:

1. push only the dxuart2 DTB to `/tmp` (md5 checked on the target); remember
   whether the NAND already runs boot2nd (PCIe device `0x0000`). `/tmp` is
   tmpfs, so the flash payload cannot be pushed yet;
2. back up `/boot/<fdtfile>` as `.release` (the release md5 is recorded as
   soon as the backup verifies, before the live file is touched), install the
   dxuart2 DTB, warm `reboot` (the UART appears, the PCIe link is off), then
   check the dxuart2 DTB really booted (`/sys/bus/platform/devices/12801000.serial`
   present, DX-M1 PCIe device gone) before touching any GPIO;
3. push `fw`, `uart_boot`, `dxflash.py` (and `dxcli.py` when erasing) to `/tmp`
   AFTER the reboot, md5 checked on the target; a mismatch aborts before the
   erase and the flash;
4. if boot2nd was running: `sf_erase 0 1000000` through `dxcli.py`, read back
   the first RTOS bytes. NO PROMPT from the boot2nd CLI is a **failure** (the
   DX-M1 needs a cold cycle, as in `dx_update.sh`), and an RTOS slot that
   does not read `40000000: ffffffff` after `sf_erase` fails with the readback;
5. export `P75` (UART mux) and `PA6` (reset) high, start `dxflash.py
   <uart_boot.bin> <fw.bin>` in the background, 2 s later pulse `PA6` low for
   0.5 s, wait for it. Success is dxflash's **real exit code 0** (read from a
   file, not a pipe) **or**, when the process is still alive, all of the
   success markers (`update_firmware end. 0`, `good CRC`, no `bad CRC`,
   `jump to rtos` or `### DONE`) seen and 30 s (`EXIT_GRACE_S`) passed without
   it exiting: it is then killed (`pkill`, then `pkill -9`, and the tool waits
   until it is gone; a survivor fails the step and leaves the GPIOs exported).
   The markers print only after the NAND write: `update_firmware end. 0` follows
   the last erase / write / verify and precedes the DX-M1's self reset, and
   `good CRC` is printed by the second boot, from the NAND. The ledger records
   the real exit code, or `killed after its success markers` /
   `killed on timeout` / `killed (bad CRC)`;
6. restore the release DTB and verify its md5 (also after any failure past
   step 2, also when the copy over the live DTB itself fails; a leftover `.release` from an interrupted run is healed before a
   new backup, never overwritten);
7. clean shutdown (see "Clean shutdown before every cut"), cold cycle (the normal
   `MIN_ON_S` / `MIN_OFF_S` rules of `Power.cycle`), log in;
8. verify PCIe device `0x0000` and the `dxrt-cli -s` version.

`P75`/`PA6` are lines `61`/`86` on `10410000.pinctrl` by default; a bench.yaml
`dxm1:` block may override `gpio_chip`, `uart_mux_line`, `reset_line` (distinct
ints; `52`/`53` = `P64`/`P65`, the DEEPX 0.75 V rail, are refused before any
export, here and again in `_sysfs_gpio_dir`). The `dxrt-cli -s` parser reads only
the ` * FW version          : vX.Y.Z` line (pinned by a bench capture,
`tests/scripts/fixtures/provision/dxrt-cli-s.txt`); the RT / PCIe driver versions
printed beside it never match. A leftover `/boot/<fdtfile>.release` (an
interrupted run whose restore also failed) makes `boot_sd_linux` and `census`
refuse until it is copied back. Ledger facts: `dxm1_fw_version`, `dxm1_fw_md5`,
`dxm1_fw_uart_boot_md5` (md5 of the bundle files used) when the step is done or
already satisfied. `census` also reads `dxm1_pcie_device` and
`dxm1_fw_version` (read-only) on a V2M (`v2n-m1`) unit.

### Lock preconditions (`run --lock`)

All must hold: `secure_page` and `cold_boot_test` done for this bundle;
`<serial>.manifest.bin` committed and byte-equal to the array; the Secure
Data Page re-read equals `<serial>.secure-page.staged.bin`; Lock Status
bit 1 clear; the unit passes the ledger ship check (disposition `ship`, GD32
released on its own by the last cold boot, no known defects, no override,
not a `--build-dir` unit); and the operator retypes the serial. After the
lock frame, only a re-read with bit 1 set counts.

### Census keys (#2624)

- **I2C reads retry.** Every `i2cget` the tool makes (census, `pmic_verify`,
  `cold_boot_test`) is tried 3 times, 0.5 s apart (bench: `Error: Read failed` on
  reads that worked minutes later). A read that still fails is recorded as
  `unread (<error>)` for its census key (`act88760_gpio_regs`, `da9292_*`, `tps_vout`,
  `rtc_rv3028_reg_0x37`, `act88760_gpio4_otp`) and listed in the step detail; it never
  passes as a value and does not drop the group's other keys. `pmic_verify` reports it
  as `<dev> <addr> reg <reg> unread (<error>)` and fails.
- **Real PHY ID.** On these SoMs the devicetree forces `ethernet-phy-id001c.c878`, so
  sysfs shows `0x001cc878`; the silicon's MII registers 2/3 read `0x001c`/`0xc916`.
  `eth0_phy_id` / `eth1_phy_id` hold the sysfs value (`/sys/class/net/<if>/phydev/phy_id`),
  `eth0_phy_id_raw` / `eth1_phy_id_raw` the raw registers (e.g. `0x001cc916`), read by a
  small python3 `SIOCGMIIPHY` / `SIOCGMIIREG` helper pushed to `/tmp` and removed again
  (the image has python3 and no `mii-tool`). A port the helper cannot read is
  `unread (<if> errno <n>)` (a port that is down may answer `EINVAL`).
  `eth_phy_id_mismatch` is `yes` when a readable port's raw and sysfs IDs differ, `no` when
  both ports match, `unread` when a value could not be read and no readable port differs.
- **gbeth DMA reset.** `eth_dma_reset_failed` is `none`, `end0`, `end1` or `end0,end1`
  from `Failed to reset the dma` in `dmesg` (`unknown` when the line names no port).
- **Supply-current screen.** With SCPI power, `current()` sends one `MEAS:CURR? CH<n>`
  (the configured channel only, over the persistent socket) and census records
  `psu_current_a` (amps) and `psu_current_state` (`at-census`: one reading, taken when
  census runs, with the load not controlled). A reply that is not a finite number is
  `unread`. A reading above
  0.40 A adds `WARNING: supply current ... A > 0.40 A` to the step detail; the step still
  passes, because the threshold belongs in the private catalogue. Background: a capacitor
  on the PHY regulator output (a fault on one SoM revision) draws about 0.45 A as soon as
  the PHYs leave reset, against 0.17 to 0.29 A normal. Labgrid and manual power have no
  meter: the keys are absent. A failed query records `psu_current_a: unread (<error>)`.

These keys reach the unit ledger only if the private catalogue lists them as `auto`. An
`unread (...)` value never satisfies a `ship_required` key in the ship check.

Not yet confirmed by a run of this code on a bench: the MII values `0x001c` / `0xc916` (seen
on this SoM revision outside the tool), the 0.40 A threshold, the `MEAS:CURR?` reply format
and the sysfs `phydev/phy_id` path.

## Provisioning SD payload store

Without it, every payload crosses a wire: the GD32 images go base64 over the 115200-baud console (about 13 min on 2026W38-0002 when the board has no network), and the DX-M1 files, the DTBs and the 210 MB `wic.gz` go over SSH on a 100 Mbit switch. The board boots Linux from the provisioning SD for every write step, so the first unit caches each payload on the SD and every later unit reads it locally.

The store is a directory on the SD's root filesystem, `/var/lib/alp-payload/<bundle sha256>/`. It needs no SD preparation and no partition: the root has several GB free. It is used only when the board runs from the SD, never when its root is the eMMC, and it is on by default (`--no-payload-store` disables it). `plan` never touches it.

For each file `run --execute` looks in the directory before any push:

- hit and the hash computed ON THE BOARD (`sha256sum`, md5 when the image's busybox lacks it) equals the host's, which is itself checked against the signed bundle's sha256: the stored file is used in place (`cp` for small files, the stored `wic.gz` for the rootfs). The SD content is never trusted without that hash.
- miss or bad hash: the file is pushed as before and cached. Before caching, `df -k` must show free space of file size + 20 % + 64 MiB, else the file is only pushed and `payload_store_note` says why. A cached file is `sync`ed.

Steps: `gd32_flash` over the console (SWD tools and images; over SSH the bench's probe wrapper still copies from the host), `dxm1_npu_flash`, `write_rootfs`, `write_xspi`, `write_emmc_boot`. Each records `payload_source: sd-store` (every file came from the store) or `pushed`, plus `payload_store_note` when the store was off or a cache write failed.

`write_rootfs` with the stored wic.gz and a bmap: the board's `bmap_writer.py` decompresses the whole stream and skips each gap between ranges, writing only the mapped ranges; the range list (small, from the host's bmap) is still pushed, and the readback md5 is compared with the host's, unchanged.

**The provisioning SD is internal-only.** It carries license-gated DEEPX binaries (`dxm1_*`) once the store is filled, and is never shipped with a unit: it stays at the bench. Do not copy its image or hand it to a customer or a contract manufacturer.

## Board image requirements

What the tool runs on the provisioning image (busybox is enough; checked on a BusyBox v1.36.1 image):

- `python3`: `write_rootfs` runs `bmap_writer.py` with it (a board without it gets the full-image write), the partition-table re-read falls back to it when `blockdev` is missing, and `dxflash.py` runs on it.
- `gunzip`, `dd`, `md5sum`: the full-image write, and every md5 readback (`dd` feeds `md5sum`; only plain `dd` operands are used).
- `sha256sum` (md5 is the fallback), `cmp`, `cp`, `df`, `mv`, `sync`, `mkdir`, `rm`: the payload store.
- `mount`, `fsck.ext4`: the rootfs check after the write.
- `mmc` (mmc-utils), `flash_erase` and `mtd_debug` (mtd-utils), `i2cdetect` / `i2cget` / `i2cset` / `i2ctransfer` (i2c-tools), `ip`, `dmesg`: the eMMC boot config, the xSPI write, the census and the network checks.

Not needed: `sfdisk`, `partx`, `findfs`, `parted`, `blockdev`, `bmaptool`, `mke2fs`, and `dd iflag=fullblock`.

## Block-map write of the system image

The V2N image builds (`IMAGE_FSTYPES:append:rzv2n-family = " wic.bmap"`, `meta-alp-sdk/recipes-images/alp-image-common.inc`) emit `<image>.wic.bmap` next to the `.wic.gz`. A bundle that lists it as the optional `system_image_bmap` role (`flash_target` `emmc`, never flashed on its own) makes `write_rootfs` skip the unused blocks of the ~7 GB image:

1. The host gunzips the wic once and checks every mapped range against the bmap checksum (`sha256` or `sha1`). A mismatch refuses **before** anything is written.
2. The mapped bytes are gzipped into one stream and sent over a single ssh command that runs `python3 /tmp/alp-bmap-writer.py <emmc> <BlockSize> <ranges> 0` with the stream on stdin (a stored `wic.gz` is read from the SD instead, with the image size as the last argument). The writer and the range list are pushed as files first, so the command length does not depend on the range count. It `pwrite`s each range, ends with `fsync`, and exits non-zero on a short or truncated stream or an IO error. The board needs only `python3` (probed first; a board without it falls back to the full-image write). No `bmaptool`, no `dd iflag=fullblock`.
3. The same ranges are read back on the board and md5-compared with the host's md5 of the concatenation. The `write_rootfs` probe uses the same mapped-range md5, so a rerun skips a finished write.
4. The step evidence always records `rootfs_write_mode` (`bmap-python` or `full-image`), `rootfs_bytes_written` and `rootfs_image_bytes`, and `rootfs_bmap_fallback: <reason>` when the full image was written (no bmap in the bundle, or no python3 on the board).

Unmapped blocks are not touched: the eMMC outside the mapped ranges keeps whatever it held (a blank or previously provisioned part), and only the mapped ranges are verified. The last range is zero-padded to a whole block, so up to `BlockSize - 1` bytes past the image end are written. Without a `system_image_bmap` the full `gunzip | dd bs=4M` path and the full-span md5 are used, as before. `check_som_bundle.py` validates the bmap and compares its `ImageSize` with the gunzipped image when both files sit beside `bundle.json`.

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
- **`dxm1_npu_flash` swaps the release DTB while it runs.** The release DTB is backed up as `/boot/<fdtfile>.release` and restored (md5-verified) on success and on failure; if a run is killed, the next run restores it first. The BOOT_CFG straps (E1M `IO17`/`IO19`/`IO20` low) are a carrier property the tool cannot measure.

## Testing

Every module has pytest tests with fakes (`tests/scripts/provision_fakes.py`
and `tests/scripts/test_provision_*.py`); no hardware is needed.
