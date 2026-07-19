# Glossary

Terms a firmware engineer will encounter in the Alp SDK and
in E1M-X module documentation.

## A-D

**ADC** -- Analog-to-Digital Converter.  The SDK's `<alp/adc.h>`
abstracts the SoC's ADC peripheral.

**AEN** -- Alp Lab module family based on **Alif Ensemble**
silicon (E3..E8).  See [`docs/soms/aen.md`](soms/aen.md).

**alp-studio** -- An optional consumer that sits on top of alp-sdk:
a GUI / codegen tool that emits `<alp/...>` calls from block
manifests + a pin allocator.  Reads SoM metadata
(including the `pad_routes:` block) from this repo's
`metadata/e1m_modules/<SKU>.yaml`; alp-sdk's metadata is
alp-studio's input, not its output.  alp-sdk is fully usable
without it.  See
[`github.com/alplabai/alp-studio`](https://github.com/alplabai/alp-studio).

**Block** -- An alp-studio concept: a reusable feature unit
(button-LED, OLED display, IMU read) that alp-studio's pin
allocator places against the active SoM by consuming the SoM
preset's `pad_routes:` from alp-sdk.

**board.yaml** -- The single declarative file at the root of every
application.  Top-level fields: board identity (`name` /
`description` / `hw_rev`, or `preset:` referencing a shared
definition under `metadata/boards/<preset>.yaml`), `som.sku`,
the per-core `cores.<id>` block (`os`, `app`, `peripherals`,
`libraries`, `iot`, `inference`, `memory`, `power`), the
board-side `populated:` chip list + `e1m_routes:` pad-to-macro
routing (sections gpio / buses / pwm / adc / dac / i2s / can /
qenc), used-pad subset `pins:`, project-level `chips:`,
cross-core `ipc:`, `boot:` (MCUboot), `ota:` (Mender), `storage:`,
`security.psa:`, and `diagnostics:`.  Validated by
`scripts/validate_board_yaml.py` against
`metadata/schemas/board.schema.json`.

**BRD_I2C** -- Board-management I²C bus on V2N + V2N-M1.  Hosts
the PMICs, RTC, OPTIGA, supervisor MCU slave interface.

**Bridge (GD32)** -- The V2N module's on-module supervisor MCU
(GD32G553) reachable over a hybrid SPI + I2C transport.  See
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md).

**Board** -- A board that an E1M SoM plugs into.  The SDK ships
presets for the E1M-EVK + E1M-X-EVK reference boards.

**Capability layer** -- The generated `<alp/cap.h>` /
`<alp/cap_instance.h>` surface (`alp_has()` / `ALP_HAS()` +
per-instance queries) that lets portable code gate on what the
active silicon offers (`ALP_CAP_ID_HW_CAN`, `HW_I2S`, …) instead of
`#if`-ing on board or SoM names.  Generated from the SoC/SoM
metadata; the "gate on capabilities, not board names" pattern is
documented in [`docs/portability.md`](portability.md) §5.2.

**Carve-out** -- A physical memory region reserved for cross-core
IPC, declared in `board.yaml ipc[]` and resolved against the
auto-derived region table (from `metadata/socs/.../<part>.json
variants[].sram_banks_kb` + `mram_mb`, computed by
`_resolve_memory_map()`); SoM presets may override this with an
explicit `memory_map:` block for non-stock partitioning.  The
orchestrator emits matching reservations into both kernels' device
trees so neither side maps the region as ordinary memory.

**Chip driver** -- A non-OS-specific C module under `chips/<part>/`
that wraps a single silicon part's I²C / SPI / GPIO surface.
Symbols use the chip's natural name (e.g. `lsm6dso_init`); the
`alp_` prefix is reserved for SDK-level abstractions.

**CMI** -- Code Matrix Index, Qorvo's term for the ACT88760 PMIC's
configuration profile.  V2N populates the **CMI 120.E1** variant.

**Conformance suite** -- The data-driven ztest suite at
`tests/zephyr/conformance/` (13 peripheral classes × 8 contract
cases) that every backend must pass; runs on `native_sim` as the
`alp_sdk.conformance.portable_api` Twister scenario and is the
proof gate for a new SoM port's backends (see
[`docs/porting-new-som.md`](porting-new-som.md) "Conformance
gate").

**Core id** -- A normalized identifier (e.g. `a55_cluster`,
`m33_sm`, `m55_hp`) assigned to each on-die programmable core in
`metadata/socs/.../*.json cores[]`.  Used by `board.yaml cores:`
blocks and SoM preset `topology:` blocks to address cores by name
rather than by `cores[]` array index.

