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
(`--pmic-expect`), optional overrides of the functional test's expected values
(`--functest-expect`) and the ledger itself (`--ledger-root`). Nothing in the
public tree names a bench, a unit serial or a memory/storage part number.
The bench runbook (switch positions, cabling, per-bench commands) lives in
the private repository.

## Fixed decisions

1. **No SD boot ROM path.** A plain microSD is not a boot source on this
   SoC. The flow is: SCIF download mode → the Flash Writer's `EM_W` puts a
   *transient* eMMC-boot BL2 (`bl2_mmc`) and the FIP into eMMC **boot partition 1** (Linux `mmcblk<N>boot0`, the one EXT_CSD`[179]=0x08` boots; Linux `boot1` is partition 2 and is never booted) →
   the unit boots U-Boot from eMMC → U-Boot boots the release wic from the
   microSD (U-Boot patch 0008 enables SDHI1 as `mmc1`) → **Linux performs
   every production write** (xSPI, eMMC boot partition 1, eMMC user area, EEPROM, GD32,
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
always runs). `--linux-host HOST` sets the target host for this run (it
overrides `bench.yaml` `linux.host`; an EEPROM MAC change still forces rediscovery). An
`--only` run that starts past `boot_sd_linux` has no console login to discover a host from, so it
needs `--linux-host` or `linux.host`. `--build-dir` builds an unsigned bundle from a deploy
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

First contact with no recorded CID (a run that starts past `bootstrap`, for
example `--from dsw1_emmc_insert_sd`) is proved over this unit's own console
before the first write: the SSH host must echo a nonce to its `/dev/console` and
the tool must read it on its serial line (once per host and power-on). A host
that does not is refused with "Nothing was written". On the normal blank-unit
path `bootstrap`'s `EM_DCID` records the CID, so every later step is checked by
CID instead.

`run --accept-cid-change REASON` is the escape for a legitimately replaced eMMC:
it adopts the CID seen over SSH as the new anchor. The `emmc_cid_change`
override (blocks shipping like the other overrides) is recorded only when a
recorded CID existed and differed, not on first contact.

## Operator flow: DSW1 and the microSD

The unit stays in **xSPI boot mode** (`SYS_LSI_MODE` `0x3c06`) for the whole run;
the boot switch is never moved to eMMC. U-Boot boots Linux from the microSD even
in xSPI mode, so each unit takes two legs:

1. **Leg 1, microSD in.** Insert the release microSD, then
   `--from dsw1_emmc_insert_sd --only dsw1_emmc_insert_sd,boot_sd_linux,gd32_flash,write_xspi,write_cm33,write_emmc_boot,write_rootfs,census,eeprom_manifest,dxm1_npu_flash,pmic_verify,secure_page`
   (drop the steps a bundle does not carry).
2. **Leg 2, microSD out.** Remove the card, then `--from dsw1_xspi_remove_sd`.

`SYS_LSI_MODE` `0x3c05` is the eMMC strap: wrong for leg 2, and `cold_boot_test`
refuses it.

Other operator rules:

- **Identify a unit before provisioning it.** Compare the EEPROM manifest
  (`i2c-0`, `0x50`, 128 bytes) with the recorded manifest, the eMMC CID with the
  recorded one, and the U-Boot line `ALP: MAC ... (serial ...)` with the serial.
  Never let the tool allocate a serial for a unit that already has one.
- **Cable `end0`.** The tool discovers the Linux host over `end0`; a gigabit link
  also shortens the rootfs transfer.
- **Pace SCPI queries.** A burst of queries wedged the SPD3303X LAN socket
  (recovery: power-cycle the supply). The tool keeps one connection and spaces
  commands by at least `bench.yaml` `power.min_gap_s` (default `0.3` s), retries
  after a connection reset with backoff, and only ever addresses the configured
  channel. Keep any manual queries just as sparse. In `bench.yaml`
  (`min_gap_s` is optional): `power: {kind: scpi, host: PSU_HOST, port: 5025, channel: 1, min_gap_s: 0.3}`
- **Prompt resync.** `cold_boot_test` tolerates a kernel message printed after the
  shell prompt: the prompt wait sends a newline so a fresh prompt appears.

## Package `scripts/provision/`

| module | responsibility |
|---|---|
| `bench.py` | Console (serial / TCP), Power (SCPI / labgrid / manual), Probe (J-Link / wrapper script), Operator prompts, `load_bench()`; the one `expect()` loop |
| `uboot.py` | U-Boot / BL2 console interaction, banner parsing, XMODEM `loadx` fallback |
| `scif_writer.py` | boot-ROM SCIF download, Flash Writer `EM_W` / `EM_SECSD` / `EM_DCID`, bin → S-record |
| `linux_target.py` | SSH runner, console login, MTD / eMMC / I2C helpers, the read-only census |
| `functest.py`, `functest-expect-v2n.yaml` | the functional test: the check catalogue, the one-script runner and the judges; its expected values |
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
   `sync; echo ALPSYNC:$?; poweroff`. On a mismatch no `poweroff` is sent, but the
   `echo ALPID<nonce> > /dev/console` line has already been printed on the other unit's
   console (harmless, yet visible to anyone running parallel stations).
3. A console whose state is unknown (no login of this tool on this power-on, no proven
   SSH host): Ctrl-C only, read 2 s. A `=> ` prompt at the end of the text means U-Boot,
   no shutdown needed (a `=> ` inside a log line does not count). Otherwise a console
   login (5 s), which starts with an Enter; on a shell, path 1. No Enter is sent until a
   U-Boot prompt has been ruled out for 2 s after the Ctrl-C (U-Boot would repeat its last
   command). A unit known to sit in the SCIF ROM or the Flash Writer
   gets nothing sent: this tool's parsers send those only CR-terminated lines and never
   `0x03`, so Ctrl-C there is untested.

Then it waits up to 120 s for the console halt line (`reboot: Power down` or
`System halted`), plus 5 s more before it gives up. It is done once per power-on.
The PSU current is not a halt signal. The OFF dwell is unchanged, so a clean poweroff
followed by it is still a cold boot, and `cold_boot_test` keeps its `--cold-cycles` (3).

The "already halted on this power-on" marker survives the operator prompt of
`dsw1_emmc_insert_sd` or `dsw1_xspi_remove_sd`: the tool saw the halt line itself, so a cut
after the prompt is not logged as `blind`. It is cleared only if the console printed
something after the halt (a halted unit is silent, so boot text means it was powered back
on). Both prompts say "Leave it OFF until the tool asks."

Every cut is recorded as the step evidence key `power_cut` (and in the step log and the
plan log), one entry per cut:

