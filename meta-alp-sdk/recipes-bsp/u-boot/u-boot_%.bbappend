# meta-alp-sdk: bring up the on-module DEEPX DX-M1 in U-Boot, before the
# A55 kernel's PCIe root complex probes.
#
# WHY: the V2N-M1 SoM wires the DX-M1 behind two passive PI3DBS12212A PCIe
# muxes (PD on Renesas P80, SEL on P95) and an active-low reset M1_RESET
# (PA6). The A55 Linux PCIe RC (rzg3s-pcie-host) trains the link in its
# builtin probe very early (~1.7s) and the driver has no gpio/reset hook,
# so Linux gpio-hogs apply too late -- the RC trains into a powered-down
# mux and fails with -ETIMEDOUT. Boot is A55-only here, so the only layer
# that runs before the RC is U-Boot.
#
# WHAT: board_late_init() (board/renesas/rzv2n-dev/rzv2n-dev.c) reads the
# on-module hardware-info manifest from the RIIC0 24C128 EEPROM (0x50),
# validates magic + schema_version + CRC32 (EEPROM-MANIFEST-SPEC.md), and
# ONLY when the manifest reports family "v2n-m1" sets, before bootcmd:
#     P80 = low   -> enable the PI3DBS12212A muxes (PD active-low)
#     P95 = low   -> route PCIe to the DEEPX path (path_0)
#     PA6 = high  -> release M1_RESET (active-low)
# So the same U-Boot is safe on a non-DEEPX V2N SoM (no manifest match ->
# DEEPX path left untouched). RIIC0 (P30/P31) pinmux is added to s_init and
# the i2c0 node enabled in rzv2n-dev.dts. (Pair with the kernel dtb's pcie
# num-lanes=2.) DEEPX rails are NOT always-on: 0004 below sequences the
# on-module DA9292 CH2 rail (0.75V) over RIIC8/BRD_I2C and releases P64
# BEFORE this reset-release step ever runs -- see 0004's own header
# comment and CONFIG_ALP_E1M_DEEPX_RAIL below.
#
# Targets the renesas-u-boot-cip SRCREV this BSP pins for rzv2n-family
# (2024.07, bcf29d98); applies on top of meta-renesas's rzv2n-dev PMIC-I2C
# removal patch.
#
# 0004 below applies on top of 0002 (md5 c546f00cabca346e335febd21ecbc440):
# both touch board/renesas/rzv2n-dev/Kconfig and 0004's hunks use 0002's
# lines as context, so 0002 must stay ahead of it.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:rzv2n-family = " \
    file://0001-rzv2n-dev-EEPROM-gated-DEEPX-DX-M1-PCIe-bring-up.patch \
    file://0002-rzv2n-dev-ALP-E1M-production-boot.patch \
    file://no-dirty-version.cfg \
    file://gigadevice-xspi.cfg \
"

# gigadevice-xspi.cfg: enables CONFIG_SPI_FLASH_GIGADEVICE. Production E1M
# V2N-family modules carry a GigaDevice LX-family xSPI NOR on some SKUs;
# its JEDEC IDs are already upstream in spi-nor-ids.c, only the vendor
# select was off. KNOWN LIMIT: this U-Boot's Renesas xSPI driver fails
# reads that cross the 16 MiB boundary (bench-observed "Read: ERROR 1"
# at 0xFFFF00+0x200) -- keep boot content below 16 MiB. Writes above
# 16 MiB are untested. See the fragment for detail.

# no-dirty-version.cfg: the vendor defconfig's CONFIG_LOCALVERSION_AUTO
# appends `git describe --dirty` -- always "-dirty" here because the
# patches above are git-applied into the tree -- leaking a "-dirty"
# flag + the upstream SHA into the boot banner. The fragment disables
# the auto version and pins an ALP localversion. Merged into the
# resolved defconfig by u-boot-configure.inc (find_cfgs() + merge_config.sh
# pick up any *.cfg in SRC_URI) -- the same path prod-boot.cfg uses.