**DEEPX DX-M1** -- An on-module AI accelerator populated on V2N-M1
SKUs only.  See [`docs/soms/v2n-m1.md`](soms/v2n-m1.md).

**DRP-AI3** -- Renesas's on-die NPU on the RZ/V2N silicon, driven
from the A55/Linux side only (the MERA / DRP-AI TVM runtime) and
exposed through the SDK's `<alp/inference.h>` dispatcher when the
Yocto build enables `ALP_SDK_USE_DRPAI_V2N`.  The V2N M33 (Zephyr)
core cannot reach the engine and runs TFLM instead.

**DSP chain** -- A composable pipeline of filter / window / FFT
stages under `<alp/dsp.h>` (standalone) and
`<alp/adc.h>`'s `alp_adc_filter_t` / `alp_adc_spectrum_t`
(ADC-pipeline-integrated).  See
[ADR 0007](adr/0007-wave2-dsp-pipeline-design.md).

## E-K

**E1M** -- 35 × 35 mm SoM form factor (312 pads).  AEN + i.MX 93
families ship in this size.

**E1M-X** -- 45 × 65 mm SoM form factor (496 pads).  V2N family.

**EVK** -- Evaluation Kit.  The reference board Alp Lab ships for
bring-up.  Two flavours: E1M-EVK (35 × 35) and E1M-X-EVK (45 × 65).

**Ethos-U** -- Arm's micro-NPU IP.  AEN modules carry Ethos-U55;
N93 modules carry Ethos-U65.

**GPU2D** -- 2D compositing accelerator (alpha blending, rotation,
scaling) for OLED/TFT pipelines.  AEN's "GPU2D" is the TES **D/AVE
2D** engine (`dave2d: true` in the AEN SoC JSON); other E1M families
have no peer hardware and run a portable software fallback.  Exposed
via `<alp/gpu2d.h>` for portability.  See
[ADR 0008](adr/0008-gpu2d-portable-shim.md).

**`<family>/hw-revisions.yaml`** -- Per-rev SDK-version compatibility
table.  Customer's `som.hw_rev` is validated against this list at
build time.

**Hand-written firmware** -- Application code that calls
`<alp/...>` directly without alp-studio's codegen.  First-class
consumer path -- not a fallback.

**Helper MCU** -- An on-module microcontroller other than the host
SoC's on-die cores (e.g. GD32G553 supervisor on V2N, CC3501E Wi-Fi
coprocessor on AEN).  Its firmware builds via dedicated pipelines
and registers into the system manifest; it is not a
heterogeneous-compute peer.

**IDCODE** -- The 32-bit Arm Coresight SW-DP identification value
returned by the target on the first SWD read after a line reset.
Documented as `0x6BA02477` for the GD32G553 (Cortex-M33 r0p1
SW-DPv2); used by `gd32_swd_connect` to confirm the link reaches
the right silicon.

**HiL** -- Hardware-in-the-Loop testing.  See
the HiL rig plan in the internal `alp-sdk-internal` repo.

**hw_rev** -- Hardware revision label (e.g. `r1`, `r2`, ...).
Distinguishes board respins of the same SKU.

**hw_info** -- Runtime structure populated from the on-module
EEPROM manifest + BOARD_ID ADC.  See
[`<alp/hw_info.h>`](../include/alp/hw_info.h).

**Inline AES** -- On-the-fly encryption of external flash traffic by
an inline-AES-capable controller (Alif SecAES on OSPI / HexSPI;
`inline_aes: true` in the AEN SoC JSONs).  Configured through
`alp_storage_configure_inline_aes()` in
[`<alp/storage.h>`](../include/alp/storage.h); backends without an
inline-AES path return `ALP_ERR_NOSUPPORT`.  Key material travels the
OPTIGA path only -- the SDK never sees the AES key in clear (see
[`docs/threat-model.md`](threat-model.md)).

**ISP** -- Image Signal Processor.  On AEN silicon this is the
VeriSilicon **ISP Pico** (`vsi,isp-pico`), populated on the E4 / E6 /
E8 variants (never E7).  Coarse controls ride the portable
`alp_camera_configure_isp()`; the finer ISP-Pico-only knobs live in
the vendor-ext `<alp/ext/alif/camera.h>`.  See
[`docs/aen-accelerator-backends-design.md`](aen-accelerator-backends-design.md).

**Kconfig** -- Linux kernel's configuration language.  Zephyr +
the SDK use it for per-feature opt-in.

