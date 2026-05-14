# Glossary

Terms a firmware engineer will encounter in the ALP SDK and
in E1M-X module documentation.

## A-D

**ADC** -- Analog-to-Digital Converter.  The SDK's `<alp/adc.h>`
abstracts the SoC's ADC peripheral.

**AEN** -- ALP Lab module family based on **Alif Ensemble**
silicon (E3..E8).  See [`docs/soms/aen.md`](soms/aen.md).

**alp-studio** -- The optional GUI / codegen tool that emits
`<alp/...>` calls from block manifests + a pin allocator.  See
[`github.com/alplabai/alp-studio`](https://github.com/alplabai/alp-studio).

**Block** -- An alp-studio concept: a reusable feature unit
(button-LED, OLED display, IMU read) that the studio's pin
allocator places against the active SoM.

**board.yaml** -- The single declarative file at the root of every
application.  Lists `som.sku`, `carrier.name`, `os`, `peripherals`,
`chips`, etc.  Validated by `scripts/validate_board_yaml.py`.

**BRD_I2C** -- Board-management I²C bus on V2N + V2N-M1.  Hosts
the PMICs, RTC, OPTIGA, supervisor MCU slave interface.

**Bridge (GD32)** -- The V2N module's on-module supervisor MCU
(GD32G553) reachable over a hybrid SPI + I2C transport.  See
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md).

**Carrier** -- A board that an E1M SoM plugs into.  The SDK ships
presets for the E1M-EVK + E1M-X-EVK reference carriers.

**Chip driver** -- A non-OS-specific C module under `chips/<part>/`
that wraps a single silicon part's I²C / SPI / GPIO surface.
Symbols use the chip's natural name (e.g. `lsm6dso_init`); the
`alp_` prefix is reserved for SDK-level abstractions.

**CMI** -- Code Matrix Index, Qorvo's term for the ACT88760 PMIC's
configuration profile.  V2N populates the **CMI 120.E1** variant.

**DEEPX DX-M1** -- An on-module AI accelerator populated on V2N-M1
SKUs only.  See [`docs/soms/v2n-m1.md`](soms/v2n-m1.md).

**DRP-AI3** -- Renesas's on-die NPU on the RZ/V2N silicon, exposed
through the SDK's `<alp/inference.h>` dispatcher when
`ALP_SDK_INFERENCE_DRPAI=y`.

## E-K

**E1M** -- 35 × 35 mm SoM form factor (312 pads).  AEN + i.MX 93
families ship in this size.

**E1M-X** -- 45 × 65 mm SoM form factor (496 pads).  V2N family.

**EVK** -- Evaluation Kit.  The reference carrier ALP Lab ships for
bring-up.  Two flavours: E1M-EVK (35 × 35) and E1M-X-EVK (45 × 65).

**Ethos-U** -- Arm's micro-NPU IP.  AEN modules carry Ethos-U55;
N93 modules carry Ethos-U65.

**`<family>/hw-revisions.yaml`** -- Per-rev SDK-version compatibility
table.  Customer's `som.hw_rev` is validated against this list at
build time.

**Hand-written firmware** -- Application code that calls
`<alp/...>` directly without alp-studio's codegen.  First-class
consumer path -- not a fallback.

**IDCODE** -- The 32-bit Arm Coresight SW-DP identification value
returned by the target on the first SWD read after a line reset.
Documented as `0x6BA02477` for the GD32G553 (Cortex-M33 r0p1
SW-DPv2); used by `gd32_swd_connect` to confirm the link reaches
the right silicon.

**HiL** -- Hardware-in-the-Loop testing.  See
[`docs/hil-plan.md`](hil-plan.md).

**hw_rev** -- Hardware revision label (e.g. `r1`, `r2`, ...).
Distinguishes board respins of the same SKU.

**hw_info** -- Runtime structure populated from the on-module
EEPROM manifest + BOARD_ID ADC.  See
[`<alp/hw_info.h>`](../include/alp/hw_info.h).

**Kconfig** -- Linux kernel's configuration language.  Zephyr +
the SDK use it for per-feature opt-in.

## L-P

**Loader** -- `scripts/alp_project.py` -- reads `board.yaml`,
resolves SoM SKU preset + carrier preset, emits the per-backend
config (Zephyr `alp.conf` / CMake `-D` flags / Yocto `local.conf`).

**MDIO** -- Management Data Input/Output.  The clause-22 bus used
to configure Ethernet PHYs.

**`metadata/`** -- Repo subtree carrying chip manifests, SoC
capability profiles, per-SKU presets, schemas, templates.