# 0002 (production boot): two build-gated ALP additions to the same
# rzv2n-dev board files.
#   CONFIG_ALP_E1M_EMMC_1V8 (default y in the patched defconfig):
#     drive eMMC_V_SEL (PA0) high in board_init, BEFORE the first MMC
#     access (initr_env loads the environment from eMMC right after
#     board_init in init_sequence_r) -- the E1M SoM's eMMC/SD0 IO rail
#     powers up at 3.3 V and eMMC has no live VccQ-switch handshake,
#     so the whole chain must run the card at 1.8 V from the start.
#     Pairs with the Linux-side PA0 gpio-hog (e1m-v2n-som.dtsi).
#   CONFIG_ALP_PROD_BOOT (default n, DEV-target only): production
#     kernel cmdline (console= pinned + quiet, earlycon dropped). The
#     dev cmdline keeps earlycon but gains console=ttySC0,115200,
#     which stops the kernel replaying the early log across the
#     console handover. The cmdline is rebuilt at CONFIG_BOOTCOMMAND
#     (patch-safe vs the build-varying env block); the future per-SKU
#     fdtfile derivation must also happen there, AFTER the leading
#     'env default -a' wipe -- see the comment in the patch.
# VALIDATION: bitbake-built dev + prod with config asserts; the FIP
# (BL2+BL31+u-boot, manual flow) was built 2026-06-12 with both ALP
# patches and the u-boot binary content-verified (alp_root bootcmd +
# pinned console). On-silicon FIP flash + boot is PENDING maintainer
# authorization (a persistent bootloader write to the shared bench
# board). The manual FIP flow (build_custom_fip_v630_deepx.sh, WSL)
# was updated the same day to apply 0002 alongside the DEEPX patch and
# to verify it (strings u-boot | grep 'setenv alp_root').

# Production boot lockdown (BOOTDELAY=0 + keyed autoboot + the prod
# cmdline above): opt-in for release-bundle builds only. An
# un-overridden prod build has an EMPTY stop string (= no stop
# sequence at all); the internal release pipeline injects the real
# per-product stop string. See prod-boot.cfg for the lock mechanism
# and the deferred saved-env hole.
ALP_PROD_BOOT ?= "0"
SRC_URI:append:rzv2n-family = "${@' file://prod-boot.cfg' if bb.utils.to_boolean(d.getVar('ALP_PROD_BOOT')) else ''}"

# 4 GB / 16 GB memory-tier (x103) SDRAM-size + control-DT memory-node patch.
# rzv2n-dev.h/.dts are shared source compiled identically for every
# rzv2n-family MACHINE (CONFIG_TARGET_RZV2N_DEV, above). e1m-v2n101-a55/
# e1m-v2m101-a55 currently build the family-default (unpatched) rzv2n-dev.h/
# .dts -- DECIDED (maintainer, 2026-09-24): they stay family-default, not
# x103-sized. Whether that firmware choice or the catalogue's dram_mbit is
# the one that's wrong is a separate, still-undecided PRODUCTION BLOCKER
# (see the OPEN comment in e1m-v2n101-a55.conf / e1m-v2m101-a55.conf and
# trusted-firmware-a_%.bbappend), not decided here.
#
# Selected by a MACHINE equality check, not an OVERRIDES suffix, DELIBERATELY:
# a `SRC_URI:append:e1m-v2n103-a55` form would face the same override-rank
# hazard documented on ALP_TFA_DDR_SRC in trusted-firmware-a_%.bbappend
# (e1m-v2n101-a55/e1m-v2m101-a55 sit to the right of the x103 names in the
# x103 MACHINEs' own MACHINEOVERRIDES chain); `d.getVar('MACHINE') in (...)`
# has no such hazard -- it's a plain string compare, immune to OVERRIDES rank.
#
# It is a THIRD SRC_URI:append:rzv2n-family statement (same override as
# 0001/0002 above), so among the meta-alp-sdk entries it lands after 0001 and
# 0002 and do_patch applies it after both -- verified live via `bitbake -e
# u-boot` (SRC_URI order), not just reasoned about. It does NOT apply last
# overall: meta-rz-drpai's add-ether and meta-rz-opencva's OpenCVA/Codec
# patches (their OWN, separate u-boot bbappends) still land after this one in
# the full do_patch sequence -- see the patch-fuzz demotion comment below for
# why those vendor hunks fuzz against ALP's context.
SRC_URI:append:rzv2n-family = "${@' file://0003-rzv2n-dev-ALP-E1M-4gb-memory-tier.patch' if d.getVar('MACHINE') in ('e1m-v2n103-a55', 'e1m-v2m103-a55') else ''}"