## L-P

**LCS** -- Lifecycle State of the Alif Secure Enclave: `0x0` CM (chip
manufacturer), `0x1` **DM** (device manufacturer -- debug open, fully
re-provisionable; the state Alp ships modules in), onward to secure /
RMA states.  Read via `se_service_system_get_device_data()`; see
[`docs/aen-se-services.md`](aen-se-services.md) +
[`docs/aen-provisioning.md`](aen-provisioning.md).  The OPTIGA Trust M
secure element has its own, separate lifecycle bits surfaced by
[`<alp/chips/optiga_trust_m.h>`](../include/alp/chips/optiga_trust_m.h).

**Library manifest** -- A `metadata/libraries/<name>.yaml` file: the
single source of truth for one curated third-party library (ADR 0018).
Declares its per-OS `integration:` wiring (Zephyr Kconfig / Yocto
`IMAGE_INSTALL` / baremetal CMake), `requires:` compatibility
constraints, curation `tier:`, pinned `version:`, and SPDX `license:`.
Selected project-wide via the top-level `libraries: [<name>, ...]` key
in `board.yaml`; the orchestrator emits the wiring and rejects an
incompatible selection at emit time.  See
[`metadata/libraries/README.md`](../metadata/libraries/).

**Loader** -- `scripts/alp_project.py` -- reads `board.yaml`,
resolves SoM SKU preset + board preset, emits the per-backend
config (Zephyr `alp.conf` / CMake `-D` flags / Yocto `local.conf`).

**MCUboot** -- The bootloader used on AEN-Zephyr for secure-
boot image verification + A/B slot swap-using-scratch.  Config
lives at `zephyr/sysbuild/aen/sysbuild.conf`.

**MDIO** -- Management Data Input/Output.  The clause-22 bus used
to configure Ethernet PHYs.

**Mender** -- The OTA update system used on Yocto-side E1M
modules (V2N, V2N-M1, i.MX 93).  Zephyr-side equivalent
deferred to v1.1 per [ADR 0009](adr/0009-mender-zephyr-client-deferred.md).

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

**`pad_routes:`** -- A block inside each SoM preset
(`metadata/e1m_modules/<SKU>.yaml`) that lists, per E1M pad, the
SoC pin it lands on and which peripheral function it carries
(`pad: AF2`, `soc_pin: P3_4`, `as: I2C_SCL`).  This is the SoM-
side dispatch routing — the layer that turns an E1M pad name into
an actual silicon pin for the active SKU.  alp-sdk holds these
routes; alp-studio's pin allocator consumes them at codegen time.
Earlier designs placed the routes inside `alp-studio/library/_soms/`;
that direction was reversed on 2026-05-18 so all generator inputs
live in one repo.

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

**SE-CryptoCell** -- The hardware-crypto backend for
[`<alp/security.h>`](../include/alp/security.h) on the Alif Ensemble
E8: hash / AEAD / random compute is pushed into the Secure Enclave's
CryptoCell over the RTSS-HE ↔ SE MHUv2 mailbox
(`src/backends/security/se_cryptocell.c`), registered one priority
step ahead of the portable MbedTLS-PSA backend -- so E8 apps get SE
key-isolated crypto by default (v0.8.0), and algorithms the SE
declines fall through to PSA on the M55.  Nothing in
`<alp/security.h>` names the SE.

**sysbuild** -- Zephyr's umbrella build system for multi-image
projects (application + MCUboot + ...).  AEN's secure-boot
profile lives at `zephyr/sysbuild/aen/sysbuild.conf`.

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

**`silicon_variant:`** -- Forward MPN-reference field on each SoM
preset that names the exact vendor order code the module is built
around (`AE302F80F55D5LE` for `E1M-AEN301`, `R9A09G056N44GBG` for
`E1M-V2N101`, …).  The loader uses it to forward-resolve the
active SoC variant in `metadata/socs/<vendor>/<family>/<part>.json`
without scanning the reverse `alp_module_skus[]` arrays.  Alp-set
on released presets; customers don't write it.  `TBD` is honoured
per the no-inventing-values rule (e.g. the current `E1M-NX9101`
preset).

**SKU** -- Stock-Keeping Unit.  In Alp terminology: an MPN that
identifies a specific SoM configuration (e.g. `E1M-V2N101`).

**Slice** -- One per-core build invocation produced by the
orchestrator (e.g. `a55_cluster-yocto`, `m33_sm-zephyr`).  Each
slice runs its native build system (bitbake, west) in its own
scoped subprocess.

