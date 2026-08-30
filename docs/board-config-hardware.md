# `board.yaml` hardware-revision tracking

How the SDK detects "wrong firmware for this hardware": the
build-time `hw_revisions:` / SDK-version window check, the runtime
on-module EEPROM manifest check, and the per-chip `assembled:`
flag that lets one SoM SKU cover multiple BOM populations.

See [`docs/board-config.md`](board-config.md) for the landing page.

## Hardware revision tracking

Every released SoM family and every released board carries a
`hw_revisions:` table.  The SDK uses it to detect "wrong firmware
for this hardware" two ways:

- **Build-time** -- each `hw_revisions:` entry carries a
  `[min_sdk_version, max_sdk_version]` window; the loader compares it
  against [`metadata/sdk_version.yaml`](../metadata/sdk_version.yaml)
  and refuses (`SdkRevisionUnsupported`, exit code 3 from
  `scripts/validate_board_yaml.py`) when the running SDK falls
  outside it.  Separately, an `hw_rev` that isn't a key in the
  resolved table at all -- a typo, or a revision newer than the
  installed SDK -- refuses too (`SdkRevisionUnknown`, exit code 4),
  rather than silently falling back to base-revision overrides.  A
  revision that EXISTS but is declared `status: reserved`,
  `status: tbd`, or carries no `status` key at all also refuses
  (`SdkRevisionNotBuildable`, exit code 5) --
  [#1025](https://github.com/alplabai/alp-sdk/issues/1025).  Every
  other declared status (`production`, `preview`, `preliminary`,
  `deprecated`) resolves and builds normally.
- **Runtime** -- the on-module EEPROM manifest is the SDK's one
  authoritative runtime identity check: `alp_hw_info_read()` reads
  the SoM's on-module 24C128 EEPROM (the AEN family populates one by
  default) for an authoritative MPN string + `hw_rev` + serial + mfg
  date -- the production-test flow writes the manifest; the SDK
  itself only reads and integrity-checks it (magic + schema_version +
  CRC32).  Matching it against the firmware build (`board.yaml`'s
  `som.sku` / `hw_rev`, via the generated `ALP_HW_BUILD_SOM_SKU` /
  `ALP_HW_BUILD_SOM_HW_REV`) with SKU precision, or as a hard
  boot-refusing failure, is still an explicit call the application
  makes (`alp_hw_info_assert_matches_build()` -- typically halt) --
  see below.  Separately (issue #1853), the SDK's own boot banner
  (`CONFIG_ALP_SDK_BANNER`, on by default) now does a narrower version
  of this automatically: it compares the live manifest's `hw_rev`
  against `CONFIG_ALP_SDK_SOM_HW_REV` (the hw_rev this firmware build
  resolved -- nothing in the compiled firmware derives a pad-routing
  table from it; some E1M pads physically route to a different chip
  depending on `hw_rev`, e.g. the AEN family's IO8/IO10/IO21, and
  application code that hardcodes a pin-to-chip map is what can
  actually mis-target one -- see
  [#1859](https://github.com/alplabai/alp-sdk/issues/1859)) and
  prints a loud warning on a disagreement, without refusing to boot; a
  factory-fresh module's NOT_PROVISIONED read never reaches this check.
  A production build that would rather halt than risk driving a pad on
  the wrong chip opts in via `CONFIG_ALP_SDK_HW_REV_MISMATCH_FATAL`.
  There is no SoM-side ADC cross-check (`<alp/hw_info.h>`).  A
  carrier/EVK board may
  separately encode its own revision on a board-side BOARD_ID
  resistor divider read over one ADC pin -- the `board_id:` sub-block
  lives under the *board* preset's `hw_revisions:` entry
  (`metadata/boards/<preset>.yaml`, not the SoM's
  `metadata/e1m_modules/` family file).  That carrier path is
  separate, optional, and independent of the SoM's identity; its
  runtime ADC decode is not implemented yet (`board_hw_rev` /
  `board_id_mv` in `alp_hw_info_t` are reserved for it -- see
  `include/alp/hw_info.h`).  Mismatch on the EEPROM check is the
  application's to act on (`alp_hw_info_assert_matches_build()` --
  typically halt); see [`docs/board-id.md`](board-id.md) for the
  manifest layout and read flow.

### Why one ADC pin on the carrier (instead of GPIO straps)

The E1M form factor has no spare GPIO pads for a carrier-side
board-ID resistor strap -- every pad is allocated by the spec.  A
single ADC channel with a resistor divider distinguishes up to ~8
carrier-board revisions at +/-100 mV bin radius (with 1 % resistors
on a 1.8 V rail); that is enough for many carrier respins and leaves
the rest of the GPIOs free for the application.  This is a
carrier/EVK-board mechanism only -- it says nothing about which SoM
is populated; the SoM's own identity is the EEPROM manifest above.
Per-rev resistor + voltage choices are documented in each *board*
preset's `board_id:` block; see
[`metadata/boards/e1m-evk.yaml`](../metadata/boards/e1m-evk.yaml) for
the E1M EVK's (channel still `TBD` pending the schematic's BOARD_ID
net assignment) and
[`metadata/boards/e1m-x-evk.yaml`](../metadata/boards/e1m-x-evk.yaml)
for the E1M-X EVK's (`adc_channel: E1M_X_ADC7`; divider resistor
values held in the private `alp-sdk-internal` repo per the
public/private split policy -- determined, not TBD).