# 0004 (DEEPX rail bring-up): board_late_init() sequences the on-module
# DA9292 PMIC's CH2 to 0.75V and confirms power-good BEFORE the 0001 mux/
# M1_RESET-release step is allowed to run -- U-Boot is the sole writer of
# the DA9292 and the sole driver of P64 (DEEPX_CORE_0P75_EN) / P65
# (DEEPX_PWR_EN_REQ) WHENEVER U-BOOT RUNS -- which is every A55 boot,
# including one released by the CM33 (see below). CONFIG_ALP_E1M_DEEPX_RAIL
# (default n) forces the rail step even before a V2M unit's EEPROM manifest
# is burned; wired below for the V2M MACHINEs only via deepx-rail.cfg -- see
# that file's own header for why forcing it there is redundant, not a
# relaxation, once the manifest is valid.
#
# CM33-boot mode (RZ/V2N pin BOOTSELCPU strapped low -- RZ/V2N HW manual
# R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table 1.9-1, and see
# metadata/e1m_modules/v2n/core-ownership.yaml's boot_mode_core in
# alp-sdk): the CM33 masters RIIC8/BRD_I2C and runs the DA9292 CH2
# sequence itself FIRST (alp-sdk's examples/v2n/v2n-cm33-deepx-rail),
# then releases the CA55. By the time THIS 0004 code below runs, the
# CM33 has already handed the bus back -- 0004's program phase is
# idempotent on a warm rail (VSTEP already 0 and both VOUT_CH2 registers
# already at the target code -> steps 3-5 skip entirely, zero CTRL_01
# writes), so re-running it here is a VERIFY, not a re-sequence, and safe
# without any code change. FOLLOW-UP (not done in this change -- U-Boot
# is not rebuilt here): 0004 should read the boot-CPU-select state
# (ACT88760 GPIO5 `V2N_BOOT_CPU_SEL`) and log that it ran in verify-only
# mode when the rail was already up, instead of relying on the warm
# path's silence to imply it.
#
# Placed AFTER the 0003 append above, not with 0001/0002 at the top: the
# meta-alp-sdk patch order is PMIC-removal (meta-renesas, ahead of every
# entry here), 0001, 0002, 0003 (x103 MACHINEs only), 0004 -- 0004 must
# see whatever 0003 already did to rzv2n-dev.h so a future 0004 hunk
# touching that file lands on the same context 0003 leaves, not the
# pre-0003 one. 0003 and this patch currently touch disjoint files
# (0003: arch/arm/dts/rzv2n-dev.dts + include/configs/rzv2n-dev.h; 0004:
# board/renesas/rzv2n-dev/Kconfig + rzv2n-dev.c), so do_patch succeeds
# either order today -- the ordering here is enforced ahead of any such
# overlap, not reacting to one.
SRC_URI:append:rzv2n-family = " file://0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch"
SRC_URI:append:e1m-v2m101-a55 = " file://deepx-rail.cfg"
SRC_URI:append:e1m-v2m102-a55 = " file://deepx-rail.cfg"
SRC_URI:append:e1m-v2m103-a55 = " file://deepx-rail.cfg"