**SoC** -- System-on-Chip.  The main silicon under a SoM's lid.

**SoM** -- System-on-Module.  Alp's per-SoC PCB module that plugs
into a board (e.g. E1M-V2N101, E1M-AEN801).

**SoM preset** (`E1M-<MPN>.yaml`) -- Per-SKU manifest declaring
silicon, populated chips, I²C device addresses, memory specs,
default board.  Lives at `metadata/e1m_modules/<SKU>.yaml`
(e.g. `E1M-AEN801.yaml`, `E1M-V2N101.yaml`).  Earlier docs
called this `som.yaml`; the file name carries the SKU now so
each preset is distinguishable in a directory listing.

**Supervisor MCU** -- The GD32G553 companion microcontroller on
V2N modules.  Owns peripherals that don't fit on the main SoC's
pinmux.  See [`docs/gd32-bridge.md`](gd32-bridge.md).

**System manifest** -- `build/system-manifest.yaml`, the generated
artefact produced by `tan build` (seeded by the SDK's `alp_orchestrate
--emit system-manifest`) that captures every slice's output binary,
every IPC carve-out's resolved address, the boot order, and pointers
to helper-MCU firmware.  The single source of truth consumed by `tan
image`, `tan flash`, the OTA bundler, and (eventually) alp-studio.

**Target mode** -- Operating an I²C or SPI controller as the bus
*target* (slave): an external controller owns the clock and our
firmware answers.  Portable surface `alp_i2c_target_*` (byte-granular
ISR callbacks) / `alp_spi_target_*` (transfer-based, preloaded TX)
in `<alp/peripheral.h>` (v0.9, `[ABI-EXPERIMENTAL]`); drivers
without target support degrade with `ALP_ERR_NOSUPPORT`.  Reference
examples: `examples/peripheral-io/i2c-slave` + `spi-slave`.

**TBD** -- "To be determined".  Used in metadata where the
authoritative value is pending (e.g. a board-rev divider voltage).

**Tier A / Tier B (libraries)** -- The two curation tiers for
curated third-party libraries (ADR 0018), recorded in each
library manifest `tier:` field.  **Tier A (curated):**
version-pinned, built in alp-sdk CI for at least one board per
supported family, ships a teaching example -- breakage blocks
release.  **Tier B (recipe-only):** wiring + compatibility metadata
are maintained and emitted, but the library is not built in alp-sdk
CI; `alp doctor` labels it.  Promotion B → A requires a dedicated
owner and a CI build lane.  (Distinct from the driver/library
integration ladder in
[ADR 0017](adr/0017-alp-sdk-over-the-vendor-sdk.md).)

**Topology block** -- The `topology:` block in
`metadata/e1m_modules/<SKU>.yaml` that declares the default OS +
app for each on-die core.  Customers inherit + override these
defaults in their project's `board.yaml cores:` block.

**V2N** -- Alp module family based on Renesas RZ/V2N silicon.
E1M-X form factor.  See [`docs/soms/v2n.md`](soms/v2n.md).

**V2N-M1** -- V2N variant with the DEEPX DX-M1 NPU on-module.
SKUs `E1M-V2M101` / `E1M-V2M102`.  See
[`docs/soms/v2n-m1.md`](soms/v2n-m1.md).

**west** -- Zephyr's meta-tool for workspace management +
sub-commands.  alp-sdk is plans-only (ADR
[0020](adr/0020-sdk-owns-build-execution.md)) and no longer ships a
build-executing west extension; building goes through the standalone
`tan` CLI (`tan build`), which pre-flights `board.yaml` validation
before delegating to `west build`.  The SDK still ships the
non-build west extension commands `west alp-migrate` (board.yaml
schema migration), `west alp-lock` (dependency lockfile), `west
alp-quality` (quality-task registry), and `west alp-emit` (artefact
inspector).

**Wi-Fi 6** -- 802.11ax.  AEN's CC3501E + V2N's Murata module
both support it.

**Yocto** -- Embedded Linux build system.  Selected via
`cores.<id>.os: yocto` in `board.yaml` (typically the A-cluster
core); on supported SoMs the SoM topology default already supplies
`yocto` so customers usually omit the field.  v0.4 deliverable per
the test plan.

**Zephyr** -- The RTOS the SDK targets as a first-class backend
(`cores.<id>.os: zephyr`, typically on M-class cores; the SoM
topology supplies `zephyr` by default so customers usually omit
the field).  Pinned to v4.4.0 per
[`docs/zephyr-version-policy.md`](zephyr-version-policy.md).
