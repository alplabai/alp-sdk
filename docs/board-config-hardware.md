# `board.yaml` hardware-revision tracking

How the SDK detects "wrong firmware for this hardware": the
build-time `hw_revisions:` / SDK-version window check, the runtime
BOARD_ID ADC + EEPROM manifest check, and the per-chip `assembled:`
flag that lets one SoM SKU cover multiple BOM populations.

See [`docs/board-config.md`](board-config.md) for the landing page.

## Hardware revision tracking

Every released SoM family and every released board carries a
`hw_revisions:` table.  The SDK uses it to detect "wrong firmware
for this hardware" two ways:

- **Build-time** -- the loader + validator read
  [`metadata/sdk_version.yaml`](../metadata/sdk_version.yaml) and
  fail-fast if the chosen `hw_rev`'s
  `[min_sdk_version, max_sdk_version]` window doesn't cover the
  current SDK version.  Validator exit code `3`; loader aborts the
  CMake configure with a clear error.
- **Runtime** -- the SDK boots into a board-ID check that uses
  a single ADC pin per board (SoM-side and board-side) fed by a
  resistor divider from a 1.8 V rail.  Each `hw_rev` entry's
  `board_id:` sub-block fixes the divider resistor values and the
  nominal mV reading the SDK looks for.  A second, finer-grained
  check reads the SoM's on-module 24C128 EEPROM (the AEN family
  populates one by default) for an authoritative MPN string +
  serial + mfg date -- the production-test flow writes the
  manifest, the SDK matches it against `board.yaml`'s
  `som.sku`.  Mismatch on either tier halts boot.

### Why one ADC pin (instead of GPIO straps)

The E1M form factor has no spare GPIO pads for board-ID resistor
straps -- every pad is allocated by the spec.  A single ADC
channel with a resistor divider distinguishes up to ~8 revisions
at +/-100 mV bin radius (with 1 % resistors on a 1.8 V rail);
that is enough for many family respins and leaves the rest of the
GPIOs free for the application.  Per-rev resistor + voltage
choices are documented in each family file's `board_id:` block;
the canonical math lives at
[`metadata/e1m_modules/aen/hw-revisions.yaml`](../metadata/e1m_modules/aen/hw-revisions.yaml).

### How the data is laid out

```
metadata/
├── sdk_version.yaml                            # SDK release version (the single source of truth)
├── e1m_modules/
│   ├── aen/hw-revisions.yaml                   # family-level revs (AEN family
│   │                                            #  shares one PCB; SKUs differ
│   │                                            #  by silicon only).
│   ├── v2n/hw-revisions.yaml                   # V2N family revs (board_id.adc_channel TBD)
│   ├── v2n-m1/hw-revisions.yaml                # V2N-M1 family revs (mirrors V2N + DEEPX)
│   ├── imx93/hw-revisions.yaml                 # i.MX 93 family revs (adc_channel TBD)
│   └── E1M-AEN801.yaml                     # MPN preset; `default_hw_rev: r2`
│                                                #  points into the family table.
└── boards/
    ├── e1m-evk.yaml                            # board preset; carries its own
    │                                            #  hw_revisions + default_hw_rev.
    ├── e1m-x-evk.yaml                          # V2N / V2N-M1 board
    │                                            #  (board_id.adc_channel TBD).
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