# 0005 (RIIC combined-read fix + busy-bus recovery): the vendor rzg2l_riic
# driver's riic_xfer() ran the two struct i2c_msg's a dm_i2c_read()
# register read always builds (msg[0] = write-the-register-offset,
# msg[1] = paired read) as two INDEPENDENT START..STOP bus transactions
# with a spurious STOP between them, and retransmitted the offset
# byte(s) in each -- not the single repeated-START transaction
# dm_i2c_read() intends. This is a generic rzg2l_riic/R9A09G056 defect
# (every register read on any of the SoC's nine RIIC instances goes
# through the same riic_xfer() path), not limited to RIIC8, so it is
# unconditional for the whole family rather than gated to the V2M
# MACHINEs the way 0004's Kconfig knob is. The fast path packs the
# offset big-endian for any 1-4 byte width (riic_send_mem_addr() already
# supports that range), so it also covers the on-module 24C128 EEPROM's
# 2-byte offset on RIIC0 -- previously stuck on the original,
# double-transaction loop. riic_check_busy() also gains a one-shot
# recovery (clock SDA free, full ICE=0+IICRST reset, retry) on a BBSY
# timeout, since riic_ops implements no dm_i2c bus-recovery op ("i2c
# reset" -> "Not supported by the driver"), and riic_wait_for_icsr2()'s
# timeout diagnostic now prints unconditionally instead of only in
# DEBUG builds.
#
# This is a real, independently-reasoned driver defect found while
# chasing a bench failure on E1M-V2M103 2026-09-24 (0004's first DA9292
# read timing out with -ETIMEDOUT, and a separate console session where
# an "i2c probe" found the DA9292 and the next "i2c md" then failed
# -EBUSY). It runs on silicon in the bench-passing chain (v4..v6, see
# changelog.d/2045): not isolated to prove it alone fixes either
# symptom on its own, but every build that brought the DEEPX rail up on
# silicon included it.
#
# Disjoint file from every patch above (drivers/i2c/rzg2l_riic.c vs
# board/renesas/rzv2n-dev/*), so its position relative to them is not
# order-sensitive; placed after 0004 to read in the order these patches
# were authored. DA9292 writes are unaffected (dm_i2c_write() already
# builds one combined message), so 0004's own writes needed no change.
SRC_URI:append:rzv2n-family = " file://0005-i2c-rzg2l_riic-combined-register-read.patch"