| value | meaning |
|---|---|
| `clean` | halted: `poweroff` sent, the halt line seen (`(late)` if it came in the extra 5 s) **and the first `sync` returned 0** |
| `clean (sync rc=<n>)` | halted, but the first `sync` returned `<n>` (non-zero, or unknown): the operator prompt says so and the card should be checked |
| `fallback` | no halt line. Over SSH a second `sync` ran and its rc (or error) is recorded; on the console no second command is sent (the first may still be running, and a new command would Ctrl-C it) |
| `unreadable (sync rc=<n>; <lines>)` | the `sync` returned rc `127` (not found), or the console showed `EXT4-fs error` / `error -5` lines: the root is treated as unreadable and those lines are quoted verbatim in the entry, the step failure and the operator prompt |
| `blind` | no console shell and no SSH path proven to be this unit. If Linux is up the cut is hard; the entry says so |
| `not-needed` | PSU already off, U-Boot prompt, SCIF ROM / Flash Writer, or already halted (plain `clean`) on this power-on |

A later cut on the same power-on repeats an earlier non-clean outcome (`fallback`,
`clean (sync rc=<n>)`) as "earlier cut on this power-on was <outcome>" instead of reporting
`not-needed`. A `blind` cut sets no marker, so the next cut tries again.

Not covered: a kernel that is still booting (no shell yet, so the probe finds nothing and
the cut is `blind`), a console shell that does not answer, a unit that is hung, and the
manual or labgrid power kinds (their prompts and commands are still preceded by this
shutdown, but nobody checks what the operator does). The `reboot: Power down` line was
seen in bench console logs of this image on 2026-10-02 (from earlier poweroff code, not from
`clean_shutdown`); the halt-line text as matched here, the 120 s bound and all of the above
are not yet confirmed by a run of this code on a bench.

## Steps

| step | what it does |
|---|---|
| `preflight` | offline gates: the `functional_test` block of `bench.yaml` and its expected values, bundle schema, artefact sha256 / size / role set (`bl2`, `bl2_mmc`, `fip`, `system_image` -- enforced here, not by the bundle schema, because flat-flow bundles such as som-0.2.0 legitimately carry no `bl2_mmc`), xSPI and eMMC boot partition size limits, family, SKU triangle, DDR tier triangle, FIP rail string (`v2n-m1`), FDT present in the wic `/boot`, N24S128 frame-table self-check |
| `detect` | clean shutdown if Linux is up, then power cycle and classify the console: SCIF ROM, BL2, U-Boot, Linux login, silent. The ROM banner `SCI Download mode (Due to parameter error)` is the ROM's fallback to SCI download, taken when the selected boot source has no valid image (blank xSPI/eMMC, or DSW1 not in mode 3). It still offers `-- Load Program to SRAM`, but it is **refused**: "boot ROM fell back to SCI download because the selected boot source has no valid image (blank xSPI/eMMC, or DSW1 not in mode 3); set DSW1 to mode 3 for a clean SCIF bootstrap". Only `(Normal SCI boot)` proceeds |
| `dsw1_scif` | operator: boot switch to SCIF download |
| `bootstrap` | reuses the live SCIF ROM state `detect` left in this run (no second power cycle); cycles only if no ON has happened since. Flash Writer: `EM_W` area 1 (boot partition 1) sector `0x1` ← `bl2_mmc`, sector `0x300` ← `fip`; `EM_SECSD` EXT_CSD `[177]=0x02` (BOOT_BUS_CONDITIONS), `[179]=0x08` (PARTITION_CONFIG); `EM_DCID` |
| `dsw1_emmc_insert_sd` | operator: insert the provisioning microSD (DSW1 may stay on xSPI); U-Boot must autoboot (see "Operator flow: DSW1 and the microSD"). The result names the boot mode BL2 reported (`xSPI` for `SYS_LSI_MODE` `0x3c06`, else the observed value, or "boot mode not reported by BL2") |
| `boot_sd_linux` | U-Boot boots the wic from microSD; log in, find the host, confirm the root is on the SD, then the live SoM check: every non-optional on-module I2C device the SoM preset declares must ACK (the GD32 excepted) before any destructive step runs. Fallback `--transfer xmodem`: `loadx` + `gzwrite` the wic from the U-Boot prompt. No IP is not a failure here: the checks run over the console and `gd32_flash` runs next |
| `gd32_flash` | applies the ACT88760 GPIO4 volatile release if still at the OTP default, DP-ID gate (`0x0BE12477` only), `loadbin` × 3, verify with `savebin` in fresh probe sessions, reset-and-run after every readback (see "The GD32 readback halts the MCU"), then `GET_VERSION` from the bridge at `0x70` (polled; the step fails if it never answers). With no network (the `boot_sd_linux` evidence says why: `network: none (gbeth DMA reset failed on <ports>)` or `none (no carrier)`) it pushes the SWD tools and the three images over the console (base64, md5-checked on the board), then cold-cycles and re-checks the IP; SSH is used when it is up |
| `write_xspi` | from Linux: `bl2` → `mtd0`, `fip` → `mtd1`; md5 readback. A FIP whose erase would reach the CM33 image at `mtd1` + `0x1A0000` is refused |
| `write_cm33` | from Linux: the bundle's `cm33` image → `mtd1` at `0x1A0000`; md5 readback of exactly the image size; probe = the same md5. Skipped (`bundle has no cm33 component`) when the bundle carries none. See "CM33 image" |
| `write_emmc_boot` | release `bl2_mmc` + `fip` into `mmcblk<N>boot0` (boot partition 1, the one EXT_CSD`[179]=0x08` boots; `boot1` is never booted), md5 readback of the same device, EXT_CSD via mmc-utils |
| `write_rootfs` | stream the wic into the eMMC user area (refused while Linux runs from the eMMC), `fsck -n`, read-only mount (`-o ro,noload`, so the ext4 journal is never replayed), `/boot/<dtb>` present. With a `system_image_bmap` in the bundle only the mapped ranges are written and verified, see below |
| `census` | read-only (one throwaway file in `/tmp`): every auto ledger key the unit can provide, plus the keys listed under "Census keys" below |
| `eeprom_manifest` | preconditions, 128-byte manifest in 8 × 16-byte page writes at `0x50`, readback, cold cycle, re-read; only then the staged blob is promoted to `<serial>.manifest.bin` |
| `dxm1_npu_flash` | `v2n-m1` bundles that carry the DX-M1 set: firmware over the ROM UART path, probe = PCIe `0x0000` + `dxrt-cli -s` version, see below |
| `pmic_verify` | compare registers against `--pmic-expect` |
| `secure_page` | write the 64-byte Secure Data Page, read back, compare. Never locks |
| `dsw1_xspi_remove_sd` | operator: boot switch in xSPI mode, remove the microSD |
| `cold_boot_test` | `--cold-cycles N`: clean BL2, DRAM tier, rail line (`v2n-m1`), login, `SYS_LSI_MODE`, ACT88760 reg `0x10` after boot, I2C scans (GD32 exempted only while `0x10` still reads `0x88`). An `end0` without carrier (PHY latch, #2582) gets one extra cold cycle, noted as `end0_no_carrier_retries` in the step evidence (not a ledger key); it fails if `end0` is still down. With the `rtc_backup` fixture and N >= 2 it also sets the RTC from the host clock after the first cycle, so `functional_test` can judge `rtc_retention` |
| `census_final` | the census again, on the unit as shipped (after the last cold boot: xSPI boot, eMMC root). Every key it reads replaces the first census's value. A key the first census recorded and this one could **not** produce (a census group that fails drops its keys) is set to `unread (not re-read by census_final)`, so a value read in the microSD boot never stands in for the shipping boot; an unread ship-required key blocks the ship check. The ACT88760 GPIO4 keys are `cold_boot_test`'s and are left alone |
| `clkgen_verify` | the on-SoM 5L35023B (`BRD_I2C`, `0x69`) OTP image against U-Boot's fixup |
| `functional_test` | every interface the tool can reach, on the unit as shipped: one generated script, one remote invocation, one `test_ft_<check>` value per check; a failing or unreadable check fails the step unless it is listed informational, and only `test_functional: pass` ships. See "Functional test coverage" |
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

A run that captured no cold-boot console (`--only` / resumed runs) cannot read the U-Boot
fixup line. The step then reports `skipped` with "not verified: no boot console captured in
this run", records `clkgen_uboot_fixup: unread (...)` in its evidence (a captured run
records `seen: <line>`), and does not fail; an OTP image mismatch still fails.
`need_linux` on such a run, with no `linux.host` pinned, logs in on the console and reads the
address like `boot_sd_linux` does, once per power-on; it first sends Ctrl-C only and sends
no Enter to a unit at a U-Boot prompt, in the SCIF ROM / Flash Writer, or halted.

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
`clkgen_otp_raw` (all 37 bytes as read, hex), `clkgen_i2c_addr`,
`clkgen_uboot_fixup` (`seen: <the boot-log line>`, or `unread (...)` when no
boot console was captured).

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