**OPTIGA Trust M** -- Infineon's secure-element IC populated on
every E1M SoM.  Exposed via `<alp/chips/optiga_trust_m.h>` for
direct use, or via `<alp/security.h>` for MbedTLS PSA Crypto
integration.

**OTA** -- Over-the-Air firmware update.  Device-side contract:
[`docs/ota-device-contract.md`](ota-device-contract.md).  Server
side is a separate repo, owned by Hakan.

**OTA opcodes (bridge)** -- The 0xF0..0xF6 opcode range reserved
in [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10
for the GD32 bridge's application-bootloader upgrade path.  Host
helpers in `<alp/chips/gd32g553.h>` (`gd32g553_ota_begin`,
`_write_chunk`, `_verify`, `_commit`, `_rollback`, `_get_state`,
`_abort`).

**PMIC** -- Power Management IC.  V2N + V2N-M1 carry two:
ACT88760 (primary) + DA9292 (secondary).

**`prj.conf`** -- Zephyr's per-application Kconfig fragment.
Mostly empty in SDK examples -- the loader emits the real config
via `OVERLAY_CONFIG`.

## Q-Z

**RGMII** -- Reduced Gigabit Media Independent Interface.  Used
between the Renesas Ethernet MAC and the V2N module's two
RTL8211FDI PHYs.

**RIIC** -- Renesas's I²C peripheral name.  `RIIC8` on the V2N is
the BRD_I2C master.

**RZ/V2N** -- Renesas's vision/AI MPU family used on V2N modules.
Quad Cortex-A55 + Cortex-M33 + DRP-AI3.

**Sample.yaml** -- Zephyr Twister test scenario metadata next to
an application's source.  Optional; required to run as a Twister
target.

**SWD** -- Serial Wire Debug.  Arm's two-wire (SWDIO + SWCLK)
debug protocol used to reflash + halt Cortex-M targets.  The SDK
ships a bit-bang controller (`chips/gd32_swd/`) that lets the V2N
host reflash the on-module GD32G553 over three GPIOs -- see
[`docs/bring-up-v2n.md`](bring-up-v2n.md) §2b for the recovery
flow.

**Silicon ref** -- Triple-colon string identifying SoC silicon
(e.g. `renesas:rzv2n:n44`, `alif:ensemble:e7`, `nxp:imx9:imx93`).
Used in the per-SKU SoM preset (`E1M-<MPN>.yaml`) and
`<alp/soc_caps.h>` selection.

**SKU** -- Stock-Keeping Unit.  In ALP terminology: an MPN that
identifies a specific SoM configuration (e.g. `E1M-V2N101`).

**SoC** -- System-on-Chip.  The main silicon under a SoM's lid.

**SoM** -- System-on-Module.  ALP's per-SoC PCB module that plugs
into a carrier (e.g. E1M-V2N101, E1M-AEN701).

**SoM preset** (`E1M-<MPN>.yaml`) -- Per-SKU manifest declaring
silicon, populated chips, I²C device addresses, memory specs,
default carrier.  Lives at `metadata/e1m_modules/<SKU>.yaml`
(e.g. `E1M-AEN701.yaml`, `E1M-V2N101.yaml`).  Earlier docs
called this `som.yaml`; the file name carries the SKU now so
each preset is distinguishable in a directory listing.

**Supervisor MCU** -- The GD32G553 companion microcontroller on
V2N modules.  Owns peripherals that don't fit on the main SoC's
pinmux.  See [`docs/gd32-bridge.md`](gd32-bridge.md).

**TBD** -- "To be determined".  Used in metadata where the
authoritative value is pending (e.g. a board-rev divider voltage).

**V2N** -- ALP module family based on Renesas RZ/V2N silicon.
E1M-X form factor.  See [`docs/soms/v2n.md`](soms/v2n.md).

**V2N-M1** -- V2N variant with the DEEPX DX-M1 NPU on-module.
SKUs `E1M-V2M101` / `E1M-V2M102`.  See
[`docs/soms/v2n-m1.md`](soms/v2n-m1.md).

**west** -- Zephyr's meta-tool for workspace management +
sub-commands.  The SDK ships an extension command `west alp-build`
that pre-flights `board.yaml` validation before delegating to
`west build`.

**Wi-Fi 6** -- 802.11ax.  AEN's CC3501E + V2N's Murata module
both support it.

**Yocto** -- Embedded Linux build system.  Selected via
`os: yocto` in `board.yaml`; v0.4 deliverable per the test plan.

**Zephyr** -- The RTOS the SDK targets as a first-class backend
(`os: zephyr`).  Pinned to v3.7.0 LTS per
[`docs/zephyr-version-policy.md`](zephyr-version-policy.md).