# 0006 (P06/P07 pull-up + RIIC clock/restart fixes): bench root-caused
# (E1M-V2M103, 2026-09-24) the RIIC8/BRD_I2C arbitration-lost failure
# (ICSR2=0x0a, STOP|AL) that 0005 alone did not resolve. Two
# independently-sufficient fixes, both carried:
#   (a) rzv2n-dev.c s_init(): enable the SoC-internal pull-up on P06/P07
#       (PUPD_H, port 0) -- Linux already does this on the same net,
#       U-Boot left the register at POR (no pull-up) until now.
#   (b) rzg2l_riic.c riic_set_clock(): R9A09G056/057 (RZ/V2N, RZ/V2H)
#       run RIIC off a 100 MHz input, not the 50 MHz this shared
#       CKS/ICBRH/ICBRL table assumes -- CKS(5)/CKS(3) (was CKS(3)/
#       CKS(1) unmodified) for 100 kHz/400 kHz: one step restores the
#       nominal rate, a second step is deliberate margin for the
#       loaded BRD_I2C bus -- see the bench matrix below and the
#       fSCL derivation in riic_set_clock()'s own comment.
# Plus two smaller, unconditional hardening fixes in the same file
# targeting the same AL failure: riic_read_common() polls ICCR1 SDAI=1
# (bounded ~1 ms, printf on expiry) before the repeated START, and
# riic_check_busy() clears stale ICSR2 AL|STOP|NACKF|START and runs
# the existing IICRST recovery on AL instead of leaving it sticky.
# riic_wait_for_icsr2()'s timeout printf also gained ICCR1 alongside
# ICSR2/ICCR2. Disjoint files from 0001-0004 (rzv2n-dev.c's own hunk
# lands after 0004's I2C0 addition, which it does not touch) and
# additive-only to 0005's rzg2l_riic.c hunks, so ordering after 0005
# is not order-sensitive, only readable.
# BENCH-VERIFIED (E1M-V2M103, 2026-09-24): (a) above and a CKS-only
# clock fix passed on silicon -- the DEEPX rail programmed and the
# DA9292 register readback matched (see changelog.d/2045). PCIe itself
# was not exercised (blank EEPROM manifest).
#
# Also folds in a bit-7 burst-read corruption fix, source-derived then
# BENCH-MATRIXED (E1M-V2M103, 2026-09-24, RIIC8/BRD_I2C, DA9292 0x1E):
# a follow-up bench pass first found long reads losing bit 7 on bytes
# from roughly position 13 onward (position-dependent, not register-
# dependent; short reads stayed clean) and this patch originally
# carried only a protocol-ordering reorder in riic_i2c_raw_read() --
# ICMR3.ACKBT (NACK) and ICCR2.SP (stop request) now set BEFORE the
# ICDRR read that consumes the final byte instead of after, matching
# drivers/i2c/rz_riic.c riic_receive_data() and the RIIC master-receive
# flowchart. That reorder is a real, independently-correct protocol fix
# kept on its own merits, but a follow-up bench matrix (CKS=3/4/5, WAIT
# off, ICBRL=0xff) REFUTED it as the cause of the bit-7 loss: v4
# (without the reorder) and v5 (with it) behaved identically on the
# same burst read. The actual cause is a bus-speed margin issue on the
# heavily loaded BRD_I2C (~13 populated branches): CKS(4) (the value
# 0006 shipped with through v5) still loses bit 7 above ~12 bytes;
# CKS(5) (this version) is clean on both a 16-byte and a single 32-byte
# burst; CKS(3) fails outright (STOP detected, -110). Root mechanism is
# still unconfirmed -- Linux drives the same bus at 400 kHz IRQ-driven
# with clean reads -- a scope capture is pending; CKS(5) is carried on
# bench evidence, not a proven timing model. See changelog.d/2045 for
# the full matrix.
SRC_URI:append:rzv2n-family = " file://0006-rzv2n-dev-i2c-rzg2l_riic-p06-p07-pullup-clock-fix.patch"

# 0007 (on-module 5L35023B clock-generator OTP fixup): the on-module
# Renesas 5L35023B programmable clock generator (RIIC8/BRD_I2C, 7-bit
# 0x69) ships an OTP image whose single-ended routing is wrong for this
# SoM -- SE1 (feeds the SoC RTXIN and the Wi-Fi module's 32k LPO) comes
# up at 24.576 MHz instead of 32.768 kHz, and SE3 (audio clock) comes up
# at 22.5792 MHz instead of 24.576 MHz. Bench-confirmed (E1M-V2M103
# board #1, 2026-09-24): with the OTP defaults the SoC RTC (RTCA-3)
# fails to start ("Failed to setup the RTC!", -ETIMEDOUT); two volatile
# register writes (reg 0x24: 0x9c->0x8e, reg 0x21: 0x80->0xc0) fix it,
# after which the RTC counts at 32.768 kHz. Both are OTP-shadow
# registers and REVERT ON POWER-CYCLE (the OTP itself cannot be
# re-burned in-system), so alp_clk5l_fixup() runs unconditionally,
# on every boot, first in board_late_init() -- ahead of and independent
# of the 0004 DEEPX rail step -- for the whole rzv2n-family (this clock
# generator is present on every V2N/V2M SoM, not just DEEPX-populated
# V2M units). Guarded to single-byte reads only (see 0005/0006 above)
# and to writing only these two registers, and only when reg 0x00 reads
# the expected OTP-burned/addr-0x69 value (0xa0) and 0x24/0x21 read the
# exact as-shipped OTP pair -- any other readback is left untouched and
# only reported. NOT disjoint from 0004: this patch's board_late_init()
# hunk rewrites the `bool v2n_m1 = alp_som_is_v2n_m1();` declaration
# 0004 adds (splitting it into a bare declaration plus a later
# assignment, so alp_clk5l_fixup() can run first) -- it must apply on
# top of 0004's context, not merely after it for readability. It is
# disjoint from 0005/0006 (drivers/i2c/rzg2l_riic.c), so its position
# after those two is not order-sensitive, only readable.
SRC_URI:append:rzv2n-family = " file://0007-rzv2n-dev-ALP-E1M-clkgen-otp-fixup.patch"