### The GD32 readback halts the MCU

`gd32_flash` reads the three image regions back over SWD (`savebin`) in its probe (before the
step runs, and again after it, also for a unit whose GD32 is already programmed, and in a
`plan` with `--bench`) and in its verify. The dump halts the GD32 core and leaves it halted. A
halted GD32 still acknowledges its I2C address `0x70` and stretches SCL, and the kernel's
bridge driver retries about once a second, so the board-management bus (`BRD_I2C`, `i2c-8` on
the reference bench) is dead until the next power cycle: the kernel log repeats
`gpio-gd32-bridge 8-0070: output state replay failed (-110)` and
`i2c i2c-8: SCL is stuck low, exit recovery`, and every later read on that bus fails
(`Error: Read failed`). The tool therefore resets and runs the core after every readback and
after a failed write or verify (in a `finally`, so also when the dump itself fails), then asks
the bridge for `GET_VERSION` (10 tries, 1 s apart). The result is recorded as
`gd32_bridge_after_readback` (probe) and `gd32_protocol` (run). If the bridge does not answer,
the step fails and the run stops before `census`. A failed reset never hides the error it
followed: it is appended to it ("... ALSO the reset-and-run failed, GD32 core left halted: ..."),
and when it happens in the probe before the step, the step itself fails with "core left halted
by the pre-run probe's readback" and writes nothing. **`SCL is stuck low` on that bus during provisioning points at a GD32 left halted over
SWD**; a census that finds unread keys says so when the kernel log carries that line.
`census_final` reads the same keys again after the final cold boot. A wedge left by the
probe's own readback clears on the next power cycle; the tool does not skip the readback on a
ledger match, because nothing ties that match to the bytes on this unit's chip. That the halt is the cause
is inferred from the probe tool's behaviour and from log timing on two units; it has not been
reproduced on a bench, and neither has this fix.

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
  Both RTL8211F (`0x001cc916`) and RTL8211F-VD (`0x001cc878`) ship on these SoMs: sysfs shows the DT-forced ID, MDIO shows the real one, and either is valid.
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

## Functional test coverage

`functional_test` tests every interface the tool can reach on the unit **as shipped**: it runs
after `cold_boot_test`'s last cold boot (xSPI boot, eMMC root), after `census_final` and
`clkgen_verify`. It is a step of its own, not part of `hil_smoke`: `hil_smoke` shells out to the
HiL runner (built example binaries, one pass/fail for a whole spec directory, nothing recorded
per check), while `functional_test` runs through the tool's own Linux target and records one
value per check. Where a HiL spec in `tests/hil/v2m103-x-evk/` already had a working command,
the check uses that command.

