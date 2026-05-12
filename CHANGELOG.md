# Changelog

All notable changes to the ALP SDK are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
See [`VERSIONS.md`](VERSIONS.md) for the forward roadmap.

## [Unreleased] — v0.3.0 candidate

(Tracks `metadata/sdk_version.yaml`'s declared version.  Per
`VERSIONS.md`, v0.1 / v0.2 / v0.3 each ship the *surface* first
and accumulate runtime implementations in subsequent point
releases -- entries below collect every Added / Changed item
that lands before the v0.3.0 tag.)

> **Verification status.**  Entries below describe what's been
> *coded and merged* on `main`; passing `pr-plain-cmake` /
> `pr-twister` / `pr-static-analysis` is necessary but not
> sufficient to call an item "GA".  Real-hardware verification is
> tracked separately in [`docs/test-plan.md`](docs/test-plan.md)
> -- a release does not tag until every row gating it flips to ✅.

### Decided (hardware-design decisions captured 2026-05-12)

- **GD32_BOOT0 -> Renesas `P75`** (was E1M PWM5 / GPT4_GTIOC4B).  PWM5
  becomes GD32-only.  Host drives `P75` high before issuing a reset
  edge on `GD32_NRST` to enter the GigaDevice factory ISP bootloader.
- **GD32_NRST -> Renesas `P74`** (was E1M PWM4 / GPT4_GTIOC4A).  PWM4
  becomes GD32-only.  **Line is shared with the primary PMIC reset
  out** -- host pad MUST be configured open-drain (drive low to
  assert reset; HiZ to release).  An external pull-up returns the
  line to its released state.
- **DEEPX `M1_RESET` polarity -> active-LOW.** The
  `chips/deepx_dxm1/` driver default flipped from `ACTIVE_HIGH`
  (placeholder pending the schematic check) to `ACTIVE_LOW`
  (confirmed).  `deepx_dxm1_set_reset_polarity()` still lets
  carrier code override.
- **Murata `BT_DEV_WAKE` intentionally not routed on V2N.**  The
  `chips/murata_lbee5hy2fy/` driver already supported the NULL-
  pin-handle case; metadata + header doc updated to make the
  "not-routed" status explicit.
- **5L35023B I2C address -> 7-bit `0x68`** (8-bit write `0xD0`) per
  the Renesas 5L35023 public datasheet.

### Added (2026-05-13 overnight run)

- **`chips/gd32_swd/` -- bit-bang SWD controller for the GD32G553 (2026-05-13).**
  Renesas-side software SWD controller that drives `GD32_SWDIO` +
  `GD32_SWCLK` + (optional) `GD32_NRST` as GPIOs to reflash the V2N
  module's companion GD32G553MEY7TR without an external probe.  No
  vendor SDK pulled in; algorithm follows Arm DDI 0316C (ADIv5) +
  GD32G553 User Manual §3 (FMC).  Surface: `gd32_swd_init`,
  `gd32_swd_connect` (line reset + JTAG-to-SWD switch + DPIDR read
  matching expected IDCODE `0x6BA02477`), `gd32_swd_halt` (Cortex-M33
  DHCSR DBGKEY + C_HALT), `gd32_swd_flash_erase` / `_write` /
  `_verify` (FMC KEY1 + KEY2 unlock, sector erase, doubleword
  programming), `gd32_swd_reset_and_run` (HW NRST pulse when wired;
  AIRCR.SYSRESETREQ otherwise).  `driver_status: partial` until
  exercised on real silicon.  New Kconfig: `CONFIG_ALP_SDK_CHIP_GD32_SWD`.
  Header: `<alp/chips/gd32_swd.h>`.  Manifest:
  `metadata/chips/gd32_swd.yaml`.

- **GD32 application-bootloader scaffold (2026-05-13).**
  New `gd32-bridge/src/bootloader/` subdir routes the seven OTA
  opcodes reserved in the bridge protocol (`CMD_OTA_BEGIN..ABORT`,
  `0xF0..0xF6`) through `bl_dispatch_ota`.  Handler bodies reply
  `STATUS_NOSUPPORT` until the real implementation lands;
  `CMD_OTA_GET_STATE` is read-only and answers concretely so the
  host driver path can be exercised early.  Hooked into
  `protocol_dispatch` in `gd32-bridge/src/protocol.c`.  Integration
  detail is held by the maintainer outside this tree.
  `docs/gd32-bridge.md` flashing table now lists both paths as
  "Scaffolded".

- **`examples/v2n-pwm-fan-control/` -- GD32-side PWM fan curve (2026-05-13).**
  Ramps a single GD32-driven PWM channel through five duty stops at
  a 25 kHz carrier (above audible) via `gd32g553_pwm_set`.
  Demonstrates the post-2026-05-11 schematic-rev convention that
  E1M PWM6 / PWM7 are GD32-only on V2N.

- **`examples/v2n-secure-element-sign/` -- OPTIGA Trust M ECDSA sign (2026-05-13).**
  Initialises the OPTIGA Trust M, reads its product-info object,
  then issues a hand-rolled `CalcSign` (0x31) APDU against key OID
  `0xE0F0` over a fixed SHA-256 digest.  Stops short of the PSA
  driver bridge -- that lands when Infineon's Host Library is
  vendored.

- **`examples/v2n-xspi-flash-readwrite/` -- on-module xSPI NOR erase / write / verify (2026-05-13).**
  Erases one 4 KiB sector at the last sector of the part, writes a
  0x00..0xFF ramp, reads it back via Zephyr's standard
  `flash_read`, and reports the comparison.  Resolves the part via
  the `xspi-flash` DT alias so it works on any V2N board file that
  exposes it.

- **`examples/v2n-emmc-block-stat/` -- on-module eMMC geometry + first-block read (2026-05-13).**
  Calls `disk_access_init("SD")` + the standard
  `DISK_IOCTL_GET_SECTOR_{SIZE,COUNT}` to report capacity, reads
  blocks 0..15 and checks the MBR signature, then asks for the
  eMMC's reported erase-block size.  Read-only.

- **GD32 bridge protocol vectors -- real CRC bytes (2026-05-13).**
  `gd32-bridge/tests/protocol_vectors.txt` is now populated with
  computed CRC-16/CCITT-FALSE values for every documented vector:
  CRC of `"123456789"` = `0x29B1` sanity check, SPI PING round-trip
  bytes, SPI `GET_VERSION` reply for firmware v0.1.0, I2C PING
  write + read.  Vectors are regenerated by
  `gd32-bridge/tests/gen_protocol_vectors.py` (idempotent;
  `--check` mode exits non-zero if the file drifts).

- **Twister coverage for `clk_5l35023b`, `murata_lbee5hy2fy`,
  `deepx_dxm1` (2026-05-13).**  `tests/zephyr/chips/src/main.c`
  picks up NULL-arg + post-init-rejection ZTESTs for all three
  V2N-family drivers added in the prior overnight run, plus the
  Murata "BT_DEV_WAKE not routed -> NOSUPPORT" contract test and
  the DEEPX out-of-range polarity rejection.  `prj.conf` opts the
  matching `CONFIG_ALP_SDK_CHIP_*` flags in.

- **`docs/test-plan.md` -- new + updated rows (2026-05-13).**
  Added rows for the GD32 SWD controller, the GD32 application
  bootloader's OTA opcode wiring, and the per-SoM hw-revision
  change-log enforcement.  Flipped `clk_5l35023b` + `tps628640`
  from `⏳ untested` to `🟡 partial` now that their failure-path
  ZTESTs + functional probe coverage are landed.

### Changed (2026-05-13 overnight run)

- **`metadata/schemas/soc-spec-v1.schema.json`** -- extended with
  optional `hwrm` (Hardware Reference Manual provenance), `errata`
  (list of vendor errata documents + per-item items), per-variant
  `notes` + `part_number_root`, and loosened the per-variant
  `package` field to optional (so a variant whose full order-code
  suffix is still pending the HW-config writeup can carry the part-
  number root without breaking validation).  Closes the 6 SoC
  schema failures `scripts/validate_metadata.py` was reporting.

- **`docs/gd32-bridge.md`** -- flashing table updated to mark the
  application-bootloader path + the SWD bit-bang path as
  "Scaffolded" (was "Planned"); source-layout tree adds the
  `bootloader/` directory + `gen_protocol_vectors.py`.

- **Customer-visible language** -- hw-rev CHANGELOGs +
  `hw-revisions-v1.schema.json` lose the "private companion repo"
  / "schematic-level annotations" phrasing in favour of the neutral
  "held outside this tree" framing.

### Added

- **`docs/gd32-bridge-protocol.md` — wire-protocol spec for the V2N supervisor MCU bridge (2026-05-12).**
  Frames + opcodes + CRC + status codes for the hybrid SPI / I2C
  bridge between the Renesas RZ/V2N and the on-module GigaDevice
  GD32G553MEY7TR.  Single command set shared between both
  transports (per memory rule `project_gd32_bridge_hybrid_spi_i2c`).
  SPI uses a SOF + CMD + payload + CRC-16/CCITT-FALSE envelope with
  two-transaction request/reply pattern; I2C reuses the same opcode
  + payload + CRC inside the standard reg-addr=0x00 write/read
  protocol so the bridge looks like a regular I2C slave to
  discovery tools.  §10 reserves OTA opcodes `0xF0..0xFF` for the
  planned application-bootloader path; §10 also documents the
  V2N→GD32 BOOT0 reroute the maintainer has committed to (pad
  selection TBD per `memory/project_gd32_boot0_to_v2n_planned.md`).

- **`chips/gd32g553/` host-side bridge driver (2026-05-12).**
  Renesas-side driver that speaks the bridge protocol over either
  SPI fast path (`P76/77/96/97` host  ↔  GD32 `PA8/9/10/PB15`) or
  I2C management path on BRD_I2C (Renesas `P07/P06` host ↔ GD32
  `PA15/PB9` at 7-bit address `0x70` by default).  Pass either bus
  (or both) to `gd32g553_init`; the driver picks the SPI fast path
  by default when both are present, and per-call `*_via` helpers
  override.  Full opcode coverage: `PING`, `GET_VERSION`,
  `GET_BUILD_ID`, `RESET_REASON`, `GPIO_READ` / `GPIO_WRITE`,
  `PWM_SET` / `PWM_GET`, `ADC_READ` (1..8 samples, little-endian
  millivolt encoding), `DA9292_STATUS_FORWARD`.  Refuses to operate
  on firmware whose `major` does not match
  `GD32G553_HOST_PROTOCOL_MAJOR` (0 today, before-tag).  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_GD32G553`.  Header: `<alp/chips/gd32g553.h>`.
  Manifest: `metadata/chips/gd32g553.yaml` (references the
  GigaDevice datasheet + user manual archived project's
  vendor datasheet ).

- **`gd32-bridge/` firmware tree scaffold (2026-05-12).**
  Separate compile artifact for the GD32G553 supervisor MCU.
  Top-level CMake + Arm-GCC toolchain config + linker-script
  placeholder + protocol layer + per-transport scaffolds
  (`transport_spi.c` + `transport_i2c.c`).  Single shared
  `protocol_dispatch()` in `src/protocol.c` handles both
  transports.  HAL backend selectable: `stub` (compiles standalone,
  every HW operation returns `BRIDGE_HW_ERR_NOTIMPL`) or `gd32`
  (TODO -- GigaDevice firmware library not yet vendored).
  `PING` / `GET_VERSION` / `GET_BUILD_ID` round-trip end-to-end
  on the stub backend so the wire layer is unit-testable before
  silicon-side firmware lands.  `tests/protocol_vectors.txt` is
  the shared CRC + wire vector file that the host-side driver
  tests and the firmware-side unit tests both consume.  See
  `docs/gd32-bridge.md` for the overview + flashing options.

- **`chips/rtl8211fdi/` Realtek Ethernet PHY driver (2026-05-12).**
  Callback-driven MDIO surface for the two RTL8211FDI-VD-CG PHYs
  populated on V2N (ET0 + ET1).  The driver takes
  `mdio_read` / `mdio_write` function pointers so it stays portable
  between Zephyr (callbacks wrap the Renesas-side
  `<zephyr/drivers/mdio.h>` controller) and baremetal (callbacks
  bit-bang or hit a vendor MAC's MMD register block).  Public API:
  probe + Realtek-OUI verify (`PHYID1 == 0x001C`), `soft_reset`,
  `restart_autoneg`, `get_link` (decodes the Realtek-extended
  page-`0xA43` reg-`0x1A` PHY-specific status), Wake-on-LAN
  magic-packet detection via the page-`0xD8A` register block, raw
  + paged register R/W escape hatches.  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_RTL8211FDI`.  Header:
  `<alp/chips/rtl8211fdi.h>`.  Manifest:
  `metadata/chips/rtl8211fdi.yaml`.

- **`chips/clk_5l35023b/` Renesas/IDT clock generator stub (2026-05-12).**
  Stub-status driver for the 5L35023B audio clock generator that
  sources `Audio_CLKB` on V2N (Renesas `P10` output, `P67` OE).
  Surface: init / probe (register-0 read) + raw register R/W +
  a `register_dump` path for production-test logging.  No
  frequency-config helpers until the maintainer adds the chip
  datasheet to the vendor datasheet and mirrors
  the Renesas RZ/V2N EVK BSP init sequence into the driver.
  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_CLK_5L35023B`.  Header:
  `<alp/chips/clk_5l35023b.h>`.  Manifest:
  `metadata/chips/clk_5l35023b.yaml`.

- **`chips/murata_lbee5hy2fy/` Wi-Fi/BLE module GPIO surface (2026-05-12).**
  Thin GPIO surface for the Murata LBEE5HY2FY-922 (Type 2FY,
  Infineon CYW55513 inside).  Owns the five side-channel lines:
  `BT_REG_ON` (GD32 `PE14`), `WL_REG_ON` (GD32 `PE15`),
  `BT_HOST_WAKE` (Renesas `P05`), `WL_HOST_WAKE` (Renesas `P72`),
  and `BT_DEV_WAKE` (TBD -- not currently routed on V2N).  The
  REG_ON lines live on the GD32 supervisor so the driver takes
  caller-supplied set / get callbacks for them (carriers route via
  `gd32g553_gpio_write` or a direct local-GPIO shim);
  HOST_WAKE / DEV_WAKE arrive as plain `alp_gpio_t` handles since
  they are host-SoC GPIOs.  Air-side traffic (Wi-Fi SDIO, BT UART,
  BT I2S) is the OS stack's responsibility (brcmfmac + BlueZ +
  ALSA on Linux); this driver is the power-and-wake glue around
  it.  New Kconfig: `CONFIG_ALP_SDK_CHIP_MURATA_LBEE5HY2FY`.
  Header: `<alp/chips/murata_lbee5hy2fy.h>`.  Manifest:
  `metadata/chips/murata_lbee5hy2fy.yaml`.

- **`chips/deepx_dxm1/` + `vendors/deepx-dxm1/` DEEPX bring-up sequencer (2026-05-12).**
  Host-side glue for the DEEPX DX-M1 NPU on V2N-M1 (NOT populated
  on V2N base).  Sequences M1_RESET (Renesas `PA6`) + the two
  passive PCIe muxes (via `chips/pi3dbs12212/`) before the Linux
  kernel driver (`dx_rt_npu_linux_driver` from upstream
  `github.com/DeepX-AI/`) opens the PCIe device.  `vendors/deepx-dxm1/README.md`
  carries the upstream-repo + Yocto layer cross-link.
  `deepx_dxm1_bring_up()` is the one-call API that runs the
  three-step rail / mux / reset sequence; rail bring-up itself is
  the caller's responsibility (composes `da9292_v2n_m1_enable_deepx_rail`
  + `tps628640_init` ACK-probes).  Reset polarity is assumed
  active-high by default with a `set_reset_polarity` flip for
  active-low boards.  Boot wait is approximate -- production
  callers pass `boot_us=0` and use their platform's busy-wait.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_DEEPX_DXM1` (selects
  `ALP_SDK_CHIP_PI3DBS12212` automatically).  Header:
  `<alp/chips/deepx_dxm1.h>`.  Manifest:
  `metadata/chips/deepx_dxm1.yaml`.

- **`docs/gd32-bridge.md` — firmware-tree overview (2026-05-12).**
  Companion to the protocol doc: source-layout map, build
  instructions (`cmake -B build -DCMAKE_TOOLCHAIN_FILE=...`), the
  three flashing options (SWD probe = supported today; in-system
  OTA = planned per protocol §10; V2N-driven factory ISP =
  pending BOOT0 routing).

- **`docs/pmic-rails.md` — rail map for V2N + V2N-M1 (2026-05-12).**
  ASCII diagram of the power chain (ACT88760 → DA9292 + TPS628640
  family).  Per-rail table with voltage / source / population
  scope.  Cross-reference to the authoritative power-sequence PDFs
  held in the vendor datasheet.
  Sequencing rules: V2N base needs no host
  intervention; V2N-M1 adds the DEEPX rail bring-up steps before
  releasing `M1_RESET`.  Lists the rails firmware MUST NOT
  modify in production (page-0 bucks on the primary PMIC,
  DA9292 CH1 VSET, CH2 on V2N base).

- **`docs/bring-up-v2n.md` + `docs/bring-up-v2n-m1.md` — bench bring-up guides (2026-05-12).**
  Step-by-step procedures: first-power smoke test, SWD attach +
  GD32 firmware flash, host ↔ bridge link confirmation, SoM
  manifest read, dual Ethernet PHY bring-up, on-module fleet sanity
  checks, and the most likely gotchas.  The V2N-M1 doc covers the
  delta (DEEPX rails + M1_RESET sequencing + DEEPX kernel hand-off).

- **`docs/board-id.md` — SoM identification flow (2026-05-12).**
  Two-stage design: EEPROM manifest (working today via
  `alp_hw_info_read()`) + BOARD_ID ADC cross-check (stubbed pending
  the `scripts/alp_project.py` per-family generated header).  Walks
  through the manifest layout in `<alp/hw_info.h>`, the
  `tools/program_eeprom.py` programming flow, and the runtime
  read path implemented in `src/zephyr/hw_info_zephyr.c`.