# Per-SKU board dtb for CONFIG_BOOTCOMMAND (alp-sdk#1252).  One u-boot
# binary serves both families, so the dtb basename is a Kconfig string
# (CONFIG_ALP_E1M_FDTFILE, patch 0002) whose default suits the V2N SKUs;
# the V2M MACHINEs override it through the same *.cfg channel
# prod-boot.cfg uses (u-boot-configure.inc's find_cfgs() +
# merge_config.sh pick up any *.cfg in SRC_URI).
#
# Scoped by MACHINE, not by rzv2n-family: the family override covers the
# V2N SKUs too, and applying the V2M name there would invert the bug.
# Any new V2M MACHINE needs a line here -- there is no wildcard that is
# safe, because "which dtb does this image contain" is a per-MACHINE fact
# (KERNEL_DEVICETREE), not a family one.
SRC_URI:append:e1m-v2m101-a55 = " file://fdtfile-v2m.cfg"
SRC_URI:append:e1m-v2m102-a55 = " file://fdtfile-v2m.cfg"
SRC_URI:append:e1m-v2m103-a55 = " file://fdtfile-v2m.cfg"

# Build U-Boot with the rzv2n-dev config, not the machine's stock rzv2n-evk.
# The DEEPX bring-up patched above lives in board/renesas/rzv2n-dev/rzv2n-dev.c,
# which is ONLY compiled under CONFIG_TARGET_RZV2N_DEV -- so the dev config is
# mandatory, not cosmetic. It also reproduces the silicon-validated manual
# bootloader (DEV target, env-in-MMC, Renesas clock driver off). The longer-term
# rzv2n-dev-vs-rzv2n-evk production-config decision (clock driver, env storage)
# is deliberately deferred -- see the alp-sdk-internal som-productization docs.
UBOOT_CONFIG[rzv2n-evk] = "rzv2n-dev_defconfig"

# Demote patch-fuzz from fatal-error to warning -- ONLY for the
# rzv2n-family u-boot build. A u-boot_%.bbappend fires for EVERY machine's
# u-boot recipe, so the demotion must carry the same :rzv2n-family
# machine override the SRC_URI additions above use; unscoped it would
# relax the fatal patch-fuzz QA gate for every other machine in the
# distro too (issue #245).
# WHY the demotion at all: u-boot here is renesas-u-boot-cip pinned via
# +git (a moving base), and the vendor feature-layer patches
# (meta-rz-drpai add-ether, meta-rz-opencva OpenCVA+Codec) apply on top
# of it. Those vendor hunks land with a small context offset -- they
# apply correctly ("Hunk succeeded"), but Yocto's fatal patch-fuzz QA
# fails do_patch. The offset is structural, not a defect in our patches:
# the OpenCVA hunk in include/configs/rzv2n-evk.h fuzzes by the same
# 5 lines even though NO ALP patch touches evk.h -- that is pure +git
# base drift. (The ALP 0002 patch additionally shifts rzv2n-dev.h,
# compounding it.) Refreshing vendor patches each BSP bump is a
# treadmill and we do not own them, so demote the gate here rather than
# mask fuzz globally.
WARN_QA:append:rzv2n-family = " patch-fuzz"
ERROR_QA:remove:rzv2n-family = "patch-fuzz"