**None of this has run on a bench yet.** Every command output format, band and time below is
either taken from a real provisioning log (marked *measured*) or assumed (see "Not yet
confirmed on a bench").

### How it runs

1. The supply current and voltage are read first, while the unit is idle (SCPI power only).
2. One shell script is generated from the catalogue, pushed to `/tmp`, run in **one** remote
   invocation and removed. It works in a fresh `mktemp -d` directory, changes into it before
   anything else runs (no tool can drop a file in the login directory on the root filesystem)
   and removes it on any exit. Checks are grouped in lanes: the `main` lane runs in the
   foreground, every other lane (eMMC read, network, radio, NPU, RTC, secure element, USB/SD,
   audio, loopbacks) in the background. Each check is a file of its own, run in its own session
   (`setsid`), writes to its own file and has its own timeout; results are printed at the end in
   catalogue order, so nothing interleaves. A check that overruns gets TERM, so its restore
   lines run, then its whole process group is killed (the `dd`, `aplay`, scan or model run it
   started included) and it is recorded `unread (timed out after N s)`. On an image without
   `setsid` only the check's own shell and its restore are covered; a child may run on.
3. The host judges each output against the expected values.

Each check is recorded as `test_ft_<check>` (a prefix of its own: the ledger's `test_<name>`
keys are also entered by operators, and the tool never writes one of those) with one of:

| value | meaning |
|---|---|
| `pass` / `pass (<measured>)` | the criterion holds; the measured value rides along |
| `fail (<reason>)` | the check ran and the unit does not meet the criterion |
| `skipped (<reason>)` | no fixture (`no fixture: <name>`); never a pass |
| `unread (<reason>)` | the check could not be evaluated: timed out, `missing tool: <name>`, a read error, output in an unexpected shape, or `no expected value: <key>` (nobody defined the criterion) |

Reasons are collapsed to one line (the census rule) and cut at 200 characters. A `fail` or
`unread` of any check not listed `informational` fails the step, stops the run and sets the
summary key `test_functional: fail (<checks>)`; `record` still runs. A `skipped` check and a
failing informational check never block, but both are in the unit record and in the step detail
(a missing expected value of an informational check is `skipped`).

**The ship check ships only `test_functional: pass`.** A missing key (the step never ran or was
skipped with `--skip`), `fail (...)`, `unread (...)` and anything else block, each with its
reason. A run that cannot reach the unit, or a bad configuration, records
`test_functional: unread (<error>)`. `record` writes the verdict of the step's **latest** run: a
later failed or interrupted `--only functional_test` replaces an earlier `pass` in the unit record
(with its recorded verdict, or `unread (functional_test <status> in its latest run)`), also
when `record` runs alone afterwards. **Units provisioned before this step existed have no
`test_functional` and are not shippable until the functional test has run on them. That is
intended:** run `--from cold_boot_test` (or `--only functional_test,record` on a unit that is
already booted from its eMMC) on each.

A missing tool never passes and never fails: every check names the tools it needs, and the
first one missing makes it `unread (missing tool: <name>)`; a command that ends with exit status
127, or whose last line is the shell's `not found`, is read the same way.

Nothing in the test writes a device register, an EEPROM, OTP, flash or the eMMC, binds or
unbinds a driver, or locks anything. I2C traffic is register-pointer writes followed by reads
(`-f` where a kernel driver owns the address), the identity page is read with the one sealed
frame, and the bridge gets its read-only `GET_VERSION`. No transfer is ever generated to an
address that is another device's shared (broadcast) address on that bus: on this carrier `0x48`
on the sensor bus, the amplifiers' shared address, read from the amplifier's chip description;
no expected-values file can change that, and a carrier's `not_fitted` list is the union of the
public and the private file. Every string that comes from `bench.yaml` or the expected values
into the script is shell-quoted.

Transient state is put back by a trap that also runs when the check is killed: `wlan0` and
`hci0` go down again if they were down, and each CAN link gets its previous state back (down,
its previous bitrate if it had one, up again only if it was up). The eMMC read drops the page
cache first, as the write steps' readbacks do. One exception to "writes nothing", off by
default: with the `rtc_backup` fixture `cold_boot_test` sets the RTC's time from the host clock
after its first cycle (see `rtc_retention`).

The Bluetooth scan uses `hcitool lescan` on the raw HCI socket, not `bluetoothctl`: it needs no
`bluetoothd`, so nothing is cached under `/var/lib/bluetooth` on the read-write root (factory
neighbours must not ship on the unit) and no device of an earlier scan is listed; its output
goes to the script's temp directory. **Whether `hcitool` is on the image, and whether a raw LE
scan works while `bluetoothd` runs, is unverified.**

### Expected values

All pass criteria live in `scripts/provision/functest-expect-v2n.yaml`; the code holds the
commands only. `--functest-expect FILE` (private, like `--pmic-expect`) is merged over it key by
key, e.g. to pin the xSPI JEDEC ID, the remaining regulator output codes, a tighter band, or to
move a check in or out of `informational`. Values that already have a home are read there and
not repeated: DRAM and eMMC size and the I2C addresses from the SoM preset, the CPU count from
the SoC description, the carrier's devices and shunt resistors from `metadata/boards/<carrier>.yaml`,
the DX-M1 firmware version from the bundle, the PMIC registers from `--pmic-expect`, the MACs
from the serial.

An expected value that is missing or null makes its check `unread (no expected value: <key>)`,
which blocks (`skipped` for an informational check): a criterion nobody defined is not a pass.

Provisional bands (no population data yet; the first bench runs must confirm or tighten them):
`mem_total_fraction` 0.70..1.00 of the SKU DRAM (one unit: 0.771), `emmc_size_fraction`
0.85..1.00 (one unit: 0.911), `emmc_read_min_mib_s` 20, `board_temp_c` 10..85 degC,
`soc_temp_c` 10..105 degC (the HiL spec's band), `supply_power_idle_w` 3.30..4.95 W for
`v2n-m1` (volts x amps as the supply measures them, so it holds at any supply voltage; derived
from 0.26..0.29 A measured at 15 V, with 0.04 A added each side; **no band for `v2n` yet, so
that check is `unread` there and blocks until one is measured**), carrier rails nominal +-5 %, rail currents 0..the monitor's full scale,
`rtc_max_error_s` 30, the audio playback time 1.8..2.5 s.

### Fixtures: `bench.yaml`

```yaml
functional_test:
  carrier: e1m-x-evk            # metadata/boards/<name>.yaml; absent = carrier checks skipped
  fixtures:                     # everything here is off unless set
    eth1_cable: true            # second Ethernet port cabled to the bench network
    wifi_ap: {ssid: <name>, min_signal_dbm: -70}    # or `true`: any network counts
    ble_advertiser: {address: <AA:BB:..>}           # or `true`: any LE advertiser counts
    dxm1_model: /path/on/the/unit/model.dxnn        # a compiled model present on the unit
    usb_stick: true             # a USB mass-storage device in the host port
    sd_card: true               # a NON-bootable card in the SD slot (a bootable one boots instead of the eMMC)
    rtc_backup: true            # a backup supply for the RTC; needs --cold-cycles >= 2
    ina228_rework: true         # this carrier has the input-monitor rework
    camera: <regex>             # a camera is fitted; the regex names its sensor driver
    can_loopback: true          # CAN0 wired to CAN1, terminated
    uart_loopback: /dev/ttySCn  # header UART, TX wired to RX
```

A fixture that is off records `skipped (no fixture: <name>)` and its command is not even in the
script. The supply check needs no switch: it runs when the bench's power kind can measure
(`scpi`: `MEAS:CURR?` and `MEAS:VOLT?` on the configured channel only), else it is
`skipped (no fixture: a supply that measures current ...)`. A current without a readable voltage
is `unread`. `preflight` validates the whole block (carrier name, fixtures mapping, the bus
numbers it needs), so a bad `bench.yaml` fails before anything is flashed.

The carrier's I2C bus number is not assumed: the carrier description names the E1M bus, and the
bench's `i2c_bus` table gives its Linux number (a null `i2c_bus.eeprom` is refused).

### Coverage matrix

Strength: **answers** = the device ACKs or its driver is bound; **ID** = an identity register
or version was read and compared; **value** = a measured value is compared with a band or an
expected value; **data** = a data path was exercised end to end.

| function | before this step existed | now | needs |
|---|---|---|---|
| **SoM** | | | |
| DRAM size and tier | `boot_sd_linux`, `cold_boot_test`: DDR tier triangle (value); census records `MemTotal` | + `mem_total` (value) | - |
| CPU | census records the max frequency | `cpu_count` (value) | - |
| image identity | census records kernel and DTB | `boot_source`, `kernel_release`, `sku` (value) | - |
| eMMC size, bus mode | census records both | `emmc_size`, `emmc_mode` (value) | - |
| eMMC health | not tested | `emmc_health` (value) | - |
| eMMC data path | `write_rootfs`, `write_emmc_boot`: md5 readback (data) | + `emmc_read`: 64 MiB at speed in the shipping boot (data) | - |
| xSPI flash | `write_xspi` md5 readback, census md5 (data) | + `xspi` (ID, partitions); `census_final` repeats the md5 | - |
| Ethernet PHYs | census records the MII IDs | `eth_phy_id` (ID), `eth_mac` (value: derived from the serial) | - |
| Ethernet port 0 | SSH runs over it; `cold_boot_test` needs carrier (data) | `eth0_link`: speed, duplex, ping bound to the port (data) | the bench network |
| Ethernet port 1 | census records the link state | PHY ID and MAC always; `eth1_link` (data) | `eth1_cable` |
| Wi-Fi module | not tested | `wifi_present`: SDIO function, firmware, `wlan0` (answers) | - |
| Wi-Fi radio | not tested | `wifi_scan` (data: receive path) | `wifi_ap` |
| Wi-Fi regulatory domain | not tested | `wifi_regdomain` (informational) | - |
| Bluetooth controller | not tested | `bt_hci`: registers, powers up, has an address (answers) | - |
| Bluetooth radio | not tested | `bt_scan` (data: receive path) | `ble_advertiser` |
| PCIe + DX-M1 | `dxm1_npu_flash`, census: device ID, firmware version (ID) | `dxm1_pcie`: device ID, driver, link width; `dxm1_runtime`: device node, service, firmware version (ID) | - |
| DX-M1 inference | not tested | `dxm1_inference` (data) | `dxm1_model` |
| DRP-AI | not tested | `drpai`: driver bound, device node (answers) | - |
| GPU | not tested | `gpu`: driver bound, device node (answers) | - |
| RTC | I2C scan (answers); census records one register | `rtc_device`, `rtc_ticks` (value: the seconds register advances), `rtc_time_set`, `rtc_backup_mode` (informational) | - |
| RTC keeps time | not tested | `rtc_retention` (data) | `rtc_backup` |
| main PMIC | `pmic_verify` (value, in the microSD boot); `cold_boot_test` reads reg `0x10` | `pmic_registers`: the same compare after a plain cold boot (value) | - |
| second PMIC | `pmic_verify`, census (ID) | `i2c_da9292_1e` (ID `0xEA`) + `pmic_registers` | - |
| NPU rail regulators | census records presence and output codes | `i2c_tps628640_<addr>` (answers; output code where pinned) | - |
| temperature sensor | I2C scan (answers) | `board_temp` (value) | - |
| clock generator | `clkgen_verify`: full OTP image (data) | unchanged | - |
| secure element | I2C scan with a wake retry (answers) | `secure_element`: protocol-level state read (ID) | - |
| GD32 bridge | `gd32_flash`: md5 readback, `GET_VERSION` (data) | `gd32_bridge`: protocol version with CRC; `gd32_gpiochip` (ID) | - |
| SoC thermal zones | not tested | `thermal` (value) | - |
| CM33 | not tested | `cm33_firmware` (informational: not blank; **blocking with an md5 compare once the bundle carries a `cm33` component**), `cm33_running` (the beacon counter advances; informational, **blocking once the bundle carries a `cm33` component**), `openamp_uio` (answers) | - |
| USB host | not tested | `usb_host`: controllers probed (answers); `usb_device` (data) | `usb_stick` |
| SD slot | `boot_sd_linux` runs Linux from it (data) | `sd_host` (answers); `sd_card` (data) | `sd_card` |
| supply power | census: one current reading, warns above 0.40 A | `supply_power_idle` (value: volts x amps) | a supply that measures |
| kernel log | census: gbeth DMA reset only | `dmesg_fatal` (known fault signatures), `dmesg_clean` (allowlist, informational) | - |
| services | not tested | `systemd_failed` | - |
| EEPROM manifest | `eeprom_manifest`: write, readback, cold cycle (data) | `eeprom_manifest`: re-read in the shipping boot (data) | - |
| secure page | `secure_page`: write, readback (data) | `secure_page`: re-read in the shipping boot (data) | - |
| **Carrier (the fixture: SoM pins that reach it)** | | | |
| the two IMUs, the barometer | not tested | `carrier_<part>_<addr>` (ID) | `carrier` |
| rail monitors | not tested | `carrier_ina236_<addr>` (ID), `rail_3v3`, `rail_1v8` (value) | `carrier` |
| input monitor | not tested | `carrier_ina228_42` (ID), `rail_5v` (value) | `ina228_rework` |
| I/O expanders | not tested | `carrier_tcal9538_<addr>` (answers) | `carrier` |
| audio amplifiers | not tested | `audio`: both bound, a 2 s playback of silence in real time (data up to the amplifier input) | `carrier` |
| display | not tested | `display_dsi`: connector registered (answers) | `carrier` |
| camera | not tested | `camera`: the sensor's driver probed (ID) | `camera` |
| CAN | not tested | `can_loopback` (data) | `can_loopback` |
| header UART | the console UART carries the whole flow (data) | `uart_loopback` (data) | `uart_loopback` |
| push button | not tested | `gpio_keys`: input device registered (answers) | `carrier` |

The carrier's camera-rail monitor at `0x48` is not fitted and that address is the amplifiers'
shared address, so nothing is ever sent there; the identity EEPROM at `0x50` is the SoM's and is covered
by `eeprom_manifest`.

### Not covered, and why

- **A DRAM pattern test.** Minutes for 4 GiB; the tier triangle and `mem_total` only prove size
  and configuration.
- **eMMC and xSPI writes in the shipping boot.** The write path is proven by the md5 readbacks
  of the write steps; the test itself writes nothing.
- **Wi-Fi / Bluetooth transmit, association, throughput.** A scan proves receive only.
- **DRP-AI inference and GPU rendering.** The image carries no model and no headless render
  test; only driver and device node are checked.
- **CM33 RPC echo.** `cm33_running` only reads the stock image's liveness beacon (see "CM33 image"); without a
  `cm33` component in the bundle the flow programs no CM33 image (so `cm33_firmware` reports a blank region), the echo needs an example binary that is not in
  the image, and the HiL spec says one attach per CM33 boot.
- **Secure-element cryptography.** Needs the host-library example binary.
- **Supply power under load.** One figure exists (0.42..0.44 A at 15 V, measured on the bench on
  2026-10-02 with the DX-M1 running inference), but the host would have to sample the supply
  while the unit runs the model; not built.
- **Anything a person has to see or hear:** a picture on the display, sound from the speakers,
  LED colours, a button press, the encoder. `display_dsi`, `audio` and `gpio_keys` stop at what
  software can observe.
- **The GD32's own peripherals** (PWM, ADC, DAC, encoder inputs on the carrier headers), the
  microphones, the M.2 sockets, touch. No loopback exists for them on the bare carrier.
- **The RTC keeping time** unless the `rtc_backup` fixture is on. Note `rtc_backup_mode`: a unit
  read on the bench has backup switchover disabled in the RTC's configuration, so retention
  would fail even with a backup supply until something enables it.

### What an RP2040-based fixture would unlock

Boot-mode switching (removes the three operator steps and allows a full unattended run), SD
card insertion and the SD mux (`sd_card` without a person, and the SD steps), the console, power
switching and current measurement on a bench without an SCPI supply (`supply_power_idle`),
the header UART loopback (`uart_loopback`), a CAN node with a transceiver (`can_loopback`),
reading the LED lines and driving the button and encoder lines (the operator-observed rows),
and loopbacks for the GD32's PWM / ADC / DAC pins. It does not help the radio, camera, display
or audio rows.

### Checks

Time is an **estimate** of the typical run time on the unit, not a measurement. The automatic
set is estimated at about 7 s on the unit (the `main` lane) plus the push and one SSH round
trip; with every fixture on, about 12 s (the radio lane). `census_final` before it is roughly
60 SSH round trips, estimated 20 to 30 s. Both are far inside the 90 s target; the timeouts are
bounds for a hung command, not the expected time.

| check | runs on the unit | pass criterion | timeout | est. |
|---|---|---|---|---|
| `boot_source` | root device, eMMC by sysfs type | the root is on the eMMC | 5 s | 0.1 s |
| `cpu_count` | `grep -c ^processor /proc/cpuinfo` | = the SoC's application-core count | 5 s | 0.1 s |
| `mem_total` | `MemTotal` of `/proc/meminfo` | 0.70..1.00 of the SKU DRAM | 5 s | 0.1 s |
| `kernel_release` | `uname -r` | matches `^6\.1\.141-cip43` | 5 s | 0.1 s |
| `sku` | `/proc/device-tree/chosen/alp,sku` | = the SKU being provisioned | 5 s | 0.1 s |
| `emmc_size` | `/sys/block/{emmc}/size` | 0.85..1.00 of the SKU eMMC size | 5 s | 0.1 s |
| `emmc_health` | `mmc extcsd read` | life time A and B <= `0x01`, pre-EOL = `0x01` | 10 s | 0.3 s |
| `emmc_mode` | mmc `ios` in debugfs, `dmesg` | timing `mmc HS200`, no `mmc_select_hs200 failed` | 5 s | 0.1 s |
| `emmc_read` | drop the page cache, then `dd` 64 MiB from 1 GiB into the user area | `64+0 records out`, >= 20 MiB/s | 20 s | 2 s |
| `xspi` | `/proc/mtd`, spi-nor `jedec_id` | `mtd0` and `mtd1` with a size, a real JEDEC ID | 5 s | 0.1 s |
| `eth_phy_id` | MII registers 2/3 of both ports (python3 ioctl helper) | both `0x001cc916`; a list is accepted (`eth_phy_id: [0x001cc916, 0x001cc878]` in `--functest-expect`: RTL8211F and RTL8211F-VD) | 5 s | 0.3 s |
| `eth_mac` | `/sys/class/net/{if}/address` | both = the MACs derived from the serial | 5 s | 0.1 s |
| `eth0_link`, `eth1_link` | carrier, speed, duplex, `ping -c 2 -I {if}` the gateway, RX counter | carrier, 100 or 1000 Mbit/s, full duplex, ping ok, >= 2 packets received on that port | 10 s | 1.5 s |
| `wifi_present` | `wlan0`, `/sys/bus/sdio/devices`, `dmesg` | all present, firmware banner, no firmware failure | 5 s | 0.1 s |
| `wifi_regdomain` | `iw reg get` | a country line (informational) | 5 s | 0.1 s |
| `wifi_scan` | `iw dev wlan0 scan` | >= 1 network; the reference AP at >= the configured signal | 20 s | 5 s |
| `bt_hci` | `hci0`, `hciconfig hci0 up` | registered, `UP RUNNING`, a non-zero address (`hciconfig` missing: `unread`) | 12 s | 1 s |
| `bt_scan` | `hcitool -i hci0 lescan` for 5 s | >= 1 address; the reference advertiser | 20 s | 6 s |
| `dxm1_pcie` | `/sys/bus/pci/devices/0000:01:00.0` | device `0x0000`, driver `dx_dma_pcie`, link width 2, link speed `8.0 GT/s` (both measured: the kernel logs `8.0 GT/s PCIe x2 link`); an unreadable width or speed is `unread` | 5 s | 0.1 s |
| `dxm1_runtime` | `/dev/dxrt0`, `dxrt.service`, `dxrt-cli -s` | node, service active, firmware = the bundle's | 20 s | 2 s |
| `dxm1_inference` | `run_model -m {model} -l 30` | exit code 0 and an FPS line | 40 s | 8 s |
| `drpai`, `gpu` | platform driver link, device node | driver bound, node exists | 5 s | 0.1 s |
| `rtc_device` | `/sys/class/rtc/rtc0/name` | names the RV-3028 | 5 s | 0.1 s |
| `rtc_ticks` | the seconds register twice, 2 s apart | valid BCD, advanced by 1..4 s | 8 s | 2.2 s |
| `rtc_time_set` | `rtc0/since_epoch` | readable (informational) | 5 s | 0.1 s |
| `rtc_backup_mode` | register `0x37` | backup switchover enabled (informational) | 5 s | 0.1 s |
| `rtc_retention` | `rtc0/since_epoch`, boot id, `dmesg` | set by `cold_boot_test` on an earlier boot, no `hctosys: unable to read the hardware clock` in this boot's log, within 30 s of the host clock | 5 s | 0.1 s |
| `board_temp` | TMP112 temperature register | 10..85 degC | 5 s | 0.1 s |
| `secure_element` | the state-register read, up to 100 tries | an answer | 12 s | 0.5 s |
| `gd32_bridge` | `GET_VERSION` frame | status 0, CRC good, protocol `0.14.0` | 5 s | 0.1 s |
| `gd32_gpiochip` | gpiochip `gd32-bridge-gpio` | >= 20 lines | 5 s | 0.1 s |
| `i2c_{chip}_{addr}` | one register read per on-module device | the ID where one is pinned (`0xEA` for the second PMIC), else an answer | 5 s | 0.1 s |
| `pmic_registers` | every register of `--pmic-expect` | all equal under their masks | 15 s | 0.6 s |
| `thermal` | every `thermal_zone*/temp` | >= 1 zone, all 10..105 degC | 5 s | 0.1 s |
| `cm33_firmware` | md5 at `mtd1` + `0x1A0000` | no `cm33` in the bundle: 64 KiB not blank (informational). With one: md5 of exactly its size = the bundle's (blocking) | 5 s / 30 s | 0.1 s / 1 s |
| `cm33_running` | `devmem`, else a python3 `/dev/mem` read, of `0x4F700FF0` (magic), offset 4 (version), offset 8 (counter); the counter again after 2 s | magic `0xA10D0683`, version 1, counter advanced by 1 to 4; informational, blocking with a `cm33` in the bundle; recorded as unread (missing tool) on an image with neither devmem nor python3 | 15 s | 2.2 s |
| `openamp_uio` | `/sys/class/uio/uio*/name` | the seven OpenAMP nodes | 5 s | 0.1 s |
| `usb_host` | `/sys/bus/usb/devices/usb*` | >= 4 root hubs | 5 s | 0.1 s |
| `usb_device`, `sd_card` | `dd` 4 MiB from the device | device present, `4+0 records out` | 15 s | 1 s |
| `sd_host` | platform driver link | driver bound | 5 s | 0.1 s |
| `systemd_failed` | `systemctl --failed` | exit status 0 and no failed unit outside the allowlist; a failing `systemctl` or output that is not a unit list is `unread` | 5 s | 0.3 s |
| `dmesg_fatal` | `dmesg -r` | none of the fault signatures (`SCL is stuck low`, `Failed to reset the dma`, `mmc_select_hs200 failed`, `I/O error`, `EXT4-fs error`, an oops, ...) | 5 s | 0.3 s |
| `dmesg_clean` | the same output | no error-level line outside the allowlist (informational) | - | - |
| `gpio_keys` | `/sys/class/input/input*/name` | `gpio-keys` registered | 5 s | 0.1 s |
| `eeprom_manifest` | 128 bytes at `0x50` | = the committed `{serial}.manifest.bin`, else magic, CRC, SKU and serial | 5 s | 0.2 s |
| `secure_page` | the sealed 64-byte read | = the staged secure page, else not blank | 5 s | 0.2 s |
| `supply_power_idle` | host: `MEAS:CURR?`, `MEAS:VOLT?` | 3.30..4.95 W (`v2n-m1`); also records `psu_voltage_v`, `psu_current_a` | - | 0.3 s |
| `carrier_{part}_{addr}` | one ID register read per carrier device | WHO_AM_I `0x67`, chip ID `0x43` / `0x50`, manufacturer ID `0x5449`; an answer for the expanders | 5 s | 0.1 s |
| `rail_3v3`, `rail_1v8`, `rail_5v` | the monitor's configuration, bus and shunt registers | bus voltage nominal +-5 %, current within the monitor's range | 5 s | 0.1 s |
| `audio` | codec driver links, `aplay` 2 s of silence | both amplifiers bound, `aplay` exit 0, 1.8..2.5 s | 10 s | 2.3 s |
| `display_dsi` | `/sys/class/drm/card*-DSI-*/status` | a DSI connector is registered | 5 s | 0.1 s |
| `camera` | `/sys/class/video4linux/*/name` | a device matching the fixture's regex | 5 s | 0.1 s |
| `can_loopback` | both links at 500 kbit/s, one frame CAN0 to CAN1 (python3), then both links back to their previous state | the frame arrives intact | 10 s | 1 s |
| `uart_loopback` | 24 bytes at 115200 on the header UART (python3) | the same bytes come back | 10 s | 0.5 s |

### First bench run

Nothing here has run on hardware. For the first run:

1. Use a **scratch `--ledger-root`** (a copy of the schema directory is enough), not the
   production ledger: the first run exists to compare formats, not to record a unit.
2. Supply at **15 V** (the idle figures behind `supply_power_idle_w` were taken there).
3. `bench.yaml`: set `functional_test.carrier`, and leave `ble_advertiser`, `dxm1_model`,
   `can_loopback` and `uart_loopback` **off** for the first pass (their tools and device nodes
   are the least certain). `usb_stick`, `eth1_cable` and `wifi_ap` can be on if the bench has them.
4. Run `--from cold_boot_test` on a provisioned unit, or `--only functional_test` on a unit
   already booted from its eMMC.
5. Keep the step log `logs/<serial>/functional_test-<stamp>.log`: every check's value, then
   the raw script output after `--- script output ---`, framed `@@ALPFT <check> <rc>` ...
   `@@ALPFT-END <check>`.
6. Expect three informational failures on today's units: `rtc_time_set` (nothing sets the RTC),
   `rtc_backup_mode` (backup switchover is disabled in the RTC's configuration) and
   `cm33_firmware` (no step programs a CM33 image while the bundle has no `cm33`). Expect `dmesg_clean` to need its allowlist
   adjusted (it was written from one boot log without levels).

Output formats with **no real-log evidence yet**; compare each frame with what the judge
assumes and fix the fragment, the judge or the expected value:

- `mmc extcsd read`: the `Life Time Estimation A/B [...]: 0x..` and `Pre EOL information [...]: 0x..` lines.
- `dd`: the `64+0 records out` / `4+0 records out` lines; `mountpoint -d /`.
- `iw reg get` (a `country XX:` line) and `iw dev wlan0 scan` (`BSS `, `signal:`, `SSID:`), and
  that the scan works while `wlan0` is otherwise unused.
- `hciconfig` (`UP RUNNING`, `BD Address:`) and `hcitool lescan` (one `<address> <name>` line
  per report); whether both are on the image; whether the raw scan works beside `bluetoothd`.
  (`bluetoothctl` is no longer used.)
- `/sys/bus/pci/devices/0000:01:00.0/current_link_width` (`2`) and `current_link_speed`
  (`8.0 GT/s PCIe`): the values are from the kernel log, the sysfs strings are not.
- `run_model`: its name, its `-m` / `-l` options and an output line containing `FPS`.
- `/dev/drpai0`, `/dev/mali0` and the platform device names.
- `/sys/class/rtc/rtc0/name` containing `rv3028`; the RTC seconds register at `0x00` counting
  while the clock is unset; register `0x37` bits 3:2 as the switchover mode (datasheet, not
  re-read for this change).
- The TMP112 temperature register: two bytes, 12-bit left-justified, 0.0625 degC per count.
- `thermal_zone*/temp` in millidegrees.
- Every carrier ID register (`0x75`; `0x00` after two dummy bytes; `0x01`; `0x3E`) and that
  reading the expanders' input register is harmless; every rail register (bus LSB 1.6 mV,
  shunt LSB 2.5 uV or 0.625 uV with the range bit; the input monitor's 24-bit bus register at
  195.3125 uV per count).
- `aplay` on the image, the card name, and 2 s of silence taking 1.8..2.5 s.
- `dmesg -r` printing `<level>` prefixes and the level of each allowlisted line.
- `systemctl --failed --no-legend --plain`: one unit per line, the unit name first, exit status 0.
- `can0` / `can1` existing at all; the header UART's device node; camera device names.
- The SD card's `device/type` reading `SD`.
- The `eth1_link` ping really leaving through that port (`ping -I`, and the port's RX counter
  rising by the two replies).
- Fractional `sleep` (`sleep 0.01`, `sleep 0.05` in the secure-element loop).
- `setsid` and `mktemp -d` on the image.
- `MEAS:VOLT? CH<n>` on the supply, the 3.30..4.95 W band, and that a reading right after login
  is an idle reading.
- Every time estimate, and that the concurrent lanes do not disturb each other (the Wi-Fi and
  Bluetooth scans share one lane on purpose; they share the radio).

## Provisioning SD payload store

Without it, every payload crosses a wire: the GD32 images go base64 over the 115200-baud console (about 13 min on 2026W38-0002 when the board has no network), and the DX-M1 files, the DTBs and the 210 MB `wic.gz` go over SSH on a 100 Mbit switch. The board boots Linux from the provisioning SD for every write step, so the first unit caches each payload on the SD and every later unit reads it locally.

The store is a directory on the SD's root filesystem, `/var/lib/alp-payload/<bundle sha256>/`. It needs no SD preparation and no partition: the root has several GB free. It is used only when the board runs from the SD, never when its root is the eMMC, and it is on by default (`--no-payload-store` disables it). `plan` never touches it. The card must carry the marker file `/var/lib/alp-payload/.provisioning-sd` (`touch` it on the internal provisioning SD only): without it the store stays off and every file is pushed, so the license-gated `dxm1_*` files never land on a release card.

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

`functional_test` runs on the **shipping** image and additionally uses, where the check exists: `ping`, `iw`, `hciconfig` and `hcitool` (bluez, fixture only for the scan), `aplay`, `dxrt-cli`, `run_model` (fixture only), `systemctl`, `mktemp`, `setsid`, `awk`, `cut`, `sed`, `seq`, `sort`, `grep`. Each check names its tools and tests for them first: a missing one makes that check `unread (missing tool: <name>)`, never `pass` and never `fail`, which fails the step for a blocking check. That is how a missing tool is found on the first bench run.

Not needed: `sfdisk`, `partx`, `findfs`, `parted`, `blockdev`, `bmaptool`, `mke2fs`, and `dd iflag=fullblock`.

## CM33 image

Every E1M-V2M103 is to ship with a CM33 firmware image (maintainer decision); which image is still open, so `cm33` is optional in the bundle and not in `V2N_REQUIRED_ROLES` yet (it becomes required once a bundle carries it).

- **Where it lives.** xSPI byte `0x200000` = `mtd1` offset `0x1A0000` (`gates.CM33_REGION_OFFSET`). BL2 copies it raw to SRAM `0x08000000`; the CM33 starts at `0x08003000`.
- **Padding.** The bundle's `cm33` component (`flash_target` `xspi:mtd1`) is the stored image: `0x3000` zero bytes followed by Zephyr's `zephyr.bin`, the same bytes the `rzv2n_mtd_flash` west runner writes. `write_cm33` and `cm33_firmware` md5 exactly these bytes.
- **Size limit.** At most `0x30000` bytes in all; BL2 silently truncates a larger image. `preflight` and `check_som_bundle.py` refuse a `cm33` whose first `0x3000` bytes are not zero, whose initial SP (word at `0x3000`) is outside SRAM0 (`0x08xxxxxx`, the runner's test), whose reset vector (word at `0x3004`) lacks the Thumb bit or lies outside `0x08003000..0x08033000`, or that exceeds `0x30000`. The FIP must still end below `0x1A0000`; its erase never reaches the CM33 region.
- **When it runs.** BL2 releases the CM33 in every boot mode. In eMMC or eSD boot today it loads the image from a location nothing wrote, so the CM33 faults and stays locked (harmless). With TF-A fix #2658 it loads the image from xSPI in every mode, so after `write_cm33` the CM33 image also runs alongside the provisioning Linux on later mode-1 boots, not only from `cold_boot_test` on. The CM33 cannot be restarted from Linux.
- **No clash with the provisioning steps.** The shipping image is the idle shim: it writes only the DDR beacon at `0x4F700FF0..0x4F700FF8` and claims no peripheral (no GPIO, SPI, I2C, SCI, DMA). No provisioning step touches that window (the census reads only the SoC `SYS` registers at `0x10430300..0x10430308`; `cm33_running` only reads the beacon), and `write_xspi` / `write_cm33` erase xSPI while the CM33 runs from the SRAM copy BL2 made, not XIP. A different CM33 image that owns a peripheral a step uses (the GD32 bridge link, RIIC) would need its own check.
- **Liveness beacon.** `firmware/alp-stock-shim` writes a magic (`0xA10D0683`, written last), an image kind (`0x00000100`, the idle shim; RPC firmware uses values below `0x100`) and a counter (+1 about every second) into the `rsctbl` window the A55 DT reserves, A55 `0x4F700FF0` / `0x4F700FF4` / `0x4F700FF8` (the `rpmsg-v2n` example's layout). The CM33 has no console here, so `cm33_running` reads the three words, waits 2 s and reads the counter again; magic and version must match and the counter must advance by 1..4 (expected values: `cm33_beacon` in `functest-expect-v2n.yaml`). Read-only. It proves the stock image runs; a different CM33 image fails the check unless it writes the same beacon.
- **No verification at boot.** No header, signature or checksum; only the tool's md5 readback and the blocking `cm33_firmware` check cover the image.

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
  `--replace-identity` (needs `--reprovision-from`) archives the committed and
  staged blobs as `<serial>.manifest.<old-hwrev>-<date>.bin` and
  `<serial>.secure-page.<old-hwrev>-<date>.bin` (`.staged.bin` for the staged
  ones) before the new ones are staged and promoted, instead of failing after
  the EEPROM write.
- **The 5L35023B cannot be re-burned in-system.** `clkgen_verify` only reads;
  a bad OTP image means a bad unit, not a fixable one.
- **`dxm1_npu_flash` swaps the release DTB while it runs.** The release DTB is backed up as `/boot/<fdtfile>.release` and restored (md5-verified) on success and on failure; if a run is killed, the next run restores it first. The BOOT_CFG straps (E1M `IO17`/`IO19`/`IO20` low) are a carrier property the tool cannot measure.

## Testing

Every module has pytest tests with fakes (`tests/scripts/provision_fakes.py`
and `tests/scripts/test_provision_*.py`); no hardware is needed.
`test_provision_functest.py` holds, for every functional check, a healthy
answer and at least one wrong answer that must fail it, and runs the generated
script's runner (framing, lanes, the timeout kill) on the host's own `sh`.
