# `metadata/e1m_modules/`

Per-SoM-family metadata for the E1M module catalogue -- both form
factors.  AEN (`E1M-AEN301..801`) and imx93 (`E1M-NX9101` [^imx93-tbd])
are **E1M** (35×35, `default_board: E1M-EVK` in each SKU's preset);
v2n and v2n-m1 are **E1M-X** (45×65, `default_board: E1M-X-EVK`).

Each family directory carries the pin-to-function mapping for the
silicon under the module's lid plus the per-revision SDK-version
compatibility table.  Each `E1M-<SKU>.yaml file` directory carries the
per-SKU manifest (silicon ref, populated on-module chips, I2C
device addresses, memory specs).

The E1M **standard** each family maps against — the fixed pad geometry
and default function per pad — is vendored as a verbatim snapshot under
[`metadata/e1m/`](../e1m/) (`pinout-v1.json` for E1M, `pinout-x-v1.json`
for E1M-X).  `scripts/check_e1m_pinout.py` cross-checks every non-`TBD`
`e1m_pad`/`e1m_function` in these families against it. See
[`docs/e1m-pinout.md`](../../docs/e1m-pinout.md).

## Families

| Family | SKUs                          | Silicon                           |
|--------|-------------------------------|-----------------------------------|
| AEN    | `E1M-AEN301..801`             | Alif Ensemble E3..E8              |
| imx93  | `E1M-NX9101` [^imx93-tbd]     | NXP i.MX 93 (i.MX 9352 variant)   |
| v2n    | `E1M-V2N101`, `E1M-V2N102`    | Renesas RZ/V2N                    |
| v2n-m1 | `E1M-V2M101`, `E1M-V2M102`    | Renesas RZ/V2N + DEEPX DX-M1      |

[^imx93-tbd]: `E1M-NX9101` is a **placeholder MPN** — the production SKU is
TBD pending the hand-written HW config (see the header of
`E1M-NX9101.yaml`).  The `som.sku` regex accepts `E1M-NX9xxx` for any
4-digit tail, so the real SKU drops in as a sibling preset; do not treat
`E1M-NX9101` as the canonical, released MPN and **never hardcode the
string `E1M-NX9101`** in tooling, docs, or examples as if it were a
shipping part.  The machine-visible marker is the preset's
`status.preliminary: true` (paired with `status.partial_hw_config:
true`) — tools that filter for released SoMs must key off that flag,
not off the SKU string.  When the real SKU lands, its preset flips
`preliminary` to `false` and this placeholder is deleted (no
legacy-compat alias).

## Schema + validation

Every `E1M-<SKU>.yaml` preset validates against
`metadata/schemas/som-preset-v1.schema.json`.  Since the 2026-07
tightening the schema sets `additionalProperties: false` and pins
**one canonical shape** per fact family — `memory:` (module DRAM /
flash capacities), `on_module:` (incl. `pmic_main` and the
`i2c_devices` address map), and `inference:` (`preferred_backend`
always; `ethos_u_variant` where applicable — the deprecated
`npu_population` list is silicon-derived, not authored) — so
a preset can no longer carry a misspelled or family-idiosyncratic
key silently.  Unknown hardware facts stay explicit `TBD`s (values
are never invented); `tan new-som` scaffolds a schema-valid preset
with exactly this shape.

Per-family pinmux capability tables live beside the presets at
`metadata/pinmux/<family>.yaml` (`aen.yaml`, `v2n.yaml`), generated
by `scripts/gen_pinmux_capability.py` and drift-gated by
`pr-generated-files.yml`.  `scripts/validate_metadata.py` sweeps the
SoM presets and `metadata/boards/` in one gate (CI:
`pr-metadata-validate.yml`).

### Per-SKU capability restriction (`silicon_capabilities:`)

Capability flags default to the **silicon's** set: the SoC JSON's
`capabilities:` block is the base layer and the preset's `capabilities:`
block only **adds** SoM-side features (on-module chips, bridge
accelerators).  When two SKUs of one family differ in silicon
*population* — an accelerator fused off / not bonded out on one order
code — the narrower SKU declares the delta as a **restriction**:

```yaml
# HYPOTHETICAL example only — no released SKU restricts its silicon
# capability set today.  Do not copy population facts from here.
silicon_capabilities:
  unpopulated: [gpu2d, dave2d]     # silicon offers these; this SKU does not populate them
```

Rules (enforced by `scripts/validate_metadata.py`):

* every listed name must resolve to a **truthy** entry in the referenced
  SoC JSON's `capabilities:` block — a SKU can only remove what its
  silicon offers, never add;
* a name must not also appear in the preset's additive `capabilities:`
  block (a capability is either SoM-added or silicon-unpopulated, never
  both);
* omitting the field means the SKU inherits the full silicon capability
  set — the default for every current SKU.

Downstream, `resolve_capabilities()` (scripts/alp_project_loader.py --
re-exported for compat through `scripts/alp_project.py`) forces each
listed capability to `false`/`0` for that SKU, and
`scripts/gen_soc_caps.py` appends an `ALP_SOM_<SKU>`-gated override block
to `include/alp/soc_caps.h` so `ALP_HAS(...)` drops the matching
`ALP_CAP_*` flags; the build emitters pass `-DALP_SOM_<SKU>` only for
restricted SKUs.

## `helper_firmware` — three independent axes

A `helper_firmware[]` entry describes an on-module helper MCU along
three axes that do **not** imply one another:

| Key | Answers |
|---|---|
| `flash_method` / `flash_args` | how the image is **written locally** (the transport) |
| `update_channel` | how the device is updated **in the field** |
| `flash_policy` | **who** may invoke `flash_method`, and **when** |

`flash_policy` is **required on every `helper_firmware` entry** — there
is no absent-means-`customer` default.  It is `customer` (a plain flash
target), `factory` (Alp Lab programs it in production; never a customer
flash target), or `recovery_only` (Alp Lab programs it in production,
and the customer may flash it *only* to recover a bricked device, with
Alp Lab-supplied binaries).  Requiring it unconditionally, rather than
only when an entry declares both a `flash_method` and an
`update_channel`, closes the gap where a helper with only a
`flash_method` silently became a customer flash target by omission.

### AEN / `cc3501e_otp`

All six AEN presets (`E1M-AEN301..801`) carry a `cc3501e_otp` helper
entry with `update_channel: alp_ota_spi_otp` and `flash_policy:
recovery_only`, and no `flash_method`.  The CC3501E (TI Wi-Fi 6 + BLE
5.4 coprocessor) is Alp-released firmware applied over the bridge SPI
link, programming the chip's own OTP — Alp Lab programs it in
production, and a customer flash is permitted only to recover a
bricked device, with Alp Lab-supplied binaries.
The six SKUs must stay in lockstep (same `update_channel`, same
`flash_policy`, same `firmware_path` provenance).

### V2N / V2M / `gd32_bridge`

All four E1M-X presets (`E1M-V2N101`, `E1M-V2N102`, `E1M-V2M101`,
`E1M-V2M102` — one PCB, variant-populated) carry a byte-identical
`gd32_bridge` entry declaring **two** axes:
`flash_policy: recovery_only` + `update_channel: alp_ota_spi_bridge`.
Field updates stream over the bridge link into the slot-A/B application
bootloader (protocol v0.6 Path A).

There is **no `flash_method`**, and no `flash_args`.  GD32 programming
is separated out of tan's scope entirely (#1439, tan-cli#732) -- tan
does not flash this part, and no preset names a local flash path for
it.  Alp Lab flashes the part in production over SWD with its own
tooling.  The on-SoM host-driven bit-bang route
(`chips/gd32_swd/`, Path B, `metadata/chips/gd32_swd.yaml`) is a
separate mechanism and is unaffected.

`flash_policy` stays because it is required on every helper entry with
or without a `flash_method` — it answers who may reach a local flash
path if one is ever added.  The six AEN `cc3501e_otp` entries are the
same shape (`recovery_only`, no `flash_method`).

The GD32's SW-DP ID remains unsettled: `metadata/chips/gd32_swd.yaml`
expects `0x6BA02477`, which `CHANGELOG.md` records as a measurement of
the **V2N CM33 DAP**, while `0x0BE12477` appears elsewhere with
conflicting provenance.  Neither is a confirmed GD32 reading — see
#1440.

See `metadata/schemas/som-preset-v1.schema.json`
`$defs/helper_firmware_entry` for the full contract.

## Consumed by

* `scripts/alp_project.py` -- reads `<SKU>.yaml` and
  `<family>/hw-revisions.yaml` to emit per-backend config from
  the customer's `board.yaml`.
* `alp-studio`'s pin allocator (same files).
* Documentation generators that translate the per-SKU SoM preset into
  per-SKU reference sheets.
