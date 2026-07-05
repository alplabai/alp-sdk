# Block physical data layer — carrier netlist + BOM enablement

**Status:** design approved 2026-07-05
**Issue:** alplabai/alp-studio#65 (sibling of #35)
**Repo:** alp-sdk (public); detail-rich mirror in alp-sdk-internal

## Context

alp-studio wants to emit a **carrier netlist + BOM** from a prompt. The
customer takes that artifact into their own EDA tool (Signex / KiCad) and does
the PCB layout themselves — **PCB automation is deliberately deferred**; the
netlist is the handoff. This is the SDK-side half: the "blocks" have no
physical data layer yet, so no netlist/BOM can be emitted.

We approach this **as a SoM manufacturer**. Our deliverables are: the **module**
(the E1M connector is its public external interface; the module's internal
schematic stays private), **reference carriers**, and **reference block
circuits**. The netlist we hand a customer describes *their carrier* wiring
blocks → E1M pads → the SoM connector. The SoM's internals never appear in it.

## Current state (verified, alp-sdk tree)

- A **block** is firmware only and **IC-agnostic by design** (`blocks/README.md`:
  "a *pattern* over peripherals … *not* a binding to a specific IC … any
  compliant peripheral plugs in"). `button_led` wraps `alp_gpio_*`; `pdm_mic`
  wraps `alp_i2s_*`. Blocks have **no** metadata manifest today.
- A **chip** (`metadata/chips/<part>.yaml`, 74 of them) already carries
  `mpn_population` (MPN), `vendor`, `signals[]` (pin name + type + bus scope),
  `bus`, i2c/spi addresses. It has **no** `package`/`footprint`/pin-number/
  `passives`/`refdes`, and **no JSON schema validates it**.
- The **carrier** (`metadata/boards/e1m-evk.yaml`) lists soldered parts by
  chip-/block-slug under `populated:` (some with refdes in comments, e.g.
  `tas2563  # U27, U28`) and E1M-pad→macro bindings under `e1m_routes:`. It is
  SoM-agnostic by design.
- The **SoM pad map** (`pad_routes:` in `metadata/e1m_modules/<SKU>.yaml`) plus
  the studio allocator's `pins_allocated` already resolve each block's abstract
  interface signals onto specific SoM pads.

**Reframe that drives the design:** physical data (MPN/footprint/pins/passives)
cannot live on the abstract block without breaking swap-friendliness. It
attaches to the **chip** that *realizes* the block. A block → realization
binding chooses what physically realizes it: a chip (discrete) or a pre-routed
module (connector).

## Goal / non-goals

**Goal:** give the studio enough physical data to emit a carrier netlist + BOM
for the **AEN801 reference-carrier part set**, with a public/private
classification split from day one.

**Non-goals:** PCB layout / routing automation; an EDA engine; populating all 74
chips (only the AEN801 reference set this slice); the SoM's internal schematic
(stays private, out of the carrier netlist).

## Design

### §1 — Chip physical (extends `metadata/chips/<part>.yaml`)

New optional `physical:` block, and a **new** `metadata/schemas/chip-v1.schema.json`
(there is none today; it retro-validates all 74 manifests, `physical:`
optional so unpopulated chips still pass):

```yaml
physical:
  refdes_prefix:  U            # U / R / C / D / J …
  package:        DSBGA-8
  footprint:      ti_dsbga_8_0p5mm   # neutral footprint id (EDA-agnostic)
  pins:                        # pad number -> a name already in signals[]
    - { pad: A1, signal: SDA }
    - { pad: A2, signal: SCL }
    - { pad: B1, signal: VDD }
    - { pad: B2, signal: GND }
  passives:                    # carrier-side support parts this chip needs
    - { role: decouple, value: 100nF, net: VDD, refdes_prefix: C }
    - { role: pullup,   value: 4k7,   net: SDA, refdes_prefix: R }
    - { role: pullup,   value: 4k7,   net: SCL, refdes_prefix: R }
  visibility:     public       # public | internal (per-part; §3)
```

`pins[].signal` **must** reference a `name` already declared in the chip's
`signals[]` (or a power/ground net) — the schema and `validate_metadata.py`
enforce this so pinout and footprint can't drift.

### §2 — Block realization (`metadata/blocks/<name>.yaml`, new)

First-class block manifest, symmetric with `metadata/chips/`, plus a new
`metadata/schemas/block-v1.schema.json`:

```yaml
schema_version: 1
block_id:       button_led
display_name:   "Generic button + LED helper"
kconfig:        ALP_SDK_BLOCK_BUTTON_LED      # matches blocks/README convention
interface:                                    # the abstract need (mirrors studio block-schema)
  - { signal: BTN, dir: input,  pull: up }
  - { signal: LED, dir: output, rail: 3V3, max_ma: 10 }
realizations:
  - id:            evk_discrete
    physical_form: discrete
    parts:
      - { chip: omron_b3u,  maps: { SW: BTN } }     # button -> BTN
      - { chip: led_0603,   maps: { A: LED } }      # LED anode -> LED
    passives:
      - { role: series, value: 330R, net: LED, refdes_prefix: R }
    visibility:    public
```

Module form (e.g. cc3501e):

```yaml
realizations:
  - id:            m2_e_key
    physical_form: module
    connector:     { footprint: m2_e_key, pins: [ ... pin -> signal ... ] }
    # module internals are NOT emitted into the carrier netlist
    visibility:    public
```

### §3 — Public/private split (from day one, per-part `visibility:`)

Per `classifying-public-vs-internal` gate 3 (no SoM physical-design detail in
public) and the sanitised↔detail-rich duality:

- **Public** (`alp-sdk`): block realizations, catalog-IC footprints, and the
  **E1M connector pinout** (the module's public external interface — approved
  as public, like a datasheet pinout).
- **Internal** (`alp-sdk-internal`): any realization or footprint that leans on
  SoM-internal routing / schematic detail. A `visibility: internal` part's
  detail-rich body lives in the private mirror; the public tree carries only the
  sanitised stub. The emitter resolves an internal footprint from the private
  mirror at build time.
- The SoM's `on_module` parts are **module-internal** — they never enter the
  carrier netlist, so they are out of scope here regardless of visibility.

Default `visibility: internal` when unsure (public git history is forever);
promote per-part deliberately.

### §4 — Validation + derivation

- Extend `scripts/validate_metadata.py`: load `chip-v1` + `block-v1` schemas;
  assert every `pins[].signal` resolves; assert every `passives[].net` /
  `parts[].maps` target exists; assert `visibility` present on any populated
  `physical:` / `realizations:`.
- `check_pin_conflicts.py` unchanged (SoM-pad scope) but a new check asserts a
  footprint pad is referenced at most once per realization.
- **Netlist derivation (studio-side, mechanical):** block realization pins →
  E1M pads (allocator `pins_allocated`) → SoM connector = nets. **BOM** = the
  `mpn_population` + `passives` across the realization's parts. No EDA engine.
  The studio's interface-level connectivity-report spike can run **today**
  against existing `signals[]` + `pad_routes` + allocator, before §1/§2 land.

### §5 — AEN801 reference-carrier part set (this slice)

From `e1m-evk.yaml` `populated: true` with the AEN801 SoM:

| Part | Manifest today | Action |
|---|---|---|
| icm42670, tas2563, ina236 | chip ✅ | add `physical:` |
| pdm_mic, button_led | block (firmware) | add `metadata/blocks/*.yaml` |
| bmi323, bmp581, cam_mux_pi3wvr626, tcal9538 | slug mismatch ⚠️ | reconcile chip slug, then add `physical:` |

## Testing

- Schema round-trip: all 74 chip manifests validate against `chip-v1` (physical
  optional); the new block manifests validate against `block-v1`.
- Negative: a `pins[].signal` with no matching `signals[]` entry fails
  validation; a duplicated footprint pad fails the pad-uniqueness check.
- Golden fixture: a connectivity report for the AEN801 reference set asserts
  every block interface signal resolves to an E1M pad + a BOM line, proving the
  netlist is derivable end to end from public data.

## Open questions

None outstanding — model (chip-manifest physical, per-realization), E1M pinout
public, scope (AEN801 set), and classification (per-part `visibility`) are
resolved. Exact neutral footprint-id vocabulary is chosen during
implementation (start from KiCad standard footprint names, alias as needed).