### How the data is laid out

```
metadata/
├── sdk_version.yaml                            # SDK release version (the single source of truth)
├── e1m_modules/
│   ├── aen/hw-revisions.yaml                   # family-level revs (AEN family
│   │                                            #  shares one PCB; SKUs differ
│   │                                            #  by silicon only).  SoM
│   │                                            #  identity is EEPROM-only --
│   │                                            #  no `board_id:` block here.
│   ├── v2n/hw-revisions.yaml                   # V2N family revs (same: no board_id)
│   ├── v2n-m1/hw-revisions.yaml                # V2N-M1 family revs (mirrors V2N + DEEPX)
│   ├── imx93/hw-revisions.yaml                 # i.MX 93 family revs (same: no board_id)
│   └── E1M-AEN801.yaml                     # MPN preset; `default_hw_rev: r2`
│                                                #  points into the family table.
└── boards/
    ├── e1m-evk.yaml                            # board preset; carries the
    │                                            #  carrier's own hw_revisions +
    │                                            #  default_hw_rev + `board_id:`
    │                                            #  (adc_channel TBD).
    ├── e1m-x-evk.yaml                          # V2N / V2N-M1 carrier board
    │                                            #  (`board_id.adc_channel:
    │                                            #  E1M_X_ADC7`; divider values
    │                                            #  TBD in alp-sdk-internal).
    └── custom-example.yaml                     # copy-friendly template
```

`board.yaml` overrides go in the `som.hw_rev` field (described in
the [`som` block](board-config-schema.md#som-block)) and the
`board.hw_rev` field (described under
[Board declaration -- Inline mode](board-config-schema.md#inline-mode-the-customer-path))
of the schema reference.  Omit them on stock builds -- the preset's
`default_hw_rev` is picked up automatically.

## Modular SoM: optional chip populations

The SoM YAML carries a per-chip **`assembled:`** flag for every entry
in its `i2c_devices:` topology so the SDK can express SoMs that ship
in multiple BOM variants (same SKU, different chip populations).
Three states:

| `assembled:`  | Meaning                                                                |
|---------------|------------------------------------------------------------------------|
| `true` *(default)* | Chip is always populated on every BOM variant of this SKU.       |
| `false`            | DNI (Do Not Install) -- the chip footprint exists but is empty.  |
| `"optional"`       | Per-BOM-variant -- some units have it, some don't.               |

Example (extract from `metadata/e1m_modules/E1M-V2N101.yaml`):

```yaml
i2c_devices:
  brd_i2c:
    devices:
      - { chip: rv3028c7,  role: rtc,           address_7bit: "0x52" }
      - { chip: act8760,   role: pmic_main_p0,  address_7bit: "0x25" }
      - { chip: tps628640, role: lpddr4x_0v6,   address_7bit: "0x4D",
          assembled: optional }      # only some BOM variants
```

The lint at `scripts/check_example_portability.py` reads this flag
and prints `NOTE` lines whenever an example's `chips:` list reaches
for a `assembled: optional` chip on its target SKU.  Customer code
that uses an optional chip MUST handle `alp_*_init` returning
`ALP_ERR_NOT_READY` gracefully (skip the demo, log a clear message,
fall back to a different code path) instead of crashing.

### Runtime: detecting which chips are populated

Two complementary mechanisms:

1. **`<alp/hw_info.h>`**.  `alp_hw_info_read()` reads the SoM
   manifest from the on-module 24C128 EEPROM (`metadata/templates/
   eeprom_manifest.yaml` for the layout).  Production-test wrote
   the manifest; firmware checks `hw_info.som_hw_rev` +
   capability flags to know which chip set this unit shipped with.
2. **Probe-and-fall-back**.  Every chip driver's `_init()`
   ACK-probes the I2C bus.  If the chip isn't populated the
   driver returns `ALP_ERR_NOT_READY`; firmware branches off
   that.  This is the right mechanism for runtime discovery on
   boards that don't carry the SoM manifest.

The two mechanisms cooperate: `<alp/hw_info.h>` answers "what was
this unit *intended* to carry?", and the per-chip `_init()` probe
answers "is the chip *actually* responding right now?".  When they
agree, the firmware proceeds; when they disagree, that's a
production-test follow-up signal (likely a mis-strap or assembly
defect).

### When you'd add a new optional flag

If your board strips a chip the upstream preset declares populated,
the right approach is **per-app override in `board.yaml`** rather
than editing the preset:

```yaml
# my-app/board.yaml
som:
  sku: E1M-V2N101
  overrides:
    on_module:
      i2c_devices:
        brd_i2c:
          devices:
            - { chip: optiga_trust_m, assembled: false }
            # other devices inherit from the preset
```

The loader merges your overrides onto the preset before generating
the build config.  No SDK fork needed.