- **`docs/hil-plan.md` — HiL rig requirements (2026-05-12).**
  Document of the rig the maintainer will build (not built here).
  Hardware inventory (SoM, probes, programmable supply, logic
  analyser, link partners), test coverage map (power tree, GD32
  bridge, Ethernet, Wi-Fi/BT, DEEPX), orchestrator design
  (Python + sigrok + gdbserver), and a rough schedule (~2 months
  of one engineer's time once hardware is on the bench).

- **`docs/ota-device-contract.md` — device-side OTA contract (2026-05-12).**
  Mender contract for the main system OTA (Renesas-side, Linux
  rootfs A/B slots, OPTIGA-mediated signature verification).
  Server-side stays Hakan's repo per the existing
  `project_ota_server_owner` memory.  Planned design for the GD32
  bridge firmware OTA (opcodes `0xF0..0xFF`, dual-slot flash
  layout, ECDSA-P256 signature with key baked into the bootloader
  until cross-bus OPTIGA access is wired).  Failure-recovery
  matrix covering every "what if the upgrade fails halfway"
  scenario across both layers.

- **`alp_hw_info_read` EEPROM-side implementation (2026-05-12).**
  Replaces the NOSUPPORT stub in `src/zephyr/hw_info_zephyr.c` with
  a working EEPROM-manifest reader.  Opens the configured I2C bus
  (`CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID`), reads 128 bytes via
  the 24C128 driver at `CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT` +
  `CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET`, validates the magic byte
  (`ALPH`), schema version, and CRC32 (ISO-3309 / `zlib.crc32`-
  compatible, matching `tools/program_eeprom.py`), then copies the
  SoM identifiers into `alp_hw_info_t`.  When the bus id Kconfig is
  `-1` (default) the reader returns `ALP_ERR_NOSUPPORT` -- boards
  that haven't wired the EEPROM yet aren't penalised.
  `alp_hw_info_assert_matches_build` does strncmp checks against
  `expected_sku` / `expected_hw_rev` (NULL = skip that field) with
  the same NOSUPPORT graceful-fallback policy.
  New Kconfig options under `CONFIG_ALP_SDK_HW_INFO`:
    `ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` (int, default -1)
    `ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT` (hex, range 0x50..0x57)
    `ALP_SDK_HW_INFO_EEPROM_OFFSET` (int, default 0)
    `ALP_SDK_HW_INFO_EEPROM_I2C_BITRATE_HZ` (int, default 400000)
  BOARD_ID ADC cross-check remains a no-op stub (`adc_cross_check`)
  pending the per-family generated header that maps `hw_rev` strings
  to expected mV bins (depends on `scripts/alp_project.py` emitting
  a runtime-readable digest of `metadata/e1m_modules/<family>/hw-revisions.yaml`).

- **`chips/pi3dbs12212/` Diodes PI3DBS12212A PCIe mux driver (2026-05-12).**
  GPIO-only control surface for the two passive 12 Gbps
  differential muxes that switch the V2N-M1 PCIe lane between
  the on-module DEEPX DX-M1 NPU and the E1M edge connector.  PD
  pin on Renesas `P80`, SEL pin on `P95`.  Three-state driver
  model (OFF / PATH_0 / PATH_1) with glitch-free PD pulse on
  path-to-path transitions.  Takes two `alp_gpio_t` handles --
  carrier code opens + configures the GPIOs and hands them in.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_PI3DBS12212`.  Header:
  `<alp/chips/pi3dbs12212.h>`.  Manifest:
  `metadata/chips/pi3dbs12212.yaml`.

- **`chips/rv3028c7/`: multi-source event dispatcher + CLKOUT routing (2026-05-12).**
  Extends the existing RV-3028-C7 driver from single-alarm-only to
  the full latched-event surface in the chip's STATUS register
  (PORF, EVF, AF, TF, UF, BSF, CLKF -- all 7 sources).  New API:
    - `rv3028c7_register_handler(src, cb, user)` per-source dispatch.
    - `rv3028c7_dispatch_irq(status_seen)` reads STATUS, invokes
      registered handlers, write-0-to-clears every fired flag.
    - `rv3028c7_set_int_enable(src, enable)` masks individual sources
      in CONTROL_2 (EIE/AIE/TIE/UIE/CLKIE) and EEPROM_BACKUP (BSIE
      for the backup-switchover source), guarded by the EERD
      auto-refresh pause + restore protocol.
    - `rv3028c7_route_clkout(src)` reprograms the CLKOUT pin via
      EEPROM_CLKOUT (0x35[2:0]) so carriers that wire CLKOUT as a
      **second physical interrupt line** can route a specific event
      independent of INT.  Eight CLKOUT sources: 32.768 kHz, 8192 Hz,
      1024 Hz, 64 Hz, 32 Hz, 1 Hz, periodic-update, low.
  Reference: Micro Crystal "Multiple Interrupt Lines with
  RV-3028-C7" application note.  Existing alarm helpers
  (`rv3028c7_set_alarm`, `rv3028c7_alarm_int_enable`,
  `rv3028c7_alarm_check_and_clear`) continue to work unchanged --
  they bypass the dispatcher and operate on the AF status bit
  directly.  Header: `<alp/chips/rv3028c7.h>`.

- **`chips/tps628640/` TI TPS628640 multi-instance buck stub (2026-05-12).**
  Multi-instance scaffold for the four populated TPS628640 buck
  regulators on V2N-M1's BRD_I2C bus (`0x48` = 0.85 V VDD0V85_LPDDR
  DEEPX, `0x44` = 1.05 V DDR5_VDD DEEPX, `0x4F` = 0.5 V DDR5_VDDQ_0V5
  DEEPX, `0x4D` = 0.6 V LPD4x_0V6 Renesas LPDDR4X V2N-common optional).
  Driver lands as **stub** because the TI datasheet isn't in the
  vendor datasheet yet -- only probe + raw register R/W
  works; `tps628640_set_voltage_mv` / `_get_voltage_mv` /
  `_get_status` all return `ALP_ERR_NOSUPPORT`.  Each instance
  carries its design-target voltage in `ctx->default_voltage_mv`
  as metadata for higher-level code that wants to enforce safe-
  operating-window guards once the register map is filled in.
  Carriers can rely on the chip's factory OTP voltage in the
  meantime -- the rails self-regulate without host intervention.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_TPS628640`.  Header:
  `<alp/chips/tps628640.h>`.  Manifest:
  `metadata/chips/tps628640.yaml`.

- **`chips/da9292/` Renesas DA9292 secondary PMIC driver (2026-05-12).**
  Full I2C surface for the **Renesas DA9292-AROVx** multi-phase buck
  PMIC populated on V2N + V2N-M1 at 7-bit address `0x1C`.  Strapped
  for 2-channel dual-phase mode by the CONF pin: **CH1** (phases
  1+2) is the 0.8 V Renesas rail (enabled at boot via the EN1 hard-
  strap), **CH2** (phases 3+4) is the 0.75 V DEEPX rail (disabled on
  V2N base, brought up by V2N-M1 firmware before DEEPX boot).
  Public API: probe + cached DEV_ID/REV_ID; live status decode
  (PG / UV / OV / OC / TEMP_WARN / TEMP_CRIT / VIN_UVLO); event
  read-and-clear over `PMC_EVENT_00/01` (write-1-to-clear); 5 mV-
  step voltage set/get in the `0.3..1.275 V` (VSTEP=0) range;
  per-channel enable in `PMC_CTRL_01`; and the variant-specific
  init helpers `da9292_v2n_base_init` + `da9292_v2n_m1_enable_deepx_rail`
  (the latter programs 0.75 V, reads back, enables CH2, polls
  CH2_PG with a caller-supplied timeout).  PMC_STATUS bit layout
  is assumed to mirror PMC_MASK_00 (which IS documented in the
  datasheet); flagged TODO in `chips/da9292/da9292.c` for verify
  against datasheet Table 14 before relying in production.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_DA9292`.  Header:
  `<alp/chips/da9292.h>`.  Manifest: `metadata/chips/da9292.yaml`.

- **`chips/act8760/` ACT88760 primary PMIC driver scaffolding (2026-05-12).**
  Stub-status driver for the **Qorvo ACT88760-120.E1** primary PMIC
  populated on the V2N + V2N-M1 SoMs.  Dual-page I2C interface
  (`0x25` system / Buck1..Buck6 / GPIOs; `0x26` Buck7 / LDO1..LDO6)
  on BRD_I2C.  `act8760_init` probes both pages, `act8760_get_status`
  decodes the TWARN / SYSWARN / SYSDAT / ILIM bits from the
  system-status register (read-to-clear nIRQ semantics), and raw
  register R/W is exposed on both pages.  Per-rail VSET access
  is wired but returns `ALP_ERR_NOSUPPORT` until the VSET register
  offsets are confirmed against the Users Guide Rev 3.0 table -- a
  follow-up will replace the TODO offsets in `chips/act8760/act8760.c`.
  CMI 120.E1 burns the production power-tree (rail voltages, GPIO
  assignments, power-on sequence) into the chip's non-volatile
  memory via Qorvo's ActiveCiPS dongle from the `.iact` profile
  archived vendor datasheet;
  runtime I2C is volatile-only and used here for telemetry + DVS.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_ACT8760` (off by default).
  Header: `<alp/chips/act8760.h>`.  Manifest:
  `metadata/chips/act8760.yaml` (introduces the `metadata/chips/`
  directory; per-chip manifests for the other V2N populated parts
  land alongside their drivers).

- **Renesas Ethernet + eMMC + uSD + xSPI NOR-flash pin assignments (2026-05-11).**
  Maintainer supplied four additional schematic excerpts.  63 new
  rows added to `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`:
  - **30 Ethernet rows** — RGMII to two on-module
    `RTL8211FDI-VD-CG` PHYs (ET0 + ET1).  Each PHY: 15 pins
    (TX_CTL/CLK + 4× TXD, RX_CTL/CLK + 4× RXD, MDIO/MDC,
    PHY_INTR).  22 Ω series resistors documented per data line;
    MDIO/MDC pull-ups to VDD_1V8 via 1 kΩ 1%; layout constraints
    (single 50 Ω ±10%, GND guard trace on TX/RX clocks; 5 mm
    length matching on RX_CTL relative to RX_CLK) preserved in
    the notes column.
  - **11 eMMC rows** — `SD0CLK` / `SD0CMD` / `SD0DAT0..7` /
    `SD0RSTN` for the on-module eMMC.  22 Ω series on each line,
    1 MΩ pull-up to `VDD_eMMC_3V3_1V8`, ±1.27 mm length matching
    on `eMMC_CLK`.
  - **6 uSD-card rows** — `SD1CLK` / `SD1CMD` / `SD1DAT0..3` for
    the on-module microSD card slot.  22 Ω series, 10 kΩ pull-ups
    to `µSD1_1833V` (1.8 V), ESD protection via `PUSB3FR6Z` (D28).
  - **16 xSPI rows** — `XSPI0_*` to the on-module xSPI NOR flash
    (CK ± / CS / 8× IO / DS / ECS / INT / RSTO / RESET).  22 Ω
    series on CK + IO0..IO3 (x4 data path), 10 kΩ pull-downs on
    IO4..IO7 + DS, 10 kΩ pull-ups on ECS / INT / RSTO to 0V.
    Series resistor IDs (R143..R376) preserved in notes for
    board-engineering cross-reference.

  These pads use **BGA designators** (J26, AC27, AJ18, etc.)
  rather than port-pin names because they're dedicated-function
  pads outside the GPIO bank.  The `renesas_pad` column carries
  either convention depending on the pin -- header comment block
  documents the dual-convention explicitly.  Three of the new
  pads happen to start with `P` followed by digits (P24, P25, P29
  on ET1) -- those are BGA pads, not port pins; the peripheral
  column makes the distinction unambiguous.

  Renesas peripheral map now totals **146 data rows** (was 83);
  CSV regenerated.

  Pad-uniqueness check after the additions: **all 146 Renesas pads
  are unique**.  No collisions introduced by the new rows.

### Changed

- **Renesas PWM pinout corrected; PWM6/PWM7 added; BRD_I2C master/slave roles clarified (2026-05-11).**
  Maintainer supplied the authoritative Renesas-side PWM pin list:
  - E1M PWM0: P36 -> **P64** (peripheral unchanged: GPT13_GTIOC13A)
  - E1M PWM1: P37 -> **P65** (peripheral unchanged: GPT13_GTIOC13B)
  - E1M PWM2..PWM5: unchanged (GPT0/GPT4 channels on P70/P71/P74/P75)
  - E1M PWM6: NEW row, GPT15_GTIOC15A on P36
  - E1M PWM7: NEW row, GPT15_GTIOC15B on P37

  P36 and P37 stay in use but switch role (was PWM0/PWM1 via
  GPT13; now PWM6/PWM7 via GPT15).  GPT15 was not previously
  mapped to a Renesas pad in the saved metadata.

  PWM4's Renesas-side pad (P74) preserved from the prior pinout
  because the maintainer's correction msg listed
  `PWM4 = GPT4_GTIOC4A` without an explicit pin.  P74 is adjacent
  to PWM5's confirmed P75; flagged inline pending re-confirmation.

  v2n/README.md "Cross-chip PWM" section reworked into a
  per-PWM table showing GD32 + Renesas pad for each of the eight
  channels.  Design intent recorded: pick **either GD32 or
  Renesas for all eight PWMs**, not per-channel -- the resistor
  mod is a SoM-wide source selection.

  Separately: BRD_I2C master/slave roles on the bus clarified.
  Renesas (RIIC8 on P06/P07) is the **master**; GD32 (PB9/PA15)
  is a **slave** on the shared bus alongside the RTC, OPTIGA,
  EEPROM, clock generator, and TMP112.  Captured in both files'
  notes columns.

  Renesas CSV regenerated (83 data rows, was 81).

- **GD32 pinout — schematic labels dropped; E1M signal names canonical (2026-05-11).**
  Maintainer reviewed the row-numbered CSV and pushed back on
  carrying the misleading `GD32_P<pad>_R` schematic labels as the
  peripheral name: "we should just use the pin names with E1M
  signal definitions, not the wrong signal names we mixed".
  Applied:
  - Every GPIO route's peripheral name flipped from
    `GD32_P<pad>_R` to its E1M IO destination
    (`GD32_PB3_R` → `E1M IO14`, `GD32_PC3_R` → `E1M IO24`, etc.).
    18 GPIO routes now named uniformly by E1M IO number.
  - The `GD32_I2C2.*` peripheral names flipped to `E1M I2C3.*`
    -- they're E1M-edge signals, not internal-only.
  - Maintainer's CSV correction pass merged into the canonical
    TSV: PB7 collision removed (the `GD32_PB7_R` row was a
    duplicate of `GD32_PB3_R`'s actual pad; the duplicate is
    deleted); PC14 collision removed (`GD32_PC14_R` was actually
    on PC15, not PC14); PB11 / PD1 / PD2 / PD8 / PD10 / PC15 /
    PC2 GPIO-route pad assignments rotated to the correct
    silicon pads per the maintainer's row-by-row audit.
  - Inline `[SDK note: function-A-vs-function-B pad mismatch]`
    workaround text removed everywhere -- with the schematic
    labels dropped the workaround is moot.
  - `v2n/README.md` "Function-A vs function-B pin labelling"
    section deleted (no longer applies); "Jumper-selectable
    shared pads (PB7, PC14)" section deleted (no longer applies,
    every pad in the TSV is unique).  Replaced with a "Dropped
    schematic labels" section explaining the design decision.
  - Two E1M destinations from the original spreadsheet (`IO15`
    and `IO26`) are no longer routed on V2N -- they were entries
    on schematic nets that the audit pass found to be either
    duplicates of other entries (PB7 case) or off-by-one labels
    for rows that already covered different destinations (PC15
    case).  Pre-2026-05-11 git history retains the original
    labels for board-engineering cross-reference.
  - CSV regenerated from the canonical TSV; row numbers are now
    contiguous (62 data rows, was 64 with two now-deleted rows
    consolidated by the maintainer's audit).
  - All 62 GD32 silicon pads in the file are unique.
- **GD32 pinout — final corrections + CSV companions (user-supplied, 2026-05-11).**
  Three more user-supplied fixes after the format-consistency
  pass:
  - `E1M ENC0_X` notes: dropped the bogus `SPI1_MISO` alt-function
    annotation (PA0 has no SPI1_MISO function on the GD32G5xx);
    notes now just say `TIMER1_CH0` (the encoder timer alt).
  - `E1M CAN_STBY` pad: filled in `PB13` (was `(unspecified)`).
  - `ADC1` / `ADC5` / `ADC6` / `ADC7` confirmed E1M-edge and given
    the `E1M ` prefix (alongside the already-prefixed
    ADC0/2/3/4); same prefix normalisation applied to `DAC0` /
    `DAC1` for consistency.

  Plus a broader naming-format normalisation pass:
  - **E1M-edge signals all carry `E1M ` on the left** of the
    peripheral column (E1M PWM0..PWM7, E1M ADC0..ADC7, E1M DAC0/1,
    E1M ENC0..3 X/Y, E1M CAN_STBY).  The source spreadsheet
    inconsistently prefixed some and not others; now uniform.
  - `BT_REG_ON` / `WL_REG_ON` peripheral names cleaned (the
    `LBEE5HY2FY-922` chip context moved to the notes column where
    it belongs).
  - `GD_I2C2.*` renamed to `GD32_I2C2.*` for consistency with the
    other `GD32_*`-prefixed signals.
  - Sort order tidied: debug → SPI → I²C → encoders → PWMs →
    ADCs → DACs → CAN → camera → wireless → secure-element →
    GPIO routes.

  All-eight-PWM dual-sourcing recorded:
  - User confirmed 2026-05-11 that **all eight PWMs** (PWM0..PWM7)
    are dual-sourced between Renesas and GD32 via carrier-side
    resistor mod (was previously documented as just PWM0..PWM5).
    Renesas-side pads for PWM6 and PWM7 aren't in the saved
    Renesas peripheral map yet — flagged in `v2n/README.md` as
    pending an updated Renesas pinout writeup.

  Carrier-mux on PB7 / PC14 (ROW B interpretation) confirmed:
  - User confirmed 2026-05-11 that the two pads each carry two
    E1M IO destinations selected by passive carrier jumpers:
    PB7 picks between E1M IO14 (via `GD32_PB3_R`) and E1M IO15
    (via `GD32_PB7_R`); PC14 picks between E1M IO24 (via
    `GD32_PC3_R`) and E1M IO25 (via `GD32_PC14_R`).  Documented
    in the new `Jumper-selectable shared pads` section of
    `v2n/README.md`; inline TSV notes updated from "pending user
    confirmation" to "user-confirmed".

### Added

- **Row-numbered CSV companions for each pin map.**  Generated
  via inline `awk` from the canonical TSVs so the user can
  audit each row in a spreadsheet without losing the source
  TSV semantics.  Three new files:
  - `metadata/e1m_modules/v2n/renesas-peripheral-map.csv`
    (81 data rows)
  - `metadata/e1m_modules/v2n/gd32-io-mcu-map.csv` (64 data rows)
  - `metadata/e1m_modules/v2n-m1/m1-additions.csv` (3 data rows)

  Each carries `row, peripheral, <silicon_pad>, notes` columns
  with the `row` value matching the user-facing row index for
  cross-referencing during review.  Both READMEs note that the
  CSVs are derived artefacts: edit the TSV, not the CSV.

### Changed

- **GD32 pinout pad corrections (user-supplied, 2026-05-11).**
  Six fixes to `gd32-io-mcu-map.tsv` after the user audit:
  - `PWM0` channel: `TIMER0_MCH2` → **`TIMER0_MCH0`** on PA11.
    The source-spreadsheet had PWM0 + PWM1 both on MCH2 which
    is impossible (one channel = one output); user confirms PWM0
    is on MCH0.
  - `GD32_PB3_R` actual pad: PB3 → **PB7**.  Function-A label
    retained ("PB3"); function-B alternate pad PB7 is where the
    schematic actually routes.
  - `GD32_PC3_R` actual pad: PC3 → **PC14**.
  - `GD32_PA15_R` actual pad: PA15 → **PC1**.
  - `GD32_PB9_R` actual pad: PB9 → **PC0**.
  - `GD32_PE13_R` actual pad: PE13 → **PE12**.
  Resolves the five same-pad function-A vs function-B conflicts
  flagged in the previous audit (PB3 / PC3 / PA15 / PB9 / PE13
  now cleanly owned by ENC0_Y / CAM_EN_LDO0 / BRD_I2C.SCL /
  BRD_I2C.SDA / E1M ADC2 respectively).  The TSV header now
  documents the function-A-label-on-function-B-pad convention so
  consumers don't mistake the schematic name for the silicon pad.
  v2n/README.md gains a new "Function-A vs function-B pin
  labelling" section.
- **Cross-chip PWM mux documented as design intent.**  The earlier
  audit flagged E1M PWM3..5 as a Renesas-vs-GD32 conflict.  User
  confirmed (2026-05-11) that PWM0..PWM5 are **dual-sourced by
  design** with carrier-side resistor selection; **GD32 is the
  default source**; PWM6/PWM7 are GD32-only.  v2n/README.md
  gains a "Cross-chip PWM (PWM0..PWM5)" subsection capturing
  this.  Same section notes the source-spreadsheet's inconsistent
  use of the `E1M ` prefix on PWM0/1/2 (and ADC1/5/6/7) — all are
  presumed E1M-edge; ADC prefix-inconsistency flagged as pending
  user confirmation.
- **Restored verbatim user-source notes in `gd32-io-mcu-map.tsv`.**
  Earlier ingest had paraphrased the source spreadsheet's
  annotations (e.g. "connected to Camera enable pin LDO 0" →
  "Camera enable — LDO 0"; "reset pin of SLS32AIA010MLUSON10XTMA2"
  → "Reset for SLS32AIA010MLUSON10XTMA2 (OPTIGA Trust M secure
  element)").  Following the convention used for the Renesas
  peripheral map, the TSV now carries the user's exact wording as
  the third column; SDK-added context is bracketed
  `[SDK note: ...]` so it's distinguishable from source data.
  Same intent, more faithful preservation.

### Added

- **Errata items ingested from `AERR0012` v2.0.**
  `metadata/socs/alif/ensemble/{e4,e6,e8}.json` `errata.items[]`
  arrays now carry the four documented entries (ER001 — RTC
  pre-scaler reset bug on A0, fixed in A1; ER002 — RTC drift
  during POR_N on A1, no fix planned; ER003 — STOP-mode current
  ~2 µA over spec on A0, fixed in A1; ER004 — external reset
  supervisor required on all revisions when the cold-boot supply
  ramp can't meet the monotonic-rise-above-1.65 V invariant).
  Each item records affected revisions, resolution status,
  consequence, and workaround.  `title_quirk` field documents the
  source-PDF title typo (says "E2, E4, E8" but scope is
  "E4, E6, E8" — E2 is not an Ensemble part).
- **V2N-M1 README now explicit about GD32 inheritance.**  The
  v2n-m1 directory's "Inheritance" section previously named only
  the Renesas-side base map; now it explicitly notes that the
  GD32 IO MCU map (`v2n/gd32-io-mcu-map.tsv`) is also inherited
  verbatim and lists the GD32's role unchanged on V2N-M1 builds
  (encoder capture, extra PWM/ADC/DAC, camera-LDO enables,
  Wi-Fi/BT REG_ON, OPTIGA reset, E1M-edge GPIO routes).  The
  three-file concatenation recipe to build the full V2N-M1 map
  is now spelled out in the README.
- **GD32G553 companion IO MCU pinout for V2N + V2N-M1.**
  `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — authoritative
  pinout for the GigaDevice GD32G553MEY7TR that sits next to the
  RZ/V2N as a companion.  Covers encoder capture (ENC0..3 X/Y on
  TIMERs 1-4), extra PWM fan-out (PWM3..7 on TIMER7), the bulk of
  the ADC bank (ADC0..7), DAC0/1, the four camera-enable LDOs,
  the Murata LBEE5HY2FY-922 BT_REG_ON / WL_REG_ON, the OPTIGA
  Trust M reset, the SPI1 slave interface to Renesas, BRD_I2C
  (shared with Renesas), GD_I2C2 to V2N, plus 17 general-purpose
  GPIO routes to the E1M edge IO bank (IO8..IO35).  v2n/README.md
  gets a new "The two-chip pin story" section explaining the
  Renesas-vs-GD32 split.  V2N-M1 inherits this file unchanged
  (GD32's role doesn't depend on whether DEEPX is populated).
- **Alif Ensemble metadata refresh against new datasheets + HWRMs.**
  Datasheet inventory landed in the vendor datasheet for
  the AEN family on 2026-05-11 (E3 v2.8 already on file; E4 v1.0
  new; E6 v1.0 new; E7 v2.9 already; E8 v1.0 supersedes v0.51
  preliminary; confidential HWRMs v0.3 for E4/E6/E8; public HWRMs
  for E3 v2.5 + E7 v2.6; errata AERR0012 v2.0 covering E4/E6/E8
  jointly).  Metadata updates:
  - **`e4.json`, `e6.json`, `e8.json`** flip from
    `status: "preliminary"` to `"released"`.  E4 and E6 drop the
    `pending_alif_datasheet: true` flag; E8 supersedes the v0.51
    `datasheet` block with v1.0.  Per-peripheral counts intentionally
    NOT re-ingested in this pass -- re-ingestion against v1.0 is
    tracked as a separate v0.4 task.
  - **All five files with HWRMs** (E3, E4, E6, E7, E8) gain a new
    `hwrm` block alongside `datasheet`, with `confidential: true`
    on E4/E6/E8 so the HWRM files stay out of git.
  - **E4 / E6 / E8** gain a new `errata` block pointing at
    AERR0012 v2.0 with an empty `items: []` list pending
    per-item ingest.
  - **`e5.json`** stays preliminary -- E5 alone has no Alif
    datasheet yet.  Notes refreshed to clarify that the
    other five E-class parts are now released.
- **i.MX 93 orderable variant resolved to i.MX 9352** in
  `metadata/socs/nxp/imx9/imx93.json`.  The empty `variants: []`
  list gains a single entry `MIMX9352xxxxM` (the four-character
  placeholder reflects that the full order-code suffix -- package
  + temperature + qualification -- is still pending the HW-config
  writeup).  Per-peripheral counts still empty pending a dedicated
  ingest pass against IMX93RM.pdf (5593 pages).  `PLAN.md §6`
  items 6 and 7 updated to reflect partial closure.
- **Authoritative V2N + V2N-M1 Renesas-side peripheral pinout.**
  The alp-sdk maintainer provided the canonical RZ/V2N pin
  assignments for the E1M-X V2N family on 2026-05-11.  Saved as:
  `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` — base V2N
  map (84 routed pads covering E1M-edge peripherals + on-module
  Wi-Fi/BT module + GD32 IO MCU + RTC + EEPROM + OPTIGA + clock
  generator + BRD_I2C bus); `metadata/e1m_modules/v2n-m1/m1-additions.tsv`
  — V2N-M1 overlay (DEEPX `M1_RESET` on `PA6`, `PCIe.MUX_PD` on
  `P80`, `PCIe.MUX_SEL` on `P95`).  Both directories now carry
  READMEs explaining the inheritance pattern (V2N-M1 = V2N base +
  M1 overlay + DEEPX-rail PMICs on BRD_I2C).  `hw-revisions.yaml`
  for both families walks back the "pinout TBD" framing and lists
  the authoritative populated parts; revision label + board_id ADC
  channel remain TBD pending a separate writeup.  Resolves the
  project memory note "pending exact hardware configurations" for
  the V2N + V2N-M1 families (AEN was already resolved on 2026-05-10;
  i.MX 93 is still pending).
- **From-scratch verifiability surface.**  Three artefacts answer
  "how do I prove everything works from a fresh clone":
  `scripts/bootstrap.sh` (one-time setup -- creates the Zephyr
  workspace, installs Python deps, prints apt/brew commands for
  the optional native libs), `scripts/test-all.sh` (single-command
  local verifier covering Yocto ctest + baremetal build + Zephyr
  twister + clang-format diff + metadata validate + Doxygen
  zero-warnings; with `--quick`, `--yocto-only`, `--zephyr-only`,
  `--no-clean` flags), and `docs/testing.md` (full coverage map
  per `<alp/...>` header, CI ↔ local correspondence table,
  per-feature verification policy).  README gains a "Test it from
  scratch" section pointing at the new flow.  No SDK code touched;
  documentation + tooling only.
- **Yocto `<alp/security.h>` backend via OpenSSL.**  New
  `src/yocto/security_yocto.c` binds the full `<alp/security.h>`
  surface (`alp_hash_*` for SHA-256/384/512, `alp_aead_*` for
  AES-128-GCM / AES-256-GCM / ChaCha20-Poly1305, `alp_random_bytes`)
  against OpenSSL's `EVP_*` API.  OpenSSL is the same TLS runtime
  libmosquitto links against on a stock Yocto image, so the two
  paths share entropy + CA bundle + algorithm implementation.
  Pulled in via `pkg_check_modules(libssl libcrypto)`; absent
  OpenSSL on the sysroot the backend falls through to the
  NOSUPPORT stubs in `src/common/stub_backend.c` (now gated by a
  new `ALP_VENDOR_OVERRIDES_SECURITY` macro alongside
  `_AUDIO_IN` / `_AUDIO_OUT`).  Tag-mismatch on decrypt is mapped
  to `ALP_ERR_IO` per the header contract.  AEAD key material
  cleared via `OPENSSL_cleanse` on close.  Coverage at
  `tests/yocto/security_openssl.c` (16 tests including a
  SHA-256 KAT against the NIST `"abc"` vector, full AEAD
  round-trip on AES-128-GCM + ChaCha20-Poly1305, tag-mismatch
  detection, key-length / NULL-key / unsupported-alg refusals,
  TRNG fill + null-arg).  `pr-plain-cmake` runner gains
  `libssl-dev` in the install step.  Marked 🟡 partial pending
  meta-alp's real Yocto image build.
- **Yocto audio backend via ALSA libasound.**  New
  `src/yocto/audio_yocto.c` binds the full `<alp/audio.h>` surface
  (`alp_audio_in_*` + `alp_audio_out_*`) against ALSA's `snd_pcm_*`
  API.  Device naming convention: `peripheral_id == 0` -> ALSA's
  `"default"` PCM (honours `/etc/asound.conf` + `~/.asoundrc`);
  `peripheral_id == N` -> `"hw:N-1,0"` (card N-1, device 0).
  Format mapping: `ALP_AUDIO_FMT_S16_LE` / `S24_LE` / `S32_LE` map to
  the matching ALSA constants (S24 is the 32-bit-container variant
  per the alp/audio.h "packed in 32-bit slots" semantics).
  `alp_audio_out_set_volume` applies a software linear scale during
  `alp_audio_out_write` (S16_LE path only for v0.4 prep; S24/S32
  pass through unmodified) so apps don't have to drive ALSA's
  separate mixer API.  Two new pool sizes:
  `ALP_SDK_YOCTO_MAX_AUDIO_IN_HANDLES` /
  `ALP_SDK_YOCTO_MAX_AUDIO_OUT_HANDLES` (default 2 each).  Built
  only when CMake's `pkg_check_modules` finds `alsa` (Debian/Ubuntu
  `libasound2-dev`; Yocto `alsa-lib` recipe); absent, the Yocto
  backend falls through to the NOSUPPORT stubs.  New
  `ALP_VENDOR_OVERRIDES_AUDIO_IN` / `_OUT` gates in
  `src/common/stub_backend.c` (the previous unconditional stubs got
  `z_last_error` stamping added at the same time so the
  `alp_last_error()` contract holds even on NOSUPPORT-only builds).
  Coverage at `tests/yocto/audio_alsa.c` (11 failure-path tests
  covering NULL cfg, invalid format, unreachable device, NULL-handle
  start/stop/read/write/set_volume, close-NULL safety).
  `pr-plain-cmake` runner gains `libasound2-dev` in the install
  step.  Real capture/playback parked behind `hil-yocto`.
- **`examples/uart-rx-ringbuf/` reference app.**  Hand-written
  example exercising the Phase 1 LwRB-backed RX ring buffer on
  ALP_E1M_UART0: attach with a caller-owned backing store, sleep
  to emulate "doing real work", pop batched bytes, detach.
  Heavily commented (~50% comment ratio per the examples-as-
  documentation convention) -- explains the producer/consumer
  split, when to reach for the ringbuf vs the classic
  `alp_uart_read`, and the backing-store sizing formula
  (`baud_rate / 10 * worst_case_drain_latency_s`).  New
  `alp_sdk.example.uart_rx_ringbuf` twister scenario runs the
  happy path under native_sim; the `test-plan.md` row for the
  LwRB feature now references both the failure-path ZTESTs and
  this example.
- **Yocto MQTT TLS (`mqtts://`) via libmosquitto.**  The Yocto IoT
  backend (`src/yocto/iot_yocto.c`) now accepts `mqtts://host[:port]`
  broker URIs (default port 8883) and routes them through
  `mosquitto_tls_set` + `mosquitto_tls_insecure_set` (OpenSSL
  underneath on a stock Yocto image).  New
  `alp_mqtt_tls_config_t` in `<alp/iot.h>`: optional `ca_file` /
  `cert_file` / `key_file` paths plus an `insecure` flag for dev
  testing.  `alp_mqtt_config_t` gains an optional `tls` pointer
  (NULL = use OS default CA path, no client cert -- the production
  path pins `ca_file`).  Default CA path is `/etc/ssl/certs`
  (override at compile time via `ALP_SDK_YOCTO_DEFAULT_CA_PATH`).
  TLS config errors surface at `alp_mqtt_open()` time rather than
  later at connect, so a misconfigured CA bundle is attributable.
  Existing `mqtt://` callers are unaffected -- the `tls` field is
  appended to the public struct and defaults to NULL under
  designated-initializer usage.  Updated `tests/yocto/iot_mqtt.c`:
  the previous NOSUPPORT assertion is replaced with five new TLS
  tests covering default-TLS open / pinned-CA open / missing-CA
  refusal / insecure-flag accepted / default-port-8883 parsing.
  Broker handshake roundtrip parked behind `hil-yocto`.
- **Mender OTA wiring on meta-alp (v0.4 prep).**  New opt-in
  distro include at
  `yocto/meta-alp/conf/distro/include/mender.inc` configuring
  Mender's `mender-full` class with the v0.4 reference storage
  layout (block-device A/B rootfs, 256 MiB per slot by default,
  32 MiB boot + 256 MiB data).  Machine .conf for `e1m-x-v2n`
  and `e1m-n93` gain commented opt-in hook blocks; consumers
  uncomment the `require conf/distro/include/mender.inc` line
  in their target machine to enable.  `e1m-x-v2n-m1` inherits
  via `require conf/machine/e1m-x-v2n.conf`.  `meta-mender-core`
  added to `LAYERRECOMMENDS_alp` -- builds that don't ship OTA
  drop it cleanly from `bblayers.conf`.  README gains a "OTA
  via Mender (v0.4 prep)" section with the full enablement
  walk-through (server / tenant override, artefact paths).
  New cross-cutting `docs/ota.md` covers the trust model + the
  Yocto Mender flow + the AEN-Zephyr Mender vs Hawkbit decision
  pending v0.4-final.  Three `docs/test-plan.md` rows flip from
  TBD to ⏳ untested with concrete artefact pointers (AEN-Zephyr,
  V2N-Yocto, i.MX 93-Yocto).  No code changes; scaffolding +
  documentation only.  HIL roundtrip parked behind `hil-yocto` +
  the Zephyr-client decision.
- **MCUboot secure-boot scaffolding for AEN-Zephyr (v0.4 prep).**
  New sysbuild profile at `sysbuild/aen/sysbuild.conf` (MCUboot +
  ECDSA-P256 + swap-using-scratch).  New `keys/` directory with
  a `.gitignore`'d dev-key slot, a `generate_dev_key.sh` wrapper
  around Zephyr's `imgtool`, and a `README.md` documenting the
  dev / production / rotation lifecycle.  New `docs/secure-boot.md`
  covering the full chain of trust (Alif Secure Enclave ROM ->
  first-stage -> MCUboot -> application), the production key
  lifecycle through OPTIGA Trust M, the failure-mode matrix for
  `swap-using-scratch`, and the multi-key rotation playbook.
  Live compile-verification gates on the authoritative
  `alp_e1m_evk_aen` board file landing at `alplabai/alp-zephyr-modules`
  (PLAN.md §6 item 8); the sysbuild profile is ready to drop into
  a `pr-twister` scenario the moment that board file appears.
  No code changes -- this is scaffolding + documentation only;
  the v0.4 test-plan row flips from TBD to ⏳ untested with a
  concrete artefact pointer.
- **`<alp/mproc.h>` IPC envelope framing on AEN-Zephyr (v0.4 prep).**
  Wraps every `alp_mbox_send` payload in a 12-byte little-endian
  header (`'AMPF'` magic / monotonic sequence / declared length)
  before handing it to the Zephyr mbox driver, and unwraps inbound
  frames before dispatching `alp_mbox_msg_cb_t`.  New Kconfig
  `CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING` (off by default) plus
  `CONFIG_ALP_SDK_MPROC_FRAME_MAX_BYTES` (default 512) sizing the
  per-handle TX scratch.  Implemented in
  `src/common/proto/alp_mproc_frame.{h,c}` -- a placeholder binary
  framer that exercises the same call sites in
  `src/zephyr/mproc_zephyr.c` that the nanopb-generated codec will
  occupy in v0.4-final.  The placeholder wire is intentionally NOT
  compatible with the v0.4-final protobuf wire generated against
  `metadata/protos/alp_mproc.proto`; both ends of an IPC channel
  must agree on the framing flag.  Coverage: nine ZTESTs in
  `tests/zephyr/mproc/src/main.c` covering encode roundtrip, zero
  payload, NULL outputs, capacity-short failure, decode of short /
  bad-magic / length-overflow frames.  Existing "no backend" tests
  scoped under `#if !CONFIG_ALP_SDK_MPROC` so the new
  `alp_sdk.mproc.nanopb_framing` twister scenario (flips MPROC +
  MBOX on alongside the framing flag) compile-verifies the framing
  branch in `alp_mbox_send` + `mbox_rx_cb` without colliding with
  the no-backend assertions.  Real peer-firmware roundtrip parked
  behind `nightly-aen-hil`.
- **LwRB UART RX ring buffer on AEN-Zephyr (v0.4 prep).**  First
  in-tree consumer of the LwRB anchor at `vendors/lwrb/`.  New
  opt-in API in `<alp/peripheral.h>`:
  `alp_uart_rx_ringbuf_attach()` / `_pop()` / `_count()` /
  `_detach()`.  When `CONFIG_ALP_SDK_UART_RX_RINGBUF=y` (off by
  default; depends on `CONFIG_UART_INTERRUPT_DRIVEN`), the Zephyr
  backend registers an IRQ callback on the underlying UART that
  drains the controller FIFO into a caller-supplied LwRB-backed
  ring on every byte; consumer code pops bytes without polling.
  Backed by the in-tree LwRB stub impl at
  `vendors/lwrb/src/lwrb_stub_impl.c` (~140 LoC, correct
  single-producer / single-consumer semantics with the canonical
  empty/full disambiguation) until the `extras-v04` west group
  flips upstream `MaJerle/lwrb` in.  Builds without the Kconfig
  flag (or non-Zephyr backends) get NULL/NOSUPPORT stubs in
  `src/common/stub_backend.c`, gated by a new
  `ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF` macro so backends can
  adopt the ringbuf incrementally without re-implementing the
  full UART surface.  Failure-path coverage in
  `tests/zephyr/peripheral/src/main.c` (four ZTESTs covering
  NULL-port attach, NULL-handle pop, NULL-handle count, NULL
  detach safety); compile-verification for the feature-on path
  via a new `alp_sdk.peripheral.uart_rx_ringbuf` twister
  scenario with `EXTRA_CONF_FILE=prj_uart_ringbuf.conf`.
  Real-IRQ attach gates on `nightly-aen-hil`.
- **Yocto MQTT backend via libmosquitto (v0.4 prep).**
  `src/yocto/iot_yocto.c` implements `alp_mqtt_*` against the
  Eclipse Mosquitto C client library.  Caller-driven loop model:
  `alp_mqtt_loop(handle, timeout_ms)` pumps the network event
  machine and dispatches inbound messages to every subscription
  whose filter matches via `mosquitto_topic_matches_sub` (so MQTT
  wildcards `+` and `#` work).  URI parser supports `mqtt://host`
  and `mqtt://host:port` (default 1883); `mqtts://` returns
  `ALP_ERR_NOSUPPORT` until the v0.4 secure-stack work lands a
  shared TLS context.  Optional username/password via
  `mosquitto_username_pw_set`.  New per-class
  `ALP_VENDOR_OVERRIDES_MQTT` gate in `src/common/stub_backend.c`
  -- the `alp_wifi_*` half of `<alp/iot.h>` stays stubbed on the
  Yocto path (Wi-Fi bring-up on real Yocto images is a
  wpa_supplicant / NetworkManager concern, not SDK-side).  Build
  is gated on `pkg_check_modules(libmosquitto)`: workspaces
  without libmosquitto-dev on the sysroot fall back cleanly to
  the NOSUPPORT stubs.  CI runner now installs
  `libmosquitto-dev` + `pkg-config`.  Failure-path coverage at
  `tests/yocto/iot_mqtt.c` (NULL cfg / null URI / mqtts NOSUPPORT
  / unknown scheme / empty host / bad port / NULL handle on
  publish/subscribe/loop / close-NULL safety / happy-path
  open-then-close).  Broker-roundtrip coverage parked behind
  ci/HW-IN-LOOP.md.
- **Yocto GPIO IRQ dispatcher (v0.4 prep).**  `alp_gpio_irq_enable`
  / `_disable` now wired against the GPIO v2 edge-event ABI.  A
  single shared pthread runs the `poll()` loop across every pin
  that has IRQ enabled; an eventfd lets mutators wake it for
  slot-table re-snapshots.  Re-configures the line via
  `GPIO_V2_LINE_SET_CONFIG_IOCTL` with
  `GPIO_V2_LINE_FLAG_EDGE_RISING`/`FALLING` (or both) on enable;
  back to plain `INPUT` on disable.  Dispatcher starts lazily on
  first `irq_enable` and runs for the lifetime of the process.
  Callbacks run on the dispatcher thread under the dispatcher
  mutex -- documented contract: callers must not call
  `alp_gpio_irq_disable` / `alp_gpio_close` from inside a
  callback (would deadlock).  `pthread` now linked through
  `Threads::Threads` on the Yocto path.  Failure-path tests
  updated to reflect the new contract (NULL pin / NULL cb /
  `ALP_GPIO_EDGE_NONE` all return `ALP_ERR_INVAL`).  Real-edge
  testing still parked behind the v0.4 hil-yocto runner.
- **Yocto first-class peripheral wrappers — GPIO class (v0.4 prep).**
  `src/yocto/peripheral_gpio.c` binds `alp_gpio_*` against the
  Linux GPIO character-device v2 ABI at `/dev/gpiochipN` (kernel
  >= 5.10).  No libgpiod dependency -- ioctls invoked directly
  against the kernel UAPI in `<linux/gpio.h>`, same pattern as
  the other Yocto wrappers.  Pin-id is packed as `(chip << 16) |
  line_offset` so the studio pin allocator can stay one-axis on
  the wire.  `alp_gpio_configure` switches direction + bias via
  `GPIO_V2_LINE_SET_CONFIG_IOCTL`; `alp_gpio_write` /
  `alp_gpio_read` use the values-get/set ioctls.  Bias support
  passes through to the kernel driver: chips that don't implement
  pull-up/pull-down configuration surface `ALP_ERR_NOSUPPORT`
  from configure.  IRQ paths
  (`alp_gpio_irq_enable` / `_disable`) return `ALP_ERR_NOSUPPORT`
  for now -- callback dispatch needs a `poll()`/pthread loop
  that's parked until a Yocto caller actually needs it.  Per-class
  `ALP_VENDOR_OVERRIDES_GPIO` gate.  Failure-path coverage at
  `tests/yocto/peripheral_gpio.c` (`/dev/gpiochip999` -> ENOENT,
  NULL handle on every entry point, IRQ NOSUPPORT contract,
  close-NULL safety).  Closes the Yocto core-4 peripheral
  wrapper set (I2C / SPI / UART / GPIO all real on Linux).
- **Yocto first-class peripheral wrappers — UART class (v0.4 prep).**
  `src/yocto/peripheral_uart.c` binds `alp_uart_*` against the
  Linux tty layer via termios.  Port-id resolution is a small
  table: 0..99 -> `/dev/ttyS<id>`, 100..199 -> `/dev/ttyAMA<id-100>`,
  200+ -> `/dev/ttyUSB<id-200>`.  Configures data/stop/parity via
  `c_cflag` and baud via `cfsetispeed` + `cfsetospeed`; supported
  baud rates are the standard termios constants (9600 through
  3 Mbps), unknown values return `ALP_ERR_INVAL`.  Reads honour
  `timeout_ms` via `VMIN=1` + `VTIME = ceil(timeout_ms / 100)`,
  returning `ALP_ERR_TIMEOUT` on a clean timeout-with-no-bytes
  and `ALP_OK` on a partial read that beat the timeout.  Writes
  loop on `EINTR`.  Same per-class gate (`ALP_VENDOR_OVERRIDES_UART`).
  Failure-path coverage at `tests/yocto/peripheral_uart.c` (NULL
  cfg, invalid data/stop bits, unsupported baud, /dev/ttyS999 ->
  ENOENT, NULL handle on read/write, close-NULL safety).
- **Yocto first-class peripheral wrappers — SPI class (v0.4 prep).**
  `src/yocto/peripheral_spi.c` binds `alp_spi_*` against Linux
  spidev (`/dev/spidev<bus_id>.<cs_pin_id>`).  Direct two-axis
  mapping: `bus_id` -> SPI controller index, `cs_pin_id` -> CS
  line index (the kernel owns the CS toggle; no userspace
  bit-banging).  Configures mode + bits-per-word + max speed via
  the `SPI_IOC_WR_*` ioctls before the first transfer; full-duplex
  uses `SPI_IOC_MESSAGE(1)` with both `tx_buf` and `rx_buf` set,
  half-duplex uses plain `write()` / `read()` against the same
  fd.  Same errno -> `alp_status_t` mapping as the I2C wrapper;
  same `ALP_VENDOR_OVERRIDES_SPI` per-class gate.  Failure-path
  coverage at `tests/yocto/peripheral_spi.c` (NULL cfg, invalid
  mode / bits-per-word, `/dev/spidev999.0` -> ENOENT, NULL handle
  on every entry point, close-NULL safety).
- **Yocto first-class peripheral wrappers — I2C class (v0.4 prep).**
  `src/yocto/peripheral_i2c.c` binds `alp_i2c_*` against Linux
  i2c-dev (`/dev/i2c-N`).  Maps `alp_i2c_config_t.bus_id` to the
  kernel adapter index; uses `I2C_RDWR` ioctl for write-then-read
  so the device sees a repeated-start between the register pointer
  write and the data read.  Probes `I2C_FUNCS` on open so callers
  fail fast on SMBus-only adapters.  errno → `alp_status_t`
  mapping is shared with the inference path via `errno_to_alp` in
  the same TU (ENOENT → NOT_READY, EBUSY → BUSY, ETIMEDOUT →
  TIMEOUT, default → IO).  Stamps `alp_last_error()` through the
  shared common slot.  Linux-only (gated by `__linux__` +
  `CMAKE_SYSTEM_NAME STREQUAL "Linux"`); non-Linux builds keep
  using the stub I2C symbols.  First Yocto peripheral wrapper out
  of the four-class core set (SPI / GPIO / UART follow in
  subsequent v0.4 increments).
- **Per-class `ALP_VENDOR_OVERRIDES_<CLASS>` gates in
  `src/common/stub_backend.c` (v0.4 prep).**  The previous
  monolithic `ALP_VENDOR_OVERRIDES_PERIPHERAL=1` umbrella forced
  a backend to provide all four peripheral classes (I2C, SPI,
  GPIO, UART) at once or none.  Split into per-class macros so
  the Yocto path can land I2C first and let SPI / GPIO / UART
  keep the NOSUPPORT stubs until their wrappers ship.  Umbrella
  macro is preserved -- defining `ALP_VENDOR_OVERRIDES_PERIPHERAL`
  implies all four per-class macros, so existing vendor wrappers
  at `vendors/alif/` are untouched.
- **`tests/yocto/peripheral_i2c.c` failure-path coverage.**  New
  ctest binary exercising NULL config rejection, non-existent
  bus index (errno → `ALP_ERR_NOT_READY`), NULL handle on
  read/write/write_read paths, and the close-NULL safety
  contract.  Wired into the `pr-plain-cmake.yml` yocto job which
  now configures with `-DALP_BUILD_TESTS=ON` and runs `ctest`.
  Real-adapter HIL coverage stays parked behind
  `ci/HW-IN-LOOP.md` until the `hil-yocto` self-hosted runner
  lands.
- **west.yml pins for v0.4 SDK-internal dependencies.**  LwRB
  pinned at `MaJerle/lwrb@v3.2.0` and nanopb at
  `nanopb/nanopb@nanopb-0.4.9`, both behind the `extras-v04`
  group (disabled by default via the manifest's `group-filter:
  [-extras-v04]`).  `west update` on a v0.3 workspace does not
  fetch them and the Zephyr build does not auto-import them --
  the vendor stub headers under `vendors/{lwrb,nanopb}/include/`
  keep SDK source link-clean.  Flipping the group on with `west
  update --group-filter +extras-v04` makes the upstream sources
  win the include search ahead of the stubs; nanopb's
  `zephyr/module.yml` then auto-registers it as a Zephyr module,
  while LwRB still needs the ~10-line wrapper documented in
  `vendors/lwrb/README.md` "Wiring (v0.4)" once a real consumer
  lands.  Per-library context in `vendors/lwrb/README.md` and
  `vendors/nanopb/README.md`.
- **`<alp/hw_info.h>` runtime hardware-info API.**  Public header
  declaring `alp_hw_info_t`, `alp_hw_info_eeprom_t` (128-byte
  manifest layout that lives at offset 0x0000 of the SoM's
  on-module 24C128 EEPROM), `alp_hw_info_read()`, and
  `alp_hw_info_assert_matches_build()`.  v0.3 ships the API
  contract + a Zephyr-side stub returning `ALP_ERR_NOSUPPORT`;
  the runtime EEPROM + BOARD_ID ADC reads land in v0.3.x once
  the per-family BOARD_ID channels are filled in.  Companion
  production-test programmer at `tools/program_eeprom.py` packs
  a board.yaml + serial + mfg date into the 128-byte binary that
  the production-test fixture writes to the EEPROM.  Unit tests
  pin the manifest layout against drift between the Python writer
  and the C reader; a ztest under `tests/zephyr/hw_info/` covers
  the NOSUPPORT contract for both entry points.
- **Hardware-revision tracking (board.yaml `som.hw_rev` +
  `carrier.hw_rev`).**  Every released family ships an
  `hw-revisions.yaml` (one per family + per carrier) declaring
  per-revision `[min_sdk_version, max_sdk_version]` windows.
  `metadata/sdk_version.yaml` carries the SDK's own version.  The
  loader + validator refuse to emit configs when the chosen
  hw_rev doesn't cover the current SDK version (validator exit
  code 3, loader aborts CMake configure).  Runtime detection
  reads a per-board BOARD_ID ADC channel fed by a 1.8 V
  resistor-divider (`10 kΩ / 10 kΩ → 900 mV` for r1) -- up to ~8
  distinguishable revisions per board at ±100 mV bin radius with
  1 % resistors.  The AEN family ships `r1` as production (all
  AEN MPNs share one PCB per the user-supplied constraint "all
  AENs have the same revision, only SoC changes"); V2N / V2M /
  N93 ship `r1` as TBD-status stubs pending the user-supplied
  HW writeups.  See `docs/board-config.md` "Hardware revision
  tracking".
- **VS Code extension polish.**  New *Alp: Generate all* command
  runs every loader emit mode in one keystroke and reports a
  single status-bar summary.  A new `DiagnosticCollection` runs
  `validate_board_yaml.py` on every open / save of a `board.yaml`
  and surfaces failures inline in the Problems panel -- the
  schema-level checks the Red Hat YAML extension already covers
  remain its job; this layer adds the SDK-specific checks (missing
  SoM preset, missing carrier preset without inline `populated`,
  v0.3 hw_rev / SDK-version compatibility).  Severity tracks the
  validator's exit code: 1 -> Error, 2 -> Warning, 3 -> Error.
- **In-tree VS Code extension (`vscode/`).**  TypeScript
  extension that adds schema-aware `board.yaml` editing
  (autocomplete + red squigglies via the Red Hat YAML
  extension), five starter snippets, a configurator webview
  panel with dropdowns/checkboxes driven from the live
  preset library, one command per loader emit mode, a
  customer-side validator command, per-OS dependency bootstrap
  (Linux/macOS/Windows), and `west build/flash/run` wrappers.
  CI gates compile + schema-sync + `.vsix` package on every PR.
- **SoM presets shipped for every released MPN.**  Per the design
  directive "ship EVK configuration as board.yaml and prepare
  som.yaml file for every MPN, so customer just included MPN
  number in the board.yaml" -- the SDK now ships
  `metadata/e1m_modules/<MPN>/som.yaml` for every released SoM:
    - Alif Ensemble: `E1M-AEN301`, `AEN401`, `AEN501`, `AEN601`,
      `AEN701`, `AEN801` (six MPNs; E3/E7 released silicon, the
      rest preliminary).
    - Renesas RZ/V2N: `E1M-V2N101`, `V2N102` (two MPNs differing
      in DRAM + eMMC capacity).
    - RZ/V2N + DEEPX DX-M1: `E1M-V2M101`, `V2M102` (two MPNs
      mirroring the V2N split, plus the on-module DX-M1).
    - NXP i.MX 93: `E1M-NX9101` placeholder (production MPN TBD).
  Each preset fills in the on-module support silicon (CC3501E,
  OPTIGA Trust M, RV-3028-C7, TMP112, 24C128 for AEN family;
  Murata LBEE5HY2FY + GD32G553 + DA9292 + RTL8211FDI for V2N
  family) from the existing module datasheet docs.  Per the
  project memory note, memory capacities + per-MPN datasheet
  specifics stay TBD until the user-supplied HW config writeup
  fills them in.
- **`docs/board-config.md` "Quick start" section** -- the
  minimum-viable three-line `board.yaml`, a table of every
  MPN the SDK ships a preset for, and a table of stock
  carriers.  Customers paste their MPN and they're done.
- **`docs/getting-started.md` "Project configuration" bullet**
  rewritten around the "three lines: MPN + carrier + OS"
  workflow.

### Changed

- **`alp.yaml` renamed to `board.yaml`** -- the file describes
  what's on the board the firmware runs against, so the name
  should say that.  Plus internal restructure of the metadata
  preset layout to match the SoM-vs-carrier split with one
  conventional file per directory:
    - `metadata/templates/alp.yaml` -> `metadata/templates/board.yaml`
    - `metadata/templates/alp.yaml.example` -> `metadata/templates/board.yaml.example`
    - `metadata/schemas/alp-project-v1.schema.json` ->
      `metadata/schemas/board-config-v1.schema.json`
    - `metadata/e1m_modules/aen/sku-aen701.yaml` ->
      `metadata/e1m_modules/E1M-AEN701/som.yaml`
    - `metadata/e1m_modules/v2n/sku-v2n101.yaml` ->
      `metadata/e1m_modules/E1M-V2N101/som.yaml`
    - `metadata/carriers/e1m-evk.yaml` ->
      `metadata/carriers/E1M-EVK/board.yaml`
    - `metadata/carriers/e1m-x-evk.yaml` ->
      `metadata/carriers/E1M-X-EVK/board.yaml`
    - `metadata/carriers/custom-example.yaml` ->
      `metadata/carriers/custom-example/board.yaml`
    - `docs/project-config.md` -> `docs/board-config.md`
  Loader (`scripts/alp_project.py`) updated: default `--input`
  is now `./board.yaml`; SoM preset resolution looks at
  `metadata/e1m_modules/<SKU>/som.yaml`; carrier preset
  resolution at `metadata/carriers/<name>/board.yaml`.  Schema
  $id + title + description updated to match.  All docs +
  vendor READMEs + plan/versions files updated for the new
  names + paths.  Per-file history preserved via `git mv`.
  No code change in the SDK proper -- pure config-layer rename.
- `vendors/nanopb/README.md`: corrected the west.yml revision pin
  to use nanopb's actual GitHub tag format (`nanopb-0.4.9`, with
  the `nanopb-` prefix; I had previously documented bare `0.4.9`).
  Verified the upstream repo ships a `zephyr/module.yml` so the
  west import picks it up without extra plumbing.
- `vendors/lwrb/README.md`: corrected the integration plan --
  LwRB does **not** ship a `zephyr/module.yml`, so a plain
  west.yml import alone won't register it.  Two options now
  documented: (a) west import + tiny in-repo `zephyr/module.yml`
  shim, or (b) vendor a tagged release under `vendors/lwrb/src/`.
  Plan-A picked for v0.4 default.

### Changed

- **SDK-internal libraries no longer expose user-facing enable
  flags.**  Per the design principle "for ourselves we don't need
  any enable to use in our internal functions":
  - Removed `CONFIG_ALP_SDK_USE_LWRB` + `CONFIG_ALP_SDK_USE_NANOPB`
    from `zephyr/Kconfig`.  Both libraries are SDK-internal
    dependencies (LwRB for audio DMA staging, nanopb for
    `<alp/mproc.h>` IPC framing); consumers don't enable them.
    When the v0.4 audio + mproc paths land, the SDK code uses the
    libraries unconditionally and the west.yml pins land alongside.
  - Removed `lwrb` + `nanopb` from `board.yaml`'s `libraries:` enum
    (`metadata/schemas/board-config-v1.schema.json`) and from the
    loader's `_LIBRARY_KCONFIG` map.  The enum now lists only
    user-facing libraries: etl, fmt, nlohmann_json, doctest, lvgl,
    mbedtls, cmsis_dsp, littlefs.
  - `src/zephyr/audio_zephyr.c` + `src/zephyr/mproc_zephyr.c`
    docstrings + `vendors/lwrb/README.md` + `vendors/nanopb/README.md`
    + `metadata/templates/board.yaml` updated to reflect the
    "SDK-internal, no user-visible enable" status.
  - `metadata/templates/board.yaml.example` updated to exercise the
    user-facing path (`libraries: [lvgl, mbedtls, cmsis_dsp, etl]`)
    -- CI's loader smoke test now covers the new Kconfig mappings.
- **Profile-header filenames now match each upstream library's
  expected name** -- no more six different `alp-embedded.h` files
  scattered across the tree.  Renames:
    - `etl/alp-embedded.h` -> `etl/etl_profile.h` (ETL's expected name)
    - `fmt/alp-embedded.h` -> `fmt/fmt_config.h`
    - `nlohmann_json/alp-embedded.h` -> `nlohmann_json/json_config.h`
    - `lvgl/alp-embedded.h` -> `lvgl/lv_conf.h` (LVGL's expected name)
    - `doctest/alp-embedded.h` -> `doctest/doctest_config.h`
    - `mbedtls/alp-embedded.h` -> `mbedtls/mbedtls_config.h`
      (set `MBEDTLS_CONFIG_FILE` to this path when including).
  Drop-in semantics for libraries that demand a specific config
  filename; self-documenting otherwise.  Done via `git mv` so
  per-file history is preserved.  `metadata/library-profiles/README.md`
  layout table refreshed.

### Added

- **Library profile set extended 3 -> 7.**  Per the design directive
  "make many libraries compatible for user's application, they enable
  in the config file when they want to use":
  - `metadata/library-profiles/lvgl/alp-embedded.h` -- a working
    `lv_conf.h` tuned for E1M displays (RGB565 baseline, 48 KiB
    LV_MEM_SIZE, k_uptime_get-driven LV_TICK_CUSTOM, demos off
    to save flash, image decoders off, filesystem integration off).
  - `metadata/library-profiles/mbedtls/alp-embedded.h` -- a
    minimal-but-modern MbedTLS config: SHA-256/384/512, AES-GCM/CCM,
    HMAC, HKDF, ECDH/ECDSA P-256+P-384, RSA verify, TLS 1.3 client
    only, X.509 parse.  Deliberately omits MD5/SHA-1, DES/3DES/RC4,
    plain CBC, TLS server role, x509 cert generation.
  - `metadata/library-profiles/doctest/alp-embedded.h` -- disables
    POSIX signal handlers + multithreading so doctest builds clean
    on the SDK's test runner.
  - `metadata/library-profiles/cmsis-dsp/README.md` -- intentionally-
    empty profile placeholder; CMSIS-DSP config comes from the SoM's
    target architecture via the SoC metadata, not from a header.
  Plus schema + loader updates so each new entry is enableable
  via `board.yaml`'s `libraries:` array:
  - Schema enum extended: `lvgl`, `mbedtls`, `cmsis_dsp`, `littlefs`
    added alongside the existing `etl`, `fmt`, `nlohmann_json`,
    `doctest`, `lwrb`, `nanopb`.
  - `scripts/alp_project.py` gains a `_LIBRARY_KCONFIG` map -- each
    enabled library maps to the right CONFIG_* flags (e.g.
    `lvgl` -> `CONFIG_LVGL=y`, `mbedtls` -> `CONFIG_MBEDTLS=y` +
    `CONFIG_MBEDTLS_BUILTIN=y`, `littlefs` ->
    `CONFIG_FILE_SYSTEM_LITTLEFS=y` + `CONFIG_FILE_SYSTEM=y`).
    User-facing C++ libs (etl/fmt/nlohmann_json/doctest) emit
    a TODO marker for the v0.4 CMake-include-path hook.
  - `metadata/templates/board.yaml` commented-libraries section now
    showcases all ten enableable libs with one-liner notes.
- **"Using enabled libraries" section** in
  `docs/recommended-libraries.md` -- short usage snippets for
  every Tier-1 library a consumer can enable in `board.yaml`:
  CMSIS-DSP (FIR/FFT), ETLCPP (`etl::vector`, `etl::map`), fmt
  (`fmt::format_to_n`), nlohmann/json (no-exception parse path),
  LVGL (label on the resolved display), LittleFS (mount), and
  doctest (test case).  Wrapping these libraries would be chaos
  per the design principle -- apps use the upstream native API,
  the SDK ships the compile-time profile that makes them
  compatible with our embedded environment.
  `docs/board-config.md` `libraries:` block section cross-links
  to the new "Using enabled libraries" section; also adds an
  explicit "no-wrapper rationale" callout, plus an "SoM vs
  carrier (kept deliberately separate)" subsection codifying that
  the SoM SKU presets live in their own directory hierarchy
  separate from carriers + customer config.
- `docs/board-config.md` "Single source of truth" section
  codifying the design principle: **`board.yaml` is the only place
  to configure the firmware**.  `prj.conf`, CMake `-D` args,
  `local.conf` are all derived from it.  Honest "Today's gaps"
  subsection calls out the three remaining places where hand-
  written config still leaks (DTS overlays for carrier wiring,
  `west.yml` module list, per-test `prj.conf` in
  `tests/zephyr/<area>/`) -- all v0.4 targets.  Template
  (`metadata/templates/board.yaml`) + getting-started.md updated
  with the same principle so new consumers absorb it from page
  one.
- **Library profile headers** at
  [`metadata/library-profiles/<lib>/`](metadata/library-profiles/)
  -- the "compatible without wrapping" model.  Each Tier-1
  library that consumers enable in `board.yaml`'s `libraries:`
  array has a pre-tuned compile-time profile so the upstream
  library works correctly under the SDK's no-exceptions /
  no-iostream / no-STL-on-M-class invariants.  v0.3 ships:
  - `etl/alp-embedded.h` (sets `ETL_NO_STL`, `ETL_NO_EXCEPTIONS`,
    `ETL_CPP17_SUPPORTED`).
  - `fmt/alp-embedded.h` (sets `FMT_HEADER_ONLY=1`,
    `FMT_USE_IOSTREAM=0`, `FMT_EXCEPTIONS=0`).
  - `nlohmann_json/alp-embedded.h` (sets `JSON_NOEXCEPTION=1`,
    `JSON_USE_IMPLICIT_CONVERSIONS=0`).
  v0.4 wires the loader to add the profile's include directory
  ahead of the upstream library's defaults so the profile wins.
  Apps that want different settings supply their own profile
  header at their include root -- the loader prefers the app's
  profile over the SDK's.  Apps still use the upstream API
  directly; no `<alp/...>` wrapper.  Design + per-library notes
  in [`metadata/library-profiles/README.md`](metadata/library-profiles/README.md).
  `docs/board-config.md` "libraries block" section now
  documents the model.
- `docs/board-config.md` "How the loader compiles the file"
  section rewritten from "lands in v0.4" to working invocation
  recipes.  Three concrete worked examples land:
  - Zephyr: how to call the loader at configure time + include
    the generated `alp.conf` from `prj.conf` via `rsource`, plus
    a `CMakeLists.txt` snippet that auto-regenerates on
    `board.yaml` changes via `add_custom_command`.
  - Plain CMake: piping `--emit cmake-args` straight into the
    configure step.
  - Yocto: generating a `local.conf` snippet + requiring it.
  Also documents the three loader follow-ups deferred to v0.4
  (DTS overlay generation, soc_caps cross-validation, `west
  alp-build` extension command).
- `metadata/carriers/custom-example.yaml` -- worked example of a
  customer fork of the E1M-EVK carrier preset.  Shows a slim
  production-board derivative that keeps the IMU + barometer
  and drops the multimedia / debug parts (OLEDs, camera, speaker
  amps, current monitors, I/O expander).  `e1m-evk.yaml`'s
  docstring + `docs/board-config.md` both explicitly position
  the EVK presets as **reference designs** customers fork for
  their own carriers, not just dev-kit-only configs.  Two
  consumption patterns documented: "reference + override" (small
  derivatives) and "fork the preset" (full custom boards).
- `scripts/alp_project.py` -- the `board.yaml` **loader** that compiles
  a project config into per-backend native output.  Validates against
  the v1 schema, resolves the SoM SKU + carrier presets, merges
  overrides, and emits one of three formats:
    - `--emit zephyr-conf` (default): a Kconfig fragment for `prj.conf`
      to append.  Picks the silicon `CONFIG_ALP_SOC_*=y`, the carrier
      chip-driver `CONFIG_ALP_SDK_CHIP_*` flags, the inference
      backend Kconfigs (`CONFIG_ALP_SDK_INFERENCE_TFLM` /
      `_ETHOS_U` / `_DRPAI` / `_ETHOS_U_N93`), IoT features
      (`CONFIG_ALP_SDK_IOT_WIFI` etc.), and library enables
      (`CONFIG_ALP_SDK_USE_LWRB` / `_USE_NANOPB`).
    - `--emit cmake-args`: plain-CMake configure args (`-DALP_SOM=...`,
      `-DALP_OS=...`, `-DALP_SDK_USE_DEEPX_DXM1=ON` on V2N-M1).
    - `--emit yocto-conf`: `local.conf` snippet (`MACHINE = "..."`,
      `IMAGE_INSTALL:append`).
  Python 3.10+; depends on `PyYAML` + `jsonschema` (the latter
  already on the CI path).  CI smoke-tests all three emit formats
  on `metadata/templates/board.yaml.example` via the extended
  `pr-metadata-validate` workflow -- catches schema / loader
  regressions at PR time.
- `metadata/templates/board.yaml.example` -- a fully-uncommented
  config the loader exercises end-to-end.  Distinct from
  `board.yaml` (the heavily-commented user template).

### Changed

- `board.yaml` schema split into SoM-vs-carrier blocks.  The first
  pass conflated on-module components (silicon + CC3501E + OPTIGA
  + RV3028 + TMP112 + 24C128, fixed at SoM-fab time) with carrier-
  board components (LSM6DSO + BMI323 + ICM-42670 + BMP581 + OLEDs
  + OV5640 + TAS2563 + INA236, variable per board design).  The
  corrected schema:
  - `som` block carries on-module concerns only (silicon SKU,
    on-module radio / secure element / RTC overrides for SoM-
    variant SKUs, memory capacities).
  - New `carrier` block carries the per-board chip population --
    each `populated.<name>: true` enables the corresponding
    `CONFIG_ALP_SDK_CHIP_<NAME>=y` at build time.
  - New stock carrier presets at `metadata/carriers/e1m-evk.yaml`
    (35x35 EVK -- IMU x3, BMP581, SSD1306, PDM mics, TAS2563 x2,
    INA236 x6, TCAL9538, PI3WVR626) and
    `metadata/carriers/e1m-x-evk.yaml` (45x65 EVK -- populated
    list TBD pending user HW config writeup).
  - `metadata/e1m_modules/<family>/sku-*.yaml` files updated:
    removed the misclassified carrier-side components; kept only
    on-module parts.
  - `metadata/templates/board.yaml` + `docs/board-config.md`
    updated with the SoM-vs-carrier distinction explained.

### Added

- **Project configuration (`board.yaml`)** -- one declarative YAML
  file per consumer project that picks the SoM SKU + per-component
  assembly overrides + OS backend + inference backend + optional
  libraries + connectivity features.  Collapses what was previously
  three separate config formats (`prj.conf` for Zephyr, cmake `-D`
  flags for plain CMake, `local.conf` for Yocto) into one source
  of truth.
  - Schema: `metadata/schemas/board-config-v1.schema.json` (JSON
    Schema draft-2020-12).
  - Canonical template: `metadata/templates/board.yaml` (fully
    commented).
  - Stock SKU presets: `metadata/e1m_modules/<family>/sku-<sku>.yaml`.
    v0.3 ships two worked examples (`sku-aen701.yaml`,
    `sku-v2n101.yaml`); remaining SKUs (aen301/401/501/601/801,
    v2n102, v2m101/102, NX9xxx) land as the user-supplied hardware
    configuration writeup fills them in.  Values not in the
    silicon datasheet stay `TBD` until then per the project
    memory note.
  - Design + reference: `docs/board-config.md`.
  - `docs/getting-started.md` "Where to go next" updated to point
    at board-config.md as the first item.
  - The loader script that emits per-backend native configs from
    `board.yaml` (Zephyr fragments / cmake `-D` / Yocto local.conf)
    lands in v0.4.  v0.3 documents the mapping so consumers can
    hand-translate until then.
  - **Optional libraries (ETLCPP, fmt, nlohmann/json, doctest,
    LwRB, nanopb) are declared in the same file** -- the v0.4
    loader wires their include paths into the build when listed,
    no `<alp/...>` wrapper.  Apps use the libraries through their
    native APIs.
- `bench/` extended from 3 -> 6 cases.  New files cover the
  rejection / fast-path costs for `<alp/iot.h>` (`bench_iot.c`),
  `<alp/audio.h>` (`bench_audio.c`), and `<alp/storage.h>`
  (`bench_storage.c`).  Same NULL/empty-cfg shape as the
  peripheral bench.  `bench_main.c` updated to invoke them;
  `bench/README.md` table refreshed.  Total bench coverage at
  ~6 of ~15 public API classes; v1.0 fills the rest as the
  implementations land.
- `tests/yocto/` -- plain-CMake test suite for the Linux user-space
  backend.  First entry: `alp_test_inference_dispatcher` exercises
  the seven branches of `src/yocto/inference_yocto.c` --
  NULL-cfg / NULL-model / zero-size cfg rejection, AUTO-with-no-
  backend NOSUPPORT, explicit unsupported backend NOSUPPORT, NULL-
  handle safety on every accessor, and that `alp_last_error()`
  correctly stamps `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT` per the
  new unified setter.  Mirrors the contract in the Zephyr ztest
  suite at `tests/zephyr/inference/`; uses a tiny local
  assert harness (`test_assert.h`) since plain-CMake builds don't
  pull in ztest.  Opts in via `ALP_OS=yocto` + `ALP_BUILD_TESTS=ON`.
- `src/common/alp_internal.h` -- cross-backend internal header
  declaring `alp_internal_set_last_error(s)`.  Lets non-Zephyr
  backend source files (the yocto inference dispatcher today,
  more sites in v0.4) stamp the same process-wide last-error
  slot that `alp_last_error()` reads, instead of carrying
  per-file shadow statics.  Closes the TODO(v0.4) comment in
  `src/yocto/inference_yocto.c`.  Zephyr keeps its own
  thread-local last-error in `src/zephyr/last_error.c`; the new
  header is irrelevant there.
- `vendors/renesas-rzv2n/rzv_drp-ai_tvm/README.md` -- integration
  anchor for **RUHMI** (formerly "DRP-AI TVM"), Renesas's
  Apache-2.0 host-side model compiler at
  <https://github.com/renesas-rz/rzv_drp-ai_tvm>.  Distinct from
  the target-side `libdrpai` runtime (which ships via
  `meta-rz-drpai` in the Renesas BSP).  The SDK's
  `<alp/inference.h>` Yocto backend links against the runtime;
  model authors run RUHMI on their workstation and ship compiled
  output as a model asset.  `vendors/renesas-rzv2n/README.md`
  cross-links to the new subdirectory and documents the BSP
  setup pointer.

### Changed

- `yocto/meta-alp/` rebased on the **Renesas RZ/V2N AI SDK 7.10**
  BSP.  Earlier README + layer.conf referenced `meta-renesas-rz`
  (no such repo); the canonical layer is `meta-renesas` at
  <https://github.com/renesas-rz/meta-renesas>, distributed via
  the AI SDK 7.10 tarball (`RTK0EF0045Z94001AZJ-v1.0.3.zip` on
  My Renesas -- free signup, no NDA for the standard build).
  meta-alp now `LAYERRECOMMENDS` `meta-renesas` plus the four
  `meta-rz-features/*` sublayers (`meta-rz-graphics`,
  `meta-rz-drpai`, `meta-rz-opencva`, `meta-rz-codecs`) +
  `meta-econsys` for the camera path + `meta-deepx-m1` for
  V2N-M1.  `e1m-x-v2n.conf` now inherits from Renesas's stock
  `rzv2n-evk` MACHINE (vs the earlier guess at a `rzv2n-common.inc`
  include path).  Carrier-specific DTB stays TBD per the
  user-supplied HW config writeup; build falls back to Renesas's
  stock V2N-EVK DTB until then.  Yocto Scarthgap (5.0.11)
  is the recommended series; Kirkstone still listed as
  compatible.  Full step-by-step recipe in
  `yocto/meta-alp/README.md`.
- `vendors/deepx-dxm1/README.md` rebased on the actual DEEPX
  software distribution.  Earlier copy described the runtime as
  "proprietary, sign in at developer portal"; the source is in
  fact **source-visible on GitHub at <https://github.com/DEEPX-AI>**
  but under a proprietary "customer-only" license (per the
  verbatim text of `dx_rt/LICENSE`).  Three concrete repos
  documented for the V2N-M1 integration: `dx_rt` (userspace
  runtime), `dx_rt_npu_linux_driver` (PCIe kernel driver -- previously
  unmentioned), and `meta-deepx-m1` (Yocto recipes).  Clean-room
  stub at `vendors/deepx-dxm1/include/dxnn/dxnn.h` stays unchanged
  -- the rewrite is README-only.

### Added

- Library integration pass for v0.4 prep -- two Tier-2 libraries from
  docs/recommended-libraries.md land as scaffolding anchors:
  - **LwRB** (MaJerle, MIT) under `vendors/lwrb/` with a stub
    `<lwrb/lwrb.h>` mirroring the upstream API + `CONFIG_ALP_SDK_USE_LWRB`
    Kconfig flag (default OFF).  audio_zephyr.c gets a docstring
    anchor; first real use (byte-granular DMA staging) lands v0.4
    once the west.yml pin lands.
  - **nanopb** (zlib) under `vendors/nanopb/` with stub `<pb.h>` +
    `<pb_encode.h>` + `<pb_decode.h>` + `CONFIG_ALP_SDK_USE_NANOPB`
    Kconfig flag (default OFF).  mproc_zephyr.c gets a docstring
    anchor; first real use (IPC frame serialisation) lands v0.4
    alongside the multi-proc completion.
  - New `metadata/protos/alp_mproc.proto` -- the v1 IPC schema
    (Envelope / Heartbeat / RpcRequest / RpcResponse / Notification)
    that the nanopb-backed mproc framing will code-gen against.
  - `docs/recommended-libraries.md` updated: LwRB + nanoPB move from
    Tier 2 "under evaluation" to Tier 3 "already integrated";
    TinyFrame / heatshrink / trice / nanoMODBUS / o1heap noted as
    deferred to v0.5+ with one-liner rationales.
  - `docs/getting-started.md` "Where to go next" now points at
    `docs/recommended-libraries.md` so hand-written-firmware authors
    discover the curated companion library list.
- Ethos-U65 (i.MX 93) wired alongside Ethos-U55 (AEN) on the Zephyr
  TFLM inference path.  The `inference_tflm.cpp` source path was
  already portable across U55 and U65 (both register through
  `AddEthosU()`); v0.3 lays down the Kconfig + per-variant anchor.
  New `ALP_SDK_INFERENCE_ETHOS_U_N93` Kconfig defaults on when the
  SoC choice is i.MX 93 and compiles `src/zephyr/inference_ethosu_n93.c`
  -- a thin anchor that exposes `alp_ethosu_n93_register` (no-op until
  v0.4 wires the NPU attach) + `alp_ethosu_variant_name` (literal
  `"ethos-u65"`).  `ALP_SDK_INFERENCE_ETHOS_U` now defaults on for
  both AEN-E7 and i.MX 93.  vendors/nxp-imx93/README.md documents the
  Vela invocation (`ethos-u65-256`) + the A55-side proxy plan via
  OpenAMP/M33 firmware (v0.4 first-class).  PLAN.md row updated.
- DEEPX DX-M1 inference backend on the Yocto / Linux user-space path.
  New `src/yocto/inference_yocto.c` is the first real Yocto-side
  surface in the SDK -- it owns the `alp_inference_*` public symbols
  (overriding `src/common/stub_backend.c` via the
  `ALP_VENDOR_OVERRIDES_INFERENCE` guard) and routes
  `ALP_INFERENCE_BACKEND_DEEPX_DX` through `inference_deepx.cpp`.
  `vendors/deepx-dxm1/` ships a stub `<dxnn/dxnn.h>` so the dispatcher
  compiles on hosts that don't have the proprietary DEEPX runtime
  installed -- the real `dxnn_*` link arrives v0.4 alongside the
  `deepx-dxm1-host-sdk` Yocto recipe.  Gated via the CMake option
  `ALP_SDK_USE_DEEPX_DXM1`; meta-alp's `e1m-x-v2n-m1.conf` MACHINE
  drives that ON.  `resolve_auto()` for Yocto prefers DEEPX_DX first
  on V2N-M1, falling back to NOSUPPORT when no backend is compiled
  in.  PLAN.md §2.3 + docs/recommended-libraries.md updated.
- `yocto/meta-alp/` is now a v0.3-scaffolded BSP layer (vs the
  v0.2 placeholder).  Three machine configs land:
  `e1m-x-v2n.conf` (RZ/V2N), `e1m-x-v2n-m1.conf` (V2N + DEEPX),
  `e1m-n93.conf` (i.MX 93 + Ethos-U65).  Three new recipes land:
  `alp-sdk-runtime` (libalp_sdk.so + headers, per-machine
  inference-backend cmake defines), `alp-chips` (libalp_chips.a +
  per-chip PACKAGECONFIG knobs for all 19 drivers), and
  `alp-studio-codegen` (CLI helper; opt-in via DISTRO_FEATURES,
  guarded with bb.parse.SkipPackage until alplabai/alp-studio is
  public).  layer.conf advertises `LAYERRECOMMENDS_alp` for
  meta-renesas-rz + meta-imx.  Recipes parse but don't build --
  real `do_compile` against the Yocto cross-toolchain lands in
  v0.4 with `src/yocto/` going from stub to real.
- [ADR 0006](docs/adr/0006-secure-boot-secure-ota.md) lands the
  v0.4 secure boot + secure OTA design.  Per-SoM vendor-native
  secure boot (Alif Secure Enclave + MCUboot on AEN; NXP AHAB on
  N93; Renesas SBM on V2N), unified `alp_ota_*` surface in
  `<alp/iot.h>` routed through MCUboot (Zephyr) or RAUC (Linux),
  trust-anchor pinning + min-version anti-rollback baked into the
  config.  No code yet -- v0.4 cycle implements it.
- `firmware/cc3501e/` scaffolding -- the alp-sdk side of the
  two-repo boundary with the future `alplabai/cc3501e-firmware`:
  bootstrap README that mirrors the contract from
  `docs/cc3501e-bridge.md`, a `flash.py` stub (dry-run + SHA-256
  + signature presence checks today; real bootloader sequence
  lands with the first signed binary), `protocol-version.txt`
  pinned to wire-protocol v1, and an empty `prebuilt/` with its
  own CHANGELOG ready to receive `cc3501e-vX.Y.Z.bin` releases.

### Removed (pre-1.0)

- `<alp/math.h>` and `<alp/signal.h>` deleted.  They were thin
  re-exports of CMSIS-DSP that added zero value over a direct
  `#include "arm_math.h"`.  Application code now includes
  CMSIS-DSP directly; ALP SDK internals (e.g. inside
  `<alp/audio.h>`) may still pull CMSIS-DSP in via the
  build-time `ALP_HAS_CMSIS_DSP` option.  Documented stance
  in `docs/architecture.md` + `VERSIONS.md` +
  `docs/os-support-matrix.md`.

### Added

- Public headers under `include/alp/` for the v0.1 surface:
  `peripheral.h`, `display.h`, `camera.h`, `gui.h`, `iot.h`.
  C99-compatible, Doxygen-commented.
- Zephyr backend implementations for `alp_i2c_*`, `alp_spi_*`,
  `alp_gpio_*`, `alp_uart_*` under `src/zephyr/`, with a custom
  `alp,pin-array` devicetree binding for studio-resolved pin lookup.
- Static handle pools with Kconfig-tunable size limits
  (`CONFIG_ALP_SDK_MAX_*_HANDLES`).
- Top-level CMake support for both Zephyr-module consumption and
  plain `add_subdirectory()` consumption.
- Chip metadata schema `metadata/schemas/soc-spec-v1.schema.json`
  and per-SoC metadata files for the full Alif Ensemble line:
  `e3.json` and `e7.json` (released, real datasheet),
  `e8.json` (preliminary, datasheet v0.51), and
  `e4.json` / `e5.json` / `e6.json` (preliminary stubs derived from
  the ALP Lab E1M-AEN module datasheet draft + family pattern).
- Documentation: `README.md`, `docs/architecture.md`,
  `docs/os-support-matrix.md`, `docs/porting-new-som.md`,
  `vendors/alif/README.md`, `vendors/renesas-rzv2n/README.md`.
- ztest suite at `tests/zephyr/peripheral/` runs **12 of 12 cases
  green under twister on `native_sim/native/64`** with the alp-sdk
  module loaded via `EXTRA_ZEPHYR_MODULES`.  Coverage: open/close
  lifecycle, NULL-arg validation, status-code propagation, GPIO
  configure/write/read forwarding, pool exhaustion at the
  Kconfig-defined limits, and SPI / UART / I²C round-trips
  against Zephyr's emulated controllers.
- Devicetree binding `dts/bindings/alp,pin-array.yaml` is now
  picked up via the module's `dts_root: .`, with `alp` registered
  as a vendor prefix in `dts/bindings/vendor-prefixes.txt`.
- `docs/e1m-pinout.md` documents the pinout chain (e1m-spec →
  per-SoM manifest → studio pin allocator → SDK opaque integer).
  Confirms the SDK does not duplicate pad data; pinned to
  `alplabai/e1m-spec` v1.0.
- **Global pin/instance map** at `include/alp/e1m_pinout.h` — the
  E1M-standard peripheral instance IDs (`ALP_E1M_I2C0`,
  `ALP_E1M_SPI0`, `ALP_E1M_UART0`, …) and pad-level GPIO indices
  (`ALP_E1M_GPIO_IO0`–`IO25`, `PWM0`–`PWM7`, `ENC0_X`–`ENC3_Y`)
  baked as C macros.  Carrier `alp,pin-array` overlays MUST follow
  the canonical ordering so the macros stay portable across every
  E1M-conformant carrier.  42 GPIO indices total.
- Carrier-feature names at
  `include/alp/boards/alp_e1m_evk.h` — readable EVK-side aliases
  (`EVK_PWM_LED_RED`, `EVK_PIN_ENCODER_SW`,
  `EVK_I2C_BUS_SENSORS`, on-board sensor I2C addresses) layered on
  top of the global E1M map.  The header is SoM-agnostic: the E1M
  EVK accepts 35x35 mm SoMs (currently AEN, soon N93).  Per-SoM
  dispatch differences (e.g. AEN-side CC3501E proxying for IO11 /
  IO13 / IO15..IO21) are called out per-pad in the doc-comments.
  The 45x65 E1M-X carrier gets its own future header
  `<alp/boards/alp_e1m_x_evk.h>`.
- M.2 E-key wake + radio-disable wiring on the EVK header:
  `EVK_PIN_M2E_UART_WAKE` (IO19), `EVK_PIN_M2E_SDIO_WAKE` (IO18),
  `EVK_PIN_W_DISABLE1` (IO17, Wi-Fi disable, open-drain),
  `EVK_PIN_W_DISABLE2` (IO16, BT disable, open-drain).  All four
  proxy through the on-module CC3501E.
- BMI323 INT1 macro: `EVK_PIN_BMI323_INT1` (IO15, CC3501E-side).
  The IMU's data-ready / motion / FIFO interrupt does not pass
  through the main TCAL9538 expander -- those bits hold the
  ICM-42670 + BMP581 interrupt lines.
- **Chips library v0.1**:
  - `chips/lsm6dso/` + `<alp/chips/lsm6dso.h>` — STMicro
    LSM6DSO 6-axis IMU driver.  WHO_AM_I check, ODR/full-scale
    config, raw accel/gyro/temp reads.  Symbols use the chip's
    natural prefix (`lsm6dso_*`) per the no-`alp_`-on-chip-drivers
    rule.
  - `chips/ssd1306/` + `<alp/chips/ssd1306.h>` — Solomon
    Systech SSD1306 monochrome OLED driver.  I²C, 128×64/128×32
    geometries, init sequence, `clear` / `draw_pixel` /
    `set_contrast` / `set_inverted` / `display`.  Vertical-byte
    framebuffer matching the SSD1306 GDDRAM layout.
  - `chips/button_led/` + `<alp/chips/button_led.h>` — generic
    button + LED helper.  Carries the `alp_` prefix as the
    documented exception (block utility, not single-IC driver).
    Wraps `alp_gpio_*` for portable lifecycle.
  - Each chip is opt-in via `CONFIG_ALP_SDK_CHIP_*` Kconfig.
- New ztest suite at `tests/zephyr/chips/` covering all three
  chip drivers' lifecycle, NULL handling, and status-code
  propagation.  **Now 20 of 20 ztest cases pass on
  `native_sim/native/64`** (12 peripheral + 8 chips).
- **Renesas RZ/V2N metadata** at
  `metadata/socs/renesas/rzv2n/n44.json` — extracted from
  R01DS0466EJ0120 v1.20 (Sep 2025).  Captures the quad
  Cortex-A55 + Cortex-M33 + DRP-AI3 (4 dense / 15 sparse TOPS)
  topology, 1.5 MB on-chip SRAM (ECC), full peripheral inventory
  (2× GbE, USB 3.2 Gen2, PCIe Gen3, 6× CAN-FD, 9× I²C, 24× ADC,
  …), 840-pin FCBGA, and all 8 orderable RZ/V2N + RZ/V2NP SKUs
  (N41–N48) including the ALP Lab default `R9A09G056N44GBG#AC0`.
- **DEEPX DX-M1 companion accelerator metadata** at
  `metadata/socs/deepx/dx/m1.json` — extracted from DEEPX
  Commercial Datasheet v1.0 (June 2025).  25 TOPS @ 1.0 GHz INT8
  CNN accelerator behind PCIe Gen3 ×4, with up to 8 GB
  LPDDR4X/LPDDR5 + internal Cortex-M55 firmware controller.
  FC-BGA 625-ball.  Linked to the E1M-V2M101 / E1M-V2M102 SoM
  SKUs.
- Internal design-archive metadata mirror refreshed with the new files.
- **EdgeAI vision-AEN reference application skeleton** at
  `examples/edgeai-vision-aen/` — compiles under
  `native_sim/native/64` and prints the v0.1 init flow + v0.2
  pipeline TODOs.  Ships the full Zephyr-app layout (`README.md`,
  `CMakeLists.txt`, `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_aen}.overlay`,
  `src/main.c`, `models/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.2 implementation slots into a stable
  scaffold.  The console twister harness asserts the skeleton's
  `[edgeai] done` line — passing now.
- `alp_pixfmt_t` typedef hoisted from `<alp/display.h>` to
  `<alp/peripheral.h>` so `<alp/camera.h>` can reference it
  without forcing a forward dependency on display.
- v0.1 stub for the camera surface at `src/zephyr/camera_stub.c`:
  every `alp_camera_*` entry returns `ALP_ERR_NOSUPPORT`,
  `alp_camera_open` returns `NULL` — matches the v0.1 contract
  in `<alp/camera.h>` and lets applications link cleanly until
  the v0.2 Zephyr-video integration arrives.
- **Twister now covers tests + examples**: 21 of 21 cases pass on
  `native_sim/native/64` across the peripheral suite, the chips
  suite, and the EdgeAI skeleton sample.
- **IoT connected-camera reference application skeleton** at
  `examples/iot-connected-camera/` — the v0.3 deliverable's
  v0.1 scaffold.  Targets the V2N family (Renesas RZ/V2N) for
  the connected-product path.  Six-stage skeleton:
  peripherals → camera → classifier → Wi-Fi station → MQTT/TLS
  → main loop, with TODO blocks marking which version each
  stage's real implementation lands in (v0.2 camera/classifier,
  v0.3 IoT).  Compiles + runs cleanly under
  `native_sim/native/64` (twister console-harness asserts the
  `[iotcam] done` line); EVK-V2N target parked behind a comment
  until `alplabai/alp-zephyr-modules` publishes the V2N board file.
  Ships the full scaffold (`README.md`, `CMakeLists.txt`,
  `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_v2n}.overlay`,
  `src/main.c`, `certs/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.3 implementation slots in cleanly.
- v0.1 stub for the IoT surface at `src/zephyr/iot_stub.c`:
  every `alp_wifi_*` / `alp_mqtt_*` entry returns
  `ALP_ERR_NOSUPPORT`, `*_open` returns `NULL` — matches the
  v0.1 "header-only" contract for `<alp/iot.h>` and lets
  applications link cleanly until v0.2 (Wi-Fi-station + MQTT
  on AEN-Zephyr) and v0.3 (V2N + TLS + BLE).
- **Twister now covers 4 scenarios on `native_sim/native/64`**:
  peripheral suite (12 cases), chips suite (8 cases), EdgeAI
  skeleton, IoT skeleton.  **22 of 22 cases pass.**
- **GitHub Actions CI shipped**:
  - `.github/workflows/pr-twister.yml` — runs the full twister
    matrix on every PR using the `zephyrprojectrtos/ci` Docker
    image (Zephyr v3.7.0 pinned per the v0.1 dependency matrix).
  - `.github/workflows/pr-metadata-validate.yml` — validates
    every `metadata/socs/**/*.json` against the v1 schema via
    `scripts/validate_metadata.py`.  Caught a real schema bug:
    `gops` was required on NPUs but V2N (DRP-AI) and DX-M1 quote
    `tops`.  Schema relaxed to `anyOf: [gops, tops]`; all 8
    SoC files now validate green.
  - `.github/workflows/pr-doxygen.yml` — best-effort Doxygen
    HTML build on every PR; v0.1 reports warnings as
    informational, v1.0 will gate on zero warnings.
  - `.github/workflows/pr-static-analysis.yml` — clang-format
    diff-only check (changed lines must satisfy
    `.clang-format`) plus `cppcheck` on `src/` + `chips/`.
  - `.github/workflows/nightly-aen-hil.yml` — skeleton for
    nightly HW-in-loop on a real E1M-AEN dev kit.  Gated on a
    `hil-aen` self-hosted runner (none yet).  Runner setup
    contract documented in [`ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md).
  - `.clang-format` at the repo root (LLVM-base, 100-col,
    4-space indent).
  - `ci/README.md` is now a CI policy / index doc rather than
    claiming to hold the authoritative workflow copies.
- **PR template** (`.github/PULL_REQUEST_TEMPLATE.md`) gates on:
  roadmap-row attribution, ABI impact classification, twister +
  metadata-validate test plan, CHANGELOG/VERSIONS update.
- Issue templates updated to capture SoC `ref` from
  `metadata/socs/`, SoM SKU, carrier board, OS backend, Zephyr
  version, and Twister/HIL artefact paths.
- **Deeper chip ztests** (no emul fixture required):
  `ssd1306_draw_pixel` covers the page/column math (set bit,
  clear bit, OOB silent-ignore on 4 corners), `ssd1306_clear`
  verifies it wipes only the framebuffer (`width`/`height`/`addr`
  preserved), and `test_public_headers_co_compile` includes
  every `<alp/...>` header in one TU to catch
  typedef/macro-collision regressions.  Test count climbed
  **22 → 27** with no new failures.
- E1M EVK (UG-E1M-001) support for E1M-AEN family: SDK cheat
  sheet at `docs/boards/e1m-evk.md`, board overlay at
  `tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay`, and
  an `alp_sdk.peripheral.evk_aen` build-only scenario tagged
  `alp-evk` for nightly HW-in-loop CI.
- `VERSIONS.md` tracking the v0.1 → v1.0 roadmap.
- **v0.2 chip-driver scaffolding** — public surface, opt-in build
  wiring, and lifecycle / NULL-arg ztests for five new drivers:
  - `chips/bme280/` + `<alp/chips/bme280.h>` — Bosch BME280
    combined T/H/P sensor.  I²C-only in v0.2.  Loads the
    per-die calibration coefficients on init and exposes
    `bme280_compensate` with the canonical Bosch integer-form
    arithmetic transcribed from BST-BME280-DS002 v1.6 §4.2.3.
  - `chips/lis2dw12/` + `<alp/chips/lis2dw12.h>` — STMicro
    LIS2DW12 3-axis ultra-low-power accelerometer.  WHO_AM_I
    check, ODR + full-scale + power-mode config in one call,
    raw 14-bit accel + on-die temp reads.
  - `chips/ssd1331/` + `<alp/chips/ssd1331.h>` — Solomon Systech
    SSD1331 96×64 colour OLED.  SPI 4-wire (D/C# pin),
    caller-supplied 12 KiB framebuffer (RGB565), pixel/clear/
    display API, full datasheet init sequence on `ssd1331_init`.
  - `chips/ov5640/` + `<alp/chips/ov5640.h>` — OmniVision OV5640
    5 MP image sensor SCCB-side configuration driver.  Chip-ID
    verify, soft-reset, resolution / format / test-pattern
    presets.  Capture-side (MIPI CSI-2) lives in v0.2's
    `<alp/camera.h>`; per-resolution register tables ship in
    v0.3 alongside the V2N alp_camera integration.
  - `chips/pdm_mic/` + `<alp/chips/pdm_mic.h>` — generic PDM
    microphone block helper.  `alp_`-prefixed (block utility,
    documented exception per the chip-driver naming rule).
    Surface declared; impl returns `ALP_ERR_NOSUPPORT` until
    v0.2's `<alp/audio.h>` lands the underlying I²S
    abstraction.
- **v0.2 / v0.3 public-header surface declared** so application
  code can compile against it now and the implementations slot
  in without ABI churn (same pattern `<alp/iot.h>` followed in
  v0.1):
  - `<alp/audio.h>` (v0.2) — PDM input + I²S output API,
    `alp_audio_in_*` / `alp_audio_out_*`, three sample formats
    (S16/S24/S32), per-block read/write with timeout.
  - `<alp/ble.h>` (v0.3) — peripheral + central, GATT server +
    client.  Zephyr `bt` host stack per the locked decision.
    Mesh / audio / DF explicitly out of scope for v1.0.
  - `<alp/security.h>` (v0.3) — MbedTLS re-export shape: hash
    (SHA-256/384/512), AEAD (AES-128/256-GCM, ChaCha20-Poly1305),
    TRNG.  Per-SoC hardware-accelerator routing happens at the
    backend layer.
  - `<alp/mproc.h>` (v0.3) — multi-processor IPC primitives:
    shared memory regions (`alp_shmem_*`), mailbox channels
    (`alp_mbox_*`, MHU on Alif), hardware semaphores
    (`alp_hwsem_*`).  Wraps the M55-HE core bring-up the
    "Multi-Processor Support Completion" milestone delivers.
  - Stub implementations at `src/zephyr/{audio,ble,security,
    mproc}_stub.c` returning `ALP_ERR_NOSUPPORT` and `*_open()
    → NULL`, matching the v0.1 stub contract.
- **Chip ztest suite extended** to the eight chip drivers.  Each
  new chip gets lifecycle + NULL-arg cases plus a "post-init
  calls reject uninitialised" pattern for the I²C drivers (the
  emul controller has no real device behind it, so `*_init` fails
  WHO_AM_I and downstream calls must report `NOT_READY`).  Also
  includes a pure-math test for `ssd1331_rgb565` and a runtime
  check that the v0.2 / v0.3 stubbed surfaces honour the
  documented `NULL` / `NOSUPPORT` contract.  Total tests across
  twister `native_sim/native/64` climb **27 → 43 cases** (chips
  suite alone: 13 → 29).
- `test_public_headers_co_compile` extended to include
  `<alp/audio.h>`, `<alp/ble.h>`, `<alp/security.h>`,
  `<alp/mproc.h>`, and the five new chip headers.
- **ABI snapshot tooling**: `scripts/abi_snapshot.py` walks
  `include/alp/**.h`, extracts function decls, typedefs, and
  `#define`s, and emits a stable JSON fingerprint per public
  symbol.  Supports `--diff <prior.json>` for per-symbol change
  reporting.  Pre-1.0 the diff is informational; v1.0 promotes
  it to a CI gate (`pr-abi-snapshot.yml`) where `REMOVED` /
  `CHANGED` entries require a major-version bump.
- `docs/abi/v0.1-snapshot.json` ships the v0.1 ABI fingerprint —
  21 headers, 123 functions, 80 typedefs, 114 macros.  See
  `docs/abi/README.md` for the workflow.
- `docs/architecture.md` gains a "Consumers of this SDK"
  section: alp-studio integration contract, Zephyr-application
  consumption recipe, and the v0.4 Yocto / `meta-alp` shape.
- `examples/README.md` per-example status descriptions tightened
  with the concrete pipeline stages each skeleton scaffolds and
  which version fills in each stage.
- **Register-protocol ztests** via test-only i2c-emul fixtures.
  Three fake targets (`tests/zephyr/chips/src/fake_{lsm6dso,
  ssd1306,bme280}.c`) attach to the test's `i2c0_emul`
  controller at the chip drivers' default I2C addresses,
  pre-populate `WHO_AM_I` / `CHIP_ID` plus calibration data,
  and either echo register writes back or capture the byte
  stream for the test to inspect via `fakes.h` helpers.
  - **fake LSM6DSO**: register-store with WHO_AM_I=0x6C seed.
    Tests verify `lsm6dso_init` succeeds, `set_accel`/`set_gyro`
    encode the documented byte into CTRL1_XL / CTRL2_G, and
    `read_accel` decodes the seeded LE register pairs.
  - **fake SSD1306**: command/data byte logger split by the
    SSD1306 control byte (0x00 vs 0x40).  Tests verify
    `ssd1306_init` streams DISPLAY_OFF first, charge-pump
    enable (0x8D 0x14), DISPLAY_ON last; `ssd1306_display`
    sets the full address window then pushes 1024 framebuffer
    bytes with the seeded pixel landing at offset 0.
  - **fake BME280**: register store seeded with the canonical
    BST-BME280-DS002 §4.2.2 example calibration coefficients
    and §4.2.3 example raw conversion (T_raw=519888,
    P_raw=415148).  Tests verify init loads every coefficient
    correctly, `read_raw` decodes the 20/20/16-bit values, and
    `compensate` reproduces the documented worked example
    (T = 25.08 °C, P = 100653 Pa) within ±2 LSB and ±50 Pa.
  - DT bindings live at `tests/zephyr/chips/dts/bindings/
    alp,fake-*.yaml`; the chip-test overlay attaches each
    fake at the canonical I2C address.
  - Chips suite tests grow **29 → 38 cases**; total twister
    cases on `native_sim/native/64` climb **43 → 52**.

- **v0.2 peripheral HAL expansion** — eight new peripheral classes
  wrapped, doubling the SDK's coverage of what an MCU actually
  exposes (was 4 classes, now 12).  Each new class follows the v0.1
  pattern: a public header with full Doxygen, a Zephyr backend that
  resolves studio-supplied IDs via the `alp-<class>N` DT alias and
  forwards to the matching Zephyr driver class, a Kconfig opt-in
  gated on the underlying subsystem, a static handle pool with a
  per-class quota, and runtime validation against the active SoC's
  documented caps:
  - `<alp/pwm.h>` + `src/zephyr/peripheral_pwm.c` — Zephyr `pwm_*`.
  - `<alp/adc.h>` + `src/zephyr/peripheral_adc.c` — Zephyr `adc_*`.
  - `<alp/counter.h>` + `src/zephyr/peripheral_counter.c` /
    `peripheral_qenc.c` — free-running counter via `counter_*`,
    quadrature-decoder via `sensor_*`.
  - `<alp/i2s.h>` + `src/zephyr/peripheral_i2s.c` — Zephyr `i2s_*`
    with a 2-block ping-pong memory slab.
  - `<alp/can.h>` + `src/zephyr/peripheral_can.c` — Zephyr `can_*`,
    classic + CAN-FD.
  - `<alp/rtc.h>` + `src/zephyr/peripheral_rtc.c` — Zephyr `rtc_*`.
  - `<alp/wdt.h>` + `src/zephyr/peripheral_wdt.c` — Zephyr `wdt_*`.
- **Last-error mechanism for `*_open` failure diagnosis.**  New
  status code `ALP_ERR_OUT_OF_RANGE` (= -8); new public accessor
  `alp_last_error()` (thread-local) lets callers learn *why* a
  failed `*_open` returned NULL — distinguishing
  config-out-of-range, NULL-arg, pool-exhausted, device-not-ready,
  and underlying driver error.  Internal helpers
  `alp_z_set_last_error` / `alp_z_clear_last_error` live in
  `src/zephyr/last_error.c`.
- **SoC capability validation** — `scripts/gen_soc_caps.py` reads
  `metadata/socs/**.json` and emits `include/alp/soc_caps.h` with
  per-SoC `ALP_SOC_*_COUNT` / `ALP_SOC_*_MAX_*` macros gated by
  `CONFIG_ALP_SOC_<TOKEN>`.  v0.2 wrappers consult the matching
  macros to reject configs that exceed the active SoC's documented
  caps before any I/O.  Canonical case: a 16-bit ADC request on
  Alif E3 (12-bit max) fails at `alp_adc_open` with
  `alp_last_error()` = `ALP_ERR_OUT_OF_RANGE`.
- **Kconfig SoC choice** at `zephyr/Kconfig` for the active
  capability profile.  Default `ALP_SOC_NONE` keeps validation
  permissive; alp-studio's generated build picks the matching
  `CONFIG_ALP_SOC_<VENDOR>_<FAMILY>_<PART>=y` automatically.
- **Architecture documentation** — `docs/architecture.md` gains a
  "Why this wrapper exists (despite Zephyr already abstracting
  vendors)" section and a "Capability validation" section.  New
  `docs/adr/` directory with the ADR template plus three accepted
  records:
  - [ADR 0001](docs/adr/0001-wrapper-on-top-of-zephyr.md) — why
    ALP SDK wraps Zephyr (and why the wrapper stays thin).
  - [ADR 0002](docs/adr/0002-error-mechanism.md) —
    `alp_last_error()` + compile-time SoC capability validation.
  - [ADR 0003](docs/adr/0003-peripheral-coverage.md) — wrap 12
    peripheral classes at v0.2, not just I2C/SPI/GPIO/UART.
- **ABI snapshot bumped** to reflect the new headers — 29 headers,
  ~165 functions, ~110 typedefs, ~135 macros (was 21/123/80/114).
- **E1M portability-bound macros** at `<alp/e1m_pinout.h>` —
  `ALP_E1M_<CLASS>_COUNT` constants for each peripheral class
  (I2C=2, SPI=2, UART=2, I2S=2, PDM=2, I3C=1, CAN=1, ETH=1,
  CSI=1, DSI=1, ADC=8, DAC=2, PWM=8, ENC=4, GPIO_IO=26).  These
  are the **portability contract** — apps that use only
  `ALP_E1M_<CLASS><N>` for `N < ALP_E1M_<CLASS>_COUNT` are
  guaranteed cross-SoM compatibility.  Higher indices fall into
  the SoC-specific gap (e.g. RZ/V2N's six CAN channels) which the
  SDK still accepts but apps lose the "swap the SoM" property.
- **[ADR 0004](docs/adr/0004-e1m-portability-bound.md)** — E1M-spec
  instance counts as the portability bound.  Documents the three
  concentric capability tiers: E1M reservation < studio block
  declaration < SoC count < driver array.
- `docs/architecture.md` gains an "E1M as the portability bound"
  section explaining the three-tier validation model.
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions,
  pure rename.  Older history rewritten to drop spurious
  co-author footers per the repo's solo-attribution preference.
- **VERSIONS.md v0.2 row** updated to call out the peripheral
  expansion explicitly (12 wrapped classes), capability validation,
  and the E1M portability bound.
- **Peripheral ztest suite extended** with v0.2 wrapper coverage:
  - 12 generic NULL-arg / out-of-range cases for PWM, ADC, Counter,
    QEnc, I2S, CAN, RTC, WDT, each verifying `alp_last_error()`
    returns the precise reason.
  - New `alp_sdk.peripheral.caps_e3` twister scenario pinning the
    SoC choice to `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3=y` and
    asserting that a 25-bit ADC request exceeds the documented
    24-bit cap and fails at `alp_adc_open` with
    `ALP_ERR_OUT_OF_RANGE` — the canonical case the user
    flagged ("16-bit ADC on a 12-bit SoC needs to fail
    cleanly").
  - `prj.conf` enables `CONFIG_PWM=y` / `CONFIG_ADC=y` /
    `CONFIG_COUNTER=y` / `CONFIG_SENSOR=y` / `CONFIG_I2S=y` /
    `CONFIG_CAN=y` / `CONFIG_RTC=y` / `CONFIG_WATCHDOG=y` /
    `CONFIG_THREAD_LOCAL_STORAGE=y` so the v0.2 wrapper sources
    compile and link in the test app.
- **v0.1 peripherals retrofitted** with `alp_last_error()` +
  SoC-capability validation so the diagnostic story is uniform
  across all 12 wrapped classes:
  `peripheral_i2c.c` / `peripheral_spi.c` / `peripheral_uart.c` /
  `peripheral_gpio.c` now stamp a precise reason
  (ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOT_READY /
  ALP_ERR_NOMEM / ALP_ERR_IO) before returning NULL.  ADR 0002's
  v0.3 "retrofit" follow-up landed early.
- **CI: pr-generated-files.yml** verifies generated artefacts stay
  in sync with their inputs — re-runs `gen_soc_caps.py` and
  `abi_snapshot.py` then fails the PR if `include/alp/soc_caps.h`
  or `docs/abi/v0.1-snapshot.json` differ from the committed
  copies (ignoring the snapshot's `generated` date stamp).
  Catches "metadata edited but soc_caps.h not regenerated" and
  "header added/removed but ABI snapshot not bumped" PR mistakes.
- **PLAN.md §6 Open work** updated — gaps the v0.2 expansion
  closed are kept on the list with strikethrough plus a "closed"
  reference to the matching ADR or implementation, so the audit
  trail of what was promised vs. what shipped stays explicit.
- **Per-peripheral example apps** — nine standalone hand-written
  reference apps under `examples/<peripheral>-<demo>/`, one per
  wrapped peripheral class.  Each is a minimal Zephyr app that
  shows the canonical open / call / close pattern + reads
  `alp_last_error()` on failure.  Twister gates on
  `[<class>] done` console output.  Examples shipped:
  `gpio-button-led`, `i2c-scanner`, `spi-loopback`, `uart-echo`,
  `pwm-led-fade`, `adc-voltmeter` (incl. capability-validation
  rejection demo), `counter-alarm`, `rtc-clock`, `wdt-feed`,
  `can-loopback`.  ADR 0001's "standalone usage is first-class"
  principle made tangible.
- **Standalone usage callouts** — README's "Two consumer paths"
  section explicitly lists hand-written firmware as first-class
  alongside studio-codegen apps.  ADR 0001 expanded with the
  reasoning.  Saved as a persistent project memory so future
  sessions don't accidentally treat hand-written usage as a
  workaround.
- **VS Code support** — README "Using with VS Code" section,
  fresh `.vscode/{extensions,settings,tasks,c_cpp_properties}.json`
  config aligned with the current Zephyr-module + plain-CMake
  layout.  Tasks for `validate metadata`, `regen soc_caps`,
  `regen ABI snapshot`, twister, and `west build` for each
  example.  Extension recommendations are vendor-neutral —
  no Nordic-branded tooling (the SDK targets Alif / Renesas /
  NXP, not Nordic).
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions.
- **CI infrastructure fixes** — bumped
  `ZEPHYR_SDK_INSTALL_DIR` to 0.17.0 to match the
  `ci:v0.27.4` Docker image; switched `--testsuite-root` paths
  from `${{ github.workspace }}` (host path) to
  `$GITHUB_WORKSPACE` (container path); fixed a `/**` sequence
  in the generated `soc_caps.h` comment that triggered
  `-Werror=comment`.  Twister CI was failing on the same three
  seams since the workflow was authored.

- **3 additional chip drivers (E1M EVK on-board sensors)** —
  `chips/icm42670/` (TDK 6-axis IMU), `chips/bmi323/` (Bosch 6-axis
  IMU), `chips/bmp581/` (Bosch barometer with already-compensated
  24-bit P + T outputs).  Each follows the existing chip-driver
  pattern with WHO_AM_I / CHIP_ID verify, ODR + FS / OSR config,
  burst register reads.  Brings the SDK's chip count to **11**.
- **Shared stub backend** at `src/common/stub_backend.c` — every
  public `alp_*` function defined as a NOSUPPORT stub, wired into
  both `src/baremetal/` and `src/yocto/` via `target_sources`.
  Plain-CMake builds with `-DALP_OS=baremetal` or `-DALP_OS=yocto`
  now produce link-complete `libalp_sdk.a`.
- **CI: pr-plain-cmake.yml** — runs `cmake -DALP_OS=baremetal` and
  `cmake -DALP_OS=yocto` against host gcc on every PR; catches
  "new public function added without a stub entry"
  undefined-reference errors that the Zephyr CI doesn't see.
- **`<alp/inference.h>` declared early (was held to v0.2)** —
  unified ML inference surface with backend selector
  (AUTO/CPU/Ethos-U/DRP-AI/DEEPX), model-format selector
  (TFLite/Vela/DRP-AI/DXNN/ExecuTorch), tensor descriptor with
  shape + dtype + quant params.  Stub returns NOSUPPORT; real
  impls land per backend (Ethos-U + CPU v0.2 on AEN-Zephyr,
  DRP-AI v0.3, DEEPX v0.4).  PLAN.md §6 entry strikethrough'd.
- **`<alp/storage.h>` declared early (v0.4 surface)** — block-
  oriented persistent storage: internal flash, QSPI / OSPI NOR,
  SD / eMMC.  open / get_info / read / write / erase / sync /
  close.  Real impls v0.4 (Yocto first-class).
- **`<alp/usb.h>` declared early (v0.3 surface)** — device + host
  roles in one header.  Three stock device classes wrapped
  (CDC-ACM, MSC, HID); other classes via vendor escape hatches.
- **`yocto/meta-alp/` skeleton** — Yocto BSP layer with
  `conf/layer.conf` + recipe shells (`alp-sdk_git.bb`,
  `alp-edgeai_git.bb`).  External Yocto integrators can
  `bitbake-layers add-layer` today; do_compile / do_install /
  FILES wiring lands v0.4.
- **`docs/getting-started.md`** — standalone-first walkthrough
  from `git clone` through `west build -b native_sim` and onto
  real silicon.  Reinforces ADR 0001's
  "standalone is first-class" stance with a concrete recipe
  every hand-written firmware author can follow.
- **EVK on-module audio + mux + current-sense + wake batch** --
  more user-supplied EVK schematic detail:
  - **TAS2563RPP smart-amp pair** -- two amps on I2C0 + I2S0
    (addresses 0x4D and 0x4E, plus broadcast 0x48).  New
    `chips/tas2563/` driver: init / probe / mode_ctrl /
    hw_enable.  Tuning-blob loader + I2S binding land in v0.3.x.
    AMP_ENABLE drives the chip's SD_N (active-high enable);
    AMP_FAULT reads IRQ_N (open-drain, internal pull-up).
    `CONFIG_ALP_SDK_CHIP_TAS2563` opt-in.
  - **Four MP34DT05TR-A PDM mics** -- two pairs on PDM0 and PDM1
    respectively, each pair sharing a PDM data line via the LR
    convention (LR=high samples on rising edge, LR=low on
    falling).  Documented inline; chips/pdm_mic helper covers
    the part.
  - **I2S0 74LVC157 mux** swaps the I2S0 bus between TAS2563 and
    M.2 E-key.  Control pins split across chips:
    `EVK_PIN_I2S_MUX_EN = IO8` (Alif P7.1) +
    `EVK_PIN_I2S_MUX_SEL = IO13` (CC3501E GPIO13).
  - **USB2 TMUXHS221 mux** swaps USB2 between the external USB-A
    jack and M.2 E-key USB.  Single control:
    `EVK_PIN_USB2_MUX_SEL = IO11` (CC3501E GPIO2).  /OEN
    tied to GND so the mux is always live.
  - **M.2 E-key UART wake** -- `EVK_PIN_M2E_UART_WAKE = IO19`
    (CC3501E GPIO19).
  - **Six INA236 current shunt monitors** -- one per power rail
    on I2C0 (3V3, 1V8, VIO, +V_CAM0, +V_CAM1, 5V).  INA236A
    occupies 0x40..0x43 and INA236B occupies 0x44..0x47, so all
    six fit on one bus.  Macros key on rail name; the user's
    notes had a ref-des typo (three "U30"s) -- cross-check
    pending.
  - **ENCODER_SW correction** -- moves from IO3 to IO4 per the
    schematic.  PEC12R-4222F-S0024 encoder + push switch.
  ABI: ADDED public header `alp/chips/tas2563.h` (snapshot now
  44 public headers).  SDK chip count climbs to **19**.

- **EVK Arduino + mikroBUS headers + wiring corrections** --
  user-supplied EVK wiring continues.  Arduino UNO header full
  pin map: `EVK_ARD_PWM1..PWM4` (= E1M PWM1/PWM4/PWM5/PWM2),
  `EVK_ARD_DIO1..DIO4` (= I2S1_SDO / I2S1_WS / SPI0_SCLK /
  SPI0_MOSI), `EVK_ARD_RST` (= I2S1_SCLK), and
  `EVK_ARD_A0..A5` (= ALP_E1M_ADC0..5).  Bus aliases:
  `EVK_SPI_BUS_ARDUINO` (= SPI1, CC3501E-mediated),
  `EVK_I2C_BUS_ARDUINO` (= I3C0), `EVK_UART_PORT_ARDUINO`
  (= UART1).  mikroBUS click header reuses the Arduino macros for
  the shared lines (SPI / I2C / UART / RST) and adds
  `EVK_MB_PWM` (= PWM6), `EVK_PIN_MB_INT` (= I2S1_SDI),
  `EVK_MB_ANA` (TBD-confirm).  In-flight wiring corrections
  bundled into the same commit:
  - **LED_GREEN -> PWM0** (was PWM2 inferred-and-wrong).
  - **CTP_INT -> SPI1_CS1** (was I2S1_SDI; the I2S1_SDI pad now
    correctly serves only as mikroBUS INT).  CTP_INT routes
    through the on-module CC3501E like the other CC3501E-side
    pins.
  - **CTP_RST is the TCAL9538 P3 only.**  An earlier draft also
    listed SPI1_CS0 as CTP_RST; the user clarified that's a
    mis-label -- SPI1_CS0 is the Arduino CK_CS (chip select).
  - **BMI323 address -> 0x68** (with SDO=low; user confirmed the
    strap is low, not high).  Resolves the apparent 0x69
    collision with ICM-42670-P.
  - **`EVK_SPI_BUS_M2_KEYM` removed** -- M.2 on this EVK
    uses PCIe + SDIO, not SPI; the previous macro was a guess.
  - **SPI0 fully repurposed** -- all five SPI0 pads
    (MISO/CS0/CS1/MOSI/SCLK) are GPIOs on this carrier
    (AMP_FAULT/AMP_ENABLE/IO_EXP_RST/CK_DIO4/CK_DIO3).  No
    peripheral SPI0 bus available on the EVK; documented in the
    header.
  - **I2S1 fully repurposed** -- all four I2S1 pads
    (SDO/WS/SDI/SCLK) are GPIOs (CK_DIO1/CK_DIO2/MB_INT/CK_RST).
    No peripheral I2S1 bus on the EVK; the I2S0 path remains
    available for audio.
  ABI snapshot still 43 public headers (no shape change).
- **EVK wiring batch: I2C1 = DSI/CSI control, IO5 = CAM_RST,
  PWM-driven RGB LED, repurposed SPI0 + I2S1 + AUDIO_CLK pads,
  TCAL9538 I/O expander, SDIO mux, touch-screen control** --
  large user-supplied EVK schematic update lands in
  `<alp/boards/alp_e1m_evk.h>` + `chips/tcal9538/`:
  - I2C0 stays the sensor / IO-expander / current-monitor bus;
    new `EVK_I2C_BUS_DSI_CSI = ALP_E1M_I2C1` is the
    display-and-camera-control bus per the EVK's `DSI_CSI_I2C` net.
  - `IO5` is `CAM_RST` (the camera reset line), correcting an
    earlier placeholder that had `IO5 = IO_EXP_RST`.
  - RGB LED now drives via PWM rather than GPIO:
    `EVK_PWM_LED_RED = ALP_E1M_PWM3`,
    `_GREEN = ALP_E1M_PWM2` (TBD-confirm; user stated PWM3=R + PWM1=B
    but didn't name the green PWM, PWM2 is inferred), and
    `_BLUE = ALP_E1M_PWM1`.
  - Off-GPIO_IO repurposed pads (`AUDIO_CLK`, `SPI0_MISO`,
    `SPI0_CS0`, `SPI0_CS1`, `I2S1_SDI`, `SPI1_CS0`) are exposed
    via `EVK_PIN_OVERLAY_BASE + N` indices that the carrier
    `alp,pin-array` overlay extends past the standard 42-entry
    range.  Names: `EVK_PIN_IO_EXP_INT` (AUDIO_CLK),
    `_IO_EXP_RST` (SPI0_CS1), `_AMP_FAULT` (SPI0_MISO),
    `_AMP_ENABLE` (SPI0_CS0), `_CTP_INT` (I2S1_SDI),
    `_CTP_RST` (SPI1_CS0, routed through CC3501E).
  - SDIO mux for SD-card vs M.2 E-key: 74LVC157 controlled by
    `EVK_PIN_SDIO_MUX_EN = ALP_E1M_GPIO_IO20` (active-low
    enable) and `EVK_PIN_SDIO_MUX_SEL = ALP_E1M_GPIO_IO21`
    (0 = M.2 E SDIO, 1 = SD card).  Both pins route through the
    on-module CC3501E (per from-cc3501e.tsv), so firmware drives
    the mux via `ALP_CC3501E_CMD_GPIO_WRITE` on the inter-chip
    SPI1, NOT through Alif's GPIO peripheral.  Noted in the
    header.
  - On-board sensor I2C addresses corrected per user-supplied
    strap values: ICM-42670-P AD0=high -> 0x69 (was 0x2C
    placeholder), BMI323 SDO=high -> 0x69 (was 0x68 placeholder),
    BMP581 SDO=high -> 0x47, TCAL9538 A1=1/A0=0 -> 0x72 (was 0x70).
    **Address collision warning**: ICM-42670-P and BMI323 both
    compute to 0x69 with the documented straps -- header carries
    a TODO comment asking the user to confirm whether only one
    IMU is populated at a time or whether the schematic strap is
    actually different.
  - **TCAL9538 I/O expander pin map** (8 channels of LCD / camera /
    capacitive-touch control + sensor interrupts) materialised as
    a typed enum `evk_ioexp_pin_t` so apps don't pass raw
    indices.  Note: P3 was earlier listed as `CTP_RST` but the
    user has since separately said `CTP_RST = SPI1_CS0`; both
    routes are preserved in the header with a TBD-confirm note
    flagging the conflict.
  - New `chips/tcal9538/` driver -- TI TCA9538 / TCAL9538
    8-channel I/O expander.  Per-pin and bulk
    direction / write / read APIs with cached register state
    (avoids read-modify-write round-trips on every set call).
    `CONFIG_ALP_SDK_CHIP_TCAL9538` opt-in.
  ABI snapshot now 43 public headers; SDK chip count climbs to
  **18**.
- **EVK MIPI CSI camera-mux helper + IO2 wiring correction** --
  the EVK routes a single MIPI CSI lane pair from the SoM through
  a **PI3WVR626XEBEX** 2:1 mux (camera A vs camera B); its
  `SEL` pin lands on E1M `IO2` (`W2` / Alif `P12.5`).  New thin
  driver at `chips/cam_mux_pi3wvr626/` + matching public header
  wraps SEL as a GPIO with a typed `INPUT_A` / `INPUT_B` enum.
  `<alp/boards/alp_e1m_evk.h>` gains `EVK_PIN_CAM_MUX_SEL`
  and **drops the previous placeholder `EVK_PIN_LED_BLUE = IO2`
  mapping** -- the schematic confirms `IO2` is the mux line, not
  an LED.  `LED_BLUE` is now TBD; `LED_RED` / `LED_GREEN` carry
  an explicit "TBD-confirm" caveat.  `docs/boards/e1m-evk.md`
  picks up the camera-mux details + truth table.  SDK chip count
  climbs to **17**; ABI snapshot to 42 headers.
- **On-module I2C device drivers (E1M-AEN)** -- four new chip
  drivers covering the SoM-internal I2C devices the user
  documented:
  - `chips/tmp112/` + `<alp/chips/tmp112.h>` -- TI TMP112 digital
    temperature sensor (LPI2C, +/-0.5 C, 12/13-bit, 0.0625 C/LSB).
    Init / set_rate / set_extended_mode /
    read_temp_milli_c (avoids float on M-class).
  - `chips/rv3028c7/` + `<alp/chips/rv3028c7.h>` -- Micro Crystal
    RV-3028-C7 32.768 kHz 1 PPM RTC (LPI2C, addr 0x52 fixed).
    BCD time get/set, alarm with match-mask, INT-pin enable,
    alarm flag check+clear.  Routes the alarm into Alif
    `P15_0_FLEX` per the inter-chip wiring TSV.
  - `chips/optiga_trust_m/` + `<alp/chips/optiga_trust_m.h>` --
    Infineon OPTIGA Trust M secure element
    (SLS32AIA010MLUSON10XTMA2; LPI2C, addr 0x30).  v0.3 ships
    init + I2C connectivity-probe; full APDU command set lands
    via Infineon's host library + MbedTLS PSA registration in
    v0.3.x so `<alp/security.h>` picks up HW acceleration
    transparently.
  - `chips/eeprom_24c128/` + `<alp/chips/eeprom_24c128.h>` --
    Generic 24Cxx-class 128-Kbit (16 KB) EEPROM driver covering
    Onsemi N24S128C4DYT3G (E1M-AEN default) and the
    footprint-compatible STMicro M24128-BFMH6TG (DNP alt).
    Page-aware writes (64-byte page, ACK-poll wait), arbitrary-
    range reads.  On I2C2.
  Brings the SDK's chip count to **16** (was 12).  ABI snapshot
  now 41 public headers.
- **Secure boot + secure OTA roadmap entries (v0.4)** -- production
  E1M deployments need chain-of-trust from immutable ROM up.
  Plan: MCUboot already pulled in via west.yml verifies
  application image signatures at every boot; signing keys live
  in OPTIGA Trust M's secure NVM (provisioned during factory
  programming); the verification path goes through MbedTLS PSA,
  routing to the OPTIGA hardware accelerator transparently once
  the v0.3.x PSA driver lands.  Secure OTA delivers signed
  payloads over `<alp/iot.h>` MQTT/HTTP and swaps via MCUboot's
  `swap-using-scratch` mode.  Yocto-on-V2N / i.MX 93 uses
  `meta-mender` for the equivalent.  Documented in `PLAN.md` §2.4.1
  and `VERSIONS.md` v0.4 section.
- **CC3501E Alif-side host driver** -- new chip driver at
  `chips/cc3501e/cc3501e.c` + public header
  `include/alp/chips/cc3501e.h`.  Alif-side of the inter-chip
  bridge: `cc3501e_init` / `_reset` / `_get_version` /
  `_request` / `_set_event_callback` / `_deinit`.  Synchronous
  `cc3501e_request` frames a header + payload, drives SPI1 via
  `alp_spi_transceive`, copies the response back out.  Reset
  pulses `WIFI.EN` (P15_5) + `E_WIFI.NRST` (P15_1_FLEX) via
  `alp_gpio_*` when those pins are populated.  New
  `CONFIG_ALP_SDK_CHIP_CC3501E` Kconfig (depends on `SPI && GPIO`).
  SDK's chip count climbs to **12**.  ABI snapshot now 37
  headers.  Followed in a later commit by the
  `<alp/iot.h>` / `<alp/ble.h>` dispatcher branches that route
  through this driver on E1M-AEN.
- **CC3501E wire protocol + bridge architecture** -- the E1M-AEN
  family carries a separate TI CC3501E Wi-Fi 6 + BLE 5.4 combo
  MCU, and certain E1M pads (SPI1 + IO11 / IO13 / IO15..21 + the
  camera-enable LDOs) terminate on it rather than on the Alif
  silicon.  Communication runs over an inter-chip SPI1 bus
  (Alif master, CC3501E slave) carrying a custom binary wire
  protocol.  New `include/alp/protocol/cc3501e.h` freezes that
  protocol at v1: 4-byte header + <=512 B payload, opcodes grouped
  by subsystem (meta / Wi-Fi / sockets / BLE / GPIO proxy /
  camera enable / diagnostics), per-subsystem payload structs
  (Wi-Fi connect, BLE adv start, GPIO configure, ...).
  `docs/cc3501e-bridge.md` documents the architecture, the
  two-repo split per ADR 0005 (alp-sdk owns the protocol +
  Alif-side client; the firmware that runs on the CC3501E lives
  in `alplabai/cc3501e-firmware`), and the firmware bootstrap
  recipe.  Alif-side `chips/cc3501e/` driver +
  `<alp/iot.h>` / `<alp/ble.h>` route-through-CC3501E follow.
- **Authoritative E1M-AEN pinout data** (item: pending HW configs) --
  five user-supplied TSVs land at `metadata/e1m_modules/aen/`:
  `from-alif.tsv` (E1M pad -> Alif silicon, 91 routed entries),
  `from-cc3501e.tsv` (E1M pad -> CC3501E, 14 entries),
  `inter-chip.tsv` (Alif <-> CC3501E SPI1 + SDIO + control
  signals + camera-enable wiring), `alif-ospi.tsv` (Alif's OSPI
  controller pad reservation for optional on-module memories),
  `alif-ethernet-phy.tsv` (DP83825IRMQR PHY <-> Alif RMII MAC
  pad map).  Resolves the AEN-side TBDs in
  `project_pending_hw_configs` memory note; downstream artefacts
  (`include/alp/boards/alp_e1m_evk.h`, the per-SoC metadata
  in `metadata/socs/alif/ensemble/`, alp-studio's pin allocator)
  can now regenerate against this single source of truth.  V2N +
  i.MX 93 family pinouts remain pending until the user supplies them.
- **DRP-AI inference dispatcher hook (item 11 / v0.3 milestone)** --
  `<alp/inference.h>` dispatcher in `src/zephyr/inference_zephyr.c`
  now routes `ALP_INFERENCE_BACKEND_DRPAI` into a per-backend
  `inference_drpai.c` source compiled when
  `CONFIG_ALP_SDK_INFERENCE_DRPAI=y`.  The AUTO resolver prefers
  DRPAI before falling through to TFLM-CPU.  v0.3 ships the
  dispatch routing + a 16-byte placeholder open() so apps verify
  routing today; v0.4 fills the body via Renesas's DRP-AI
  translator runtime once the vendor pack is in CI.
- **V2N vendor scaffolding (item 10 / v0.3 milestone)** --
  `vendors/renesas-rzv2n/` gains `i2c.c` / `spi.c` / `gpio.c` /
  `uart.c` source skeletons mirroring the Alif vendor layout.
  i2c.c carries the full FSP `r_riic_master` binding shape; the
  other three are starter shells with the FSP includes wired in
  for v0.4 to fill bodies.  `vendors/renesas-rzv2n/CMakeLists.txt`
  follows the same `ALP_HAS_RENESAS_FSP` gate as Alif, so the
  files compile to empty translation units when the FSP pack
  isn't present.  `src/baremetal/CMakeLists.txt` extends the
  ALP_SOM dispatch with a v2n branch; `pr-plain-cmake.yml` gains
  a `baremetal-v2n` scenario verifying the dispatch + scaffolding
  build green without the proprietary FSP pack.  Real FSP link
  follows in v0.4 alongside the V2N HIL bring-up.
- **`<alp/usb.h>` real device-stack lifecycle wrapper (v0.3 milestone)** --
  `src/zephyr/usb_zephyr.c` replaces the v0.1 NOSUPPORT stub.  Wraps
  Zephyr's `usb_enable` / `usb_disable` for the device-role
  lifecycle.  Per-class endpoint read/write (CDC-ACM, MSC, HID)
  and host-role bring-up land in v0.3.x.  New
  `CONFIG_ALP_SDK_USB` (depends on `USB_DEVICE_STACK`, default y),
  `CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES` (default 1).
  `tests/zephyr/usb/` smoke suite (5 cases).
- **`<alp/mproc.h>` real Zephyr MBOX wrapper (v0.3 milestone)** --
  `src/zephyr/mproc_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's MBOX driver class (Alif's MHU is registered as
  a Zephyr mbox device on AEN) for the mailbox path:
  `mbox_init_channel` + `mbox_send` + `mbox_register_callback`.
  DT-anchored shared-memory regions (`alp-shmemN` aliases) and
  per-SoC HWSEM register access return NOSUPPORT until v0.3.x
  finishes the EVK overlay and the Alif HWSEM driver.  New
  Kconfig: `CONFIG_ALP_SDK_MPROC` (depends on MBOX, default y),
  pool-size knobs (`MAX_SHMEM_HANDLES`, `MAX_MBOX_HANDLES`,
  `MAX_HWSEM_HANDLES`).  `tests/zephyr/mproc/` smoke suite
  (7 cases).
- **`<alp/security.h>` real MbedTLS PSA Crypto wrapper (v0.3 milestone)** --
  `src/zephyr/security_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps PSA Crypto (`psa_crypto_init`, `psa_hash_*`, `psa_aead_*`,
  `psa_generate_random`) -- when MbedTLS is built with the
  vendor's HW-accelerator driver registered, PSA routes to the
  Alif Ensemble crypto subsystem (E7/E8) or the Renesas RZ/V2N
  RSIP automatically without code changes here.  Hash covers
  SHA-256/384/512; AEAD covers AES-128/256-GCM and
  ChaCha20-Poly1305; TRNG via `psa_generate_random`.  New
  Kconfig: `CONFIG_ALP_SDK_SECURITY` (depends on
  `MBEDTLS_PSA_CRYPTO_C`, default y),
  `CONFIG_ALP_SDK_MAX_HASH_HANDLES` / `_AEAD_HANDLES` (default 4
  each).  AEAD encrypt/decrypt path uses a 4 KiB stack scratch
  buffer (heap fall-back lands in v0.3.x for larger blobs).
  `tests/zephyr/security/` smoke suite (7 cases).
- **`<alp/ble.h>` real Zephyr `bt`-host wrapper (v0.3 milestone)** --
  `src/zephyr/ble_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's `bt` host stack: `bt_enable` for the singleton
  controller, `bt_le_adv_start` / `bt_le_adv_stop` for the
  peripheral role, `bt_le_scan_start` / `bt_le_scan_stop` with a
  per-packet `recv` callback for the central role, and
  `bt_conn_le_create` / `bt_conn_disconnect` for connections.
  Gated on new `CONFIG_ALP_SDK_BLE` (depends on `BT`, default y).
  GATT runtime registration + sync read/write helpers stay
  NOSUPPORT until v0.3.x lands a callback-with-semaphore shim
  (Zephyr's GATT API is async-only; the public surface is
  synchronous).  New `CONFIG_ALP_SDK_BLE_MAX_CONNS` (default 2)
  sizes the connection-handle pool.  `tests/zephyr/ble/` smoke
  suite (6 cases) verifies the NOSUPPORT-fall-back contract +
  every NULL-arg branch under native_sim.
- **Bare-metal AEN backend scaffolding (v0.2 milestone, item 4)** --
  v0.2's "stub-to-real" baseline for the bare-metal Alif Ensemble
  path.  New `vendors/alif/{i2c,spi,gpio,uart}.c` source files
  wrap the four core peripherals on Alif's CMSIS-Driver layer
  (`Driver_I2C0..3`, `Driver_SPI0..3`, `Driver_GPIO0..7`,
  `Driver_USART0..3`).  Each wrapper's body is gated on
  `ALP_HAS_ALIF_HAL` so the files compile cleanly even without
  the proprietary Ensemble CMSIS pack present.  When the option
  flips ON the wrapper sets `ALP_VENDOR_OVERRIDES_PERIPHERAL`,
  which excludes the matching stubs from `src/common/stub_backend.c`
  -- no duplicate symbols.  CMake wiring: new top-level
  `ALP_SOM` variable (`none|aen|v2n|imx93`) layered on top of
  `ALP_OS`; `src/baremetal/CMakeLists.txt` pulls
  `vendors/alif/CMakeLists.txt` only when `ALP_OS=baremetal`
  AND `ALP_SOM=aen`, leaving non-AEN bare-metal builds on the
  pure stub path.  `pr-plain-cmake.yml` gains a `baremetal-aen`
  job that exercises the new dispatch.  Real HAL link follows in
  v0.2.x via `cpackget add AlifSemiconductor::Ensemble` -- the
  scaffolding here lets that change be a body-of-function-only
  follow-up.
- **Cross-platform development support made explicit** -- README
  gains a "Cross-platform development" table calling out
  first-class macOS / Windows / Linux support; `docs/getting-started.md`
  prerequisite section adds per-OS install one-liners (Homebrew,
  apt, winget, WSL).  No code changes -- the toolchain (Zephyr +
  west + CMake + Python + GCC) was already cross-platform; the
  documentation just admits it now.  New `.gitattributes` pins
  LF endings on every source file so a Windows checkout, a macOS
  clone, and a Linux pull see and commit identical bytes --
  avoids clang-format-diff CI failures when a developer crosses
  hosts.
- **[ADR 0005](docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md)** —
  alp-sdk vs alp-studio repo boundary.  Codifies the dual-use
  acid test ("would a hand-written-firmware author ever directly
  use this?") and the canonical table of which artefacts belong
  in which repo.  `docs/architecture.md` gains a "Repository
  boundary" subsection so reviewers can resolve where a new
  addition belongs without opening the ADR.
- **`<alp/inference.h>` real wrapper + TFLM/Ethos-U C++ executor**
  — v0.2's third "stub-to-real" milestone.  `src/zephyr/inference_zephyr.c`
  replaces the v0.1 NOSUPPORT stub with the public-API glue: handle
  pool (default 2), capability validation, backend selector
  (AUTO / CPU / ETHOS_U / DRPAI / DEEPX_DX), and dispatch into a
  per-backend executor.  `src/zephyr/inference_tflm.cpp` -- new C++
  source compiled only when `CONFIG_ALP_SDK_INFERENCE_TFLM=y` (depends
  on `TENSORFLOW_LITE_MICRO`) -- wraps `tflite::MicroInterpreter`
  and `tflite::MicroMutableOpResolver<32>` with the canonical
  MobileNetV2 op set + the Arm `AddEthosU()` registration so
  Vela-compiled `.tflite` files dispatch their ETHOS-U custom op
  to the NPU.  Default 128 KiB tensor arena (caller-supplied via
  `cfg.arena` overrides).  The CPU backend uses TFLM's reference
  kernels for any layer Vela leaves on-host plus pure CPU models;
  ETHOS_U routes through the same MicroInterpreter with the op
  resolver picking the NPU op when available.  `west.yml`
  name-allowlist gains `tflite-micro` so `west update` pulls the
  module on every refresh; CI's native_sim builds keep
  `CONFIG_ALP_SDK_INFERENCE_TFLM=n` and the wrapper falls back to
  ALP_ERR_NOSUPPORT cleanly.  New Kconfig options:
  `CONFIG_ALP_SDK_INFERENCE_TFLM` (default n; enables the C++
  executor), `CONFIG_ALP_SDK_INFERENCE_ETHOS_U` (default y if
  TFLM and SOC_SERIES_ENSEMBLE_E7), `CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES`
  (default 2).  The `examples/edgeai-vision-aen/` example now
  wires `alp_inference_open` into stage 3 (model load) using
  `ALP_INFERENCE_BACKEND_AUTO` -- skips on native_sim with the
  precise NOSUPPORT diagnostic, runs the real Vela model on AEN HW
  once the toolchain output lands under `models/`.  New
  `tests/zephyr/inference/` ztest suite (7 cases) verifies the
  cfg-validation + NOSUPPORT-fall-back contract under native_sim.
- **`<alp/audio.h>` real PDM input + I²S output on AEN-Zephyr** —
  v0.2's second "stub-to-real" milestone.  `src/zephyr/audio_zephyr.c`
  replaces the v0.1 `audio_stub.c` and splits in two halves: a
  `audio_in` path wrapping Zephyr's `audio_dmic` API
  (`dmic_configure` / `dmic_trigger` / `dmic_read`), and a
  `audio_out` path that delegates straight to the existing
  `alp_i2s_*` wrapper (TX direction, frame translation in the
  config).  Two Kconfig toggles -- `CONFIG_ALP_SDK_AUDIO_IN`
  (depends on `AUDIO_DMIC`, default y) and `CONFIG_ALP_SDK_AUDIO_OUT`
  (depends on `I2S`, default y) -- gate the real glue behind the
  v0.1 NULL-with-NOSUPPORT contract for native_sim and any other
  no-audio target.  The PDM path runs the ALP DSP chain on every
  block: a 1st-order DC-block (alpha = 0.995 in Q15, ~10 Hz
  cut-off at 16 kHz) silences the DC bias every PDM mic carries.
  AGC and resample land in v0.3 alongside `<alp/security.h>`; the
  hook stays in `dmic_read`'s post-pass so v0.3 only adds passes.
  The output path adds a Q8.8 software volume scale (default
  unity, `alp_audio_out_set_volume(out, vol)` from 0..255) that
  applies before `alp_i2s_write` -- usable on codecs without a
  separate gain pin.  Pool sizes
  (`CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES` / `_OUT_HANDLES`, both
  default 1) and the per-handle slab (4 KiB *
  `CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS`, default 4 blocks) are
  statically allocated -- no `k_malloc` in the audio path.  New
  `examples/audio-loopback/` hand-written reference: opens
  `ALP_E1M_PDM0` + `ALP_E1M_I2S0`, streams 50 blocks of 256
  S16-mono frames from mic to speaker at 16 kHz, prints
  `[audio] done`.  Skips gracefully on native_sim (no audio
  devices) so the existing twister scenario continues to pass.
  New `tests/zephyr/audio/` ztest suite (9 cases) verifies the
  NOSUPPORT-fall-back contract under native_sim plus every
  NULL-arg / invalid-config branch.
- **`<alp/iot.h>` real Wi-Fi station + MQTT on AEN-Zephyr** —
  v0.2's first "stub-to-real" milestone.  `src/zephyr/iot_zephyr.c`
  replaces the v0.1 `iot_stub.c` and wraps Zephyr's `wifi_mgmt`
  (`net_mgmt(NET_REQUEST_WIFI_CONNECT, …)`) plus `mqtt_client`
  (`mqtt_connect`, `mqtt_publish`, `mqtt_subscribe`, `mqtt_input`,
  `mqtt_live`).  Two new Kconfig toggles --
  `CONFIG_ALP_SDK_IOT_WIFI` (depends on `WIFI && NET_MGMT`,
  default y) and `CONFIG_ALP_SDK_IOT_MQTT` (depends on
  `MQTT_LIB && NET_TCP`, default y) -- gate the real glue.  When
  neither is on (host build, native_sim, any target without a
  Wi-Fi/TCP stack) the wrapper honours the v0.1 contract:
  `*_open()` returns NULL with `alp_last_error()` =
  `ALP_ERR_NOSUPPORT`.  Static handle pools sized via
  `CONFIG_ALP_SDK_MAX_WIFI_HANDLES` (default 1, single-radio per
  E1M SoM) and `CONFIG_ALP_SDK_MAX_MQTT_HANDLES` (default 2),
  with `CONFIG_ALP_SDK_MQTT_BUF_SIZE` (default 256 B) controlling
  the per-client rx / tx / topic scratch.  `examples/iot-connected-camera/`
  ships a new `overlay-aen.conf` that pulls in `CONFIG_NET_*`,
  `CONFIG_WIFI`, `CONFIG_WIFI_NM_WPA_SUPPLICANT`, `CONFIG_MQTT_LIB`
  for AEN builds; the base `prj.conf` remains native_sim-friendly
  and the example continues to print `[iotcam] done` end-to-end.
  New `tests/zephyr/iot/` ztest suite (12 cases) verifies the
  NOSUPPORT-fall-back contract + every NULL-arg branch, bringing
  twister's `native_sim/native/64` total to **64 cases** (52 + 12).

### Notes

- Repo migrated from a CMSIS-Toolbox csolution / VFT mock-driver
  layout to the OS-pivoted layout described in `docs/architecture.md`.
  The previous tree is preserved in git history.
- Apache-2.0 license replaces the original MIT badge.
- Twister build hit a Kconfig-discovery issue (the alp-sdk module
  surface isn't being merged) — fix tracked, will be green before tag.
