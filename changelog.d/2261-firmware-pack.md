### Added — `E1M-V2N103` / `E1M-V2M103` firmware pack: BL2 DDR param + 4 GB U-Boot config (#2261)

Closes the `TODO(firmware-pack)` the two x103 machine confs carried since
they were added: `bitbake trusted-firmware-a u-boot` (+ `firmware-pack`)
now produces the correct 4 GB boot firmware for `e1m-v2n103-a55` /
`e1m-v2m103-a55`.

- `trusted-firmware-a_%.bbappend`: the BL2 DDR param file is now selected
  per MACHINE via `ALP_TFA_DDR_SRC` — the rzv2n-family default
  (`ddr_param_def_lpddr4-alp.c`, config `L4X.R2W32X16D16S32.ADEE`) for
  every existing MACHINE, and a new 4 GB sibling
  (`ddr_param_def_lpddr4-alp-d8s32.c`, config `L4X.R2W32X16D8S32.ADEE`,
  bench-proven 2026-09-24 on an E1M-V2M103: 30/30 cold boots, memtester
  16/16) for `e1m-v2n103-a55`/`e1m-v2m103-a55` only. Both files stay
  private (`alp-sdk-internal` overlay), same split as the existing
  default.
- `u-boot_%.bbappend`: a new `0003-rzv2n-dev-ALP-E1M-4gb-memory-tier.patch`
  (applies after 0001/0002) sets `CONFIG_SYS_SDRAM_SIZE` and the control
  DT's `memory@48000000` node to 4 GB, gated to the two x103 MACHINEs only.

**Also fixed in the same slice — the DDR-param install was a silent
no-op through bitbake, for every rzv2n-family MACHINE, until now.**
`do_configure[noexec] = "1"` in meta-arm's `trusted-firmware-a.inc` means
bitbake never executes a noexec task's function body; the DDR-param
install lived in `do_configure:append:rzv2n-family()`, so it silently
never ran, for `e1m-v2n101-a55`/`e1m-v2n102-a55`/`e1m-v2m101-a55`/
`e1m-v2m102-a55` too, not just the new x103 MACHINEs. Every bitbake-built
BL2 to date compiled the **stock Renesas DDR param** (`ddr_version_str`
`"03.01"`), plus meta-rz-drpai's `0000-ddr_param_def_lpddr4-rzv2n_1.patch`
MC-arbitration tweaks where that layer was present — **not** the intended
ALP gen_tool file. Moved the install to `do_compile:prepend:rzv2n-family()`
(not noexec) and added a build-time assertion that the two x103 MACHINEs
actually resolve to the D8S32 file. Confirmed on alplab-gw (2026-09-24):
`e1m-v2n103-a55`/`e1m-v2m103-a55` compile the correct D8S32 file into BL2
(byte-identical to the intended source, not merely "unchanged" — the prior
report of `e1m-v2m101-a55` being "regression-checked unchanged" meant
unchanged **relative to the intended ALP D16S32 file**, which itself was
newly-taking-effect through this same fix, not unchanged relative to
what bitbake shipped before).

**Maintainer decisions on the two items raised in review (2026-09-24):**
- **V2x101 production DRAM part/tier: still undecided, a production
  blocker.** `E1M-V2N101`/`E1M-V2M101`'s own catalogue entry
  (`metadata/e1m_modules/E1M-V2N101.yaml`, `E1M-V2M101.yaml`:
  `dram_mbit: 32768` = 4 GB) states the same 4 GB size this x103
  firmware targets, yet those MACHINEs continue to ship the
  family-default D16S32 (8 GB) config. Decided: firmware **stays**
  D16S32 for V2N101/V2M101; which of the two facts is actually right
  remains open — see the `# OPEN` note added to both machine confs and
  `docs/build-yocto-v2n.md`.
- **Fold meta-rz-drpai's DDR MC-arbitration register values in: YES.**
  `param_setup_mc` `0x0134`/`0x0135`/`0x0178`/`0x017f`/`0x0181`/`0x02cd`/
  `0x02cf`/`0x02d0` — which the ALP gen_tool files don't carry, and which
  `do_compile:prepend`'s whole-file replacement discards on any
  DRP-AI-enabled build — get folded into both `ddr_param_def_lpddr4-alp.c`
  and `-alp-d8s32.c`. Confirmed density-independent (identical current
  baseline in both files) and non-colliding with the D8 range regs/tRFC
  rows; `param_phyinit_2d_dat1[15]` stays at the ALP value (the drpai
  patch doesn't touch it). **Not yet applied** — the target values are
  derived, verified, and recorded in `alp-sdk-internal
  docs/bootloader-equivalence-verdict.md`, but editing those two private
  register-table files was blocked by a permission classifier this
  session on every attempt. Still needs: applying the 8 values, a
  provenance note in each file's header, a rebuild + bl2-binary proof on
  e1m-v2m103-a55, and it is **not silicon-verified either way**.

See `alp-sdk-internal docs/bootloader-equivalence-verdict.md` (corrected the
same day) for the discovery trail and exact target values.
