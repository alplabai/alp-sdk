# Board identification — SoM EEPROM manifest

The SoM hardware revision is identified by a single authoritative
surface: a **128-byte EEPROM manifest** programmed once into the
SoM's on-module 24C128. It carries family, SKU, hardware revision,
serial number, and manufacturing date. Implemented in
[`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c) as the
EEPROM-side reader behind `alp_hw_info_read()`.

The EEPROM travels with the SoM, so it *is* the module's identity.
There is no SoM-side ADC resistor-divider cross-check; the manifest's
own integrity protection (magic + `schema_version` + CRC32) is what
guards against an unprogrammed or corrupt module.

This guards against:

* **Wrong firmware build.** Application links against the wrong
  metadata-driven config. `alp_hw_info_assert_matches_build()` is the
  explicit check that catches a build pointed at the wrong SoM SKU /
  hw_rev.
* **Unprogrammed or corrupt module.** A blank EEPROM returns
  `ALP_ERR_NOT_PROVISIONED`; a present-but-corrupt manifest returns
  `ALP_ERR_IO`. Boot code can branch on which.

> **Carrier note.** A carrier/EVK may still encode its own revision on
> a board-side BOARD_ID resistor divider, surfaced as `board_hw_rev` /
> `board_id_mv`. That is a separate, board-side path independent of the
> SoM revision and is not covered here.

## EEPROM manifest layout

128 bytes, located by default at offset 0 of the on-module 24C128
EEPROM (`0x50` strap default; configurable via Kconfig).  The
struct definition lives in `<alp/hw_info.h>`:

```c
typedef struct {
    uint32_t magic;             /* "ALPH" -- 0x414C5048, little-endian */
    uint32_t schema_version;    /* currently 1 */
    char     family[16];        /* e.g. "v2n", "v2n-m1", "aen" */
    char     sku[24];           /* e.g. "E1M-V2N101"            */
    char     hw_rev[8];         /* e.g. "2626-r2" -- see below  */
    char     serial[24];        /* factory-assigned             */
    uint16_t mfg_year;
    uint8_t  mfg_month;
    uint8_t  mfg_day;
    uint8_t  reserved[40];
    uint32_t crc32;             /* ISO-3309 over offset 0..crc32 */
} alp_hw_info_eeprom_t;          /* 128 bytes total */
```

The field widths above are plain integers, not the header's
`ALP_HW_INFO_*_LEN` macro names, because this doc's audience is
decoding raw manifest bytes off an EEPROM, not compiling C -- but the
numbers themselves are transcribed straight from those macros, not
independently chosen: `ALP_HW_INFO_FAMILY_LEN`, `ALP_HW_INFO_SKU_LEN`,
`ALP_HW_INFO_HW_REV_LEN`, and `ALP_HW_INFO_SERIAL_LEN`.
`scripts/check_board_id_doc_parity.py` resolves those macros and
re-checks the typedef block above against them on every PR; it does
not parse this paragraph, which is unenforced prose -- the typedef
block above is the one the gate actually holds in sync.

The CRC32 polynomial matches Python's `zlib.crc32` (poly
`0xEDB88320`, init `0xFFFFFFFF`, xor-out `0xFFFFFFFF`) so the
production-test programmer (`scripts/program_eeprom.py`) and the
runtime reader cannot disagree.

## Programming flow

```
production tool                          on-module EEPROM
─────────────────                        ────────────────
$ python scripts/program_eeprom.py \
      --board-yaml board.yaml \
      --serial ALP-V2N101-26W19-00042 \
      --mfg-date 2026-05-09 \
      --output build/eeprom-manifest.bin
        │
        ├── read som.sku (+ hw_rev) from board.yaml
        ├── resolve family from the SKU's metadata/e1m_modules preset
        ├── pack 128 bytes per <alp/hw_info.h>
        ├── append zlib.crc32 over offset 0..(crc32-1)
        └── write the 128-byte blob to --output
                                          ┌──────────────┐
                                          │ offset 0:    │
                                          │  ALPH..CRC32 │
                                          └──────────────┘
                                          A separate production-test
                                          fixture writes --output's
                                          bytes to the EEPROM over
                                          I²C -- program_eeprom.py
                                          itself never touches hardware.
```

The maintainer runs this script during board assembly QC, then the
test fixture writes its output to the module.  Failed boards (CRC
mismatch on read-back) are quarantined for rework.

## Runtime read flow

```
                                        ┌──────────────────────────────┐
                                        │  on-module 24C128 EEPROM     │
                                        │  (offset 0, 128 bytes)        │
                                        └───────────┬──────────────────┘
                                                    │ I2C read
                                                    ▼
alp_hw_info_read(out)
   ├── eeprom_24c128_init(...) on ALP_E1M_I2C0 (V2N) / SoC I2C2,
   │     DesignWare i2c_dw (AEN -- bridge/DNP-selected, NOT LPI2C0)
   ├── eeprom_24c128_read(0, &manifest, 128)
   ├── verify manifest.magic == "ALPH"
   ├── verify manifest.schema_version == 1
   ├── verify zlib.crc32(0..crc32-1) == manifest.crc32
   ├── copy family / sku / hw_rev / serial into out
   ├── classify: magic (else NOT_PROVISIONED) → schema/CRC (else IO)
   └── return ALP_OK
```

Application code can then assert:

```c
alp_hw_info_t info;
alp_hw_info_read(&info);
alp_hw_info_assert_matches_build(&info,
                                  /* expected_sku    */ "E1M-V2N101",
                                  /* expected_hw_rev */ "r1");
```

### `hw_rev` carries the full board designator

The field holds `<board_datecode>-<revision key>`, e.g. **`2626-r2`** — the
physical board is `E1M-AEN-2626-R2`, where `2626` is a YYWW datecode carried by
the Altium board number. A module whose manifest said only `r2` could not be tied
back to its board number, so `scripts/program_eeprom.py` composes the two.

The datecode is a **family** property, declared once as `board_datecode:` in
`metadata/e1m_modules/<family>/hw-revisions.yaml` and shared by every revision of
that PCB. The revision **key** stays bare (`r2`) everywhere else — `board.yaml`,
the loader's `hw_revisions` lookup, `pad_route_overrides` — because that is the key
those tables are indexed by. Only the identity written onto the module carries the
composed form, since only the module needs to name its own board. A family that
declares no `board_datecode` gets the bare key, unchanged.

> **The field has no slack.** `ALP_HW_INFO_HW_REV_LEN` is 8, so the composed
> string may be at most 7 characters plus its NUL. `2626-r2` is exactly 7. A
> revision key of `r10` (`2626-r10`, 8 chars) will not fit, and
> `program_eeprom.py` refuses it rather than truncating a board number. Widening
> the field is a `schema_version` bump. This is pinned by
> `TestBoardDatecode` in `tests/scripts/test_program_eeprom.py`.

A blank EEPROM (no `ALPH` magic) returns `ALP_ERR_NOT_PROVISIONED`; a
manifest whose magic is present but whose `schema_version` or CRC32 is
wrong returns `ALP_ERR_IO`. `alp_hw_info_assert_matches_build()`
returns `ALP_ERR_IO` on a SKU/hw_rev disagreement. Application code
can log and continue, or halt boot, depending on safety requirements.

## V2N-specific specifics

* **EEPROM**: Onsemi `N24S128C4DYT3G` on `ALP_E1M_I2C0` (Renesas RIIC0,
  `P31`/`P30`).  Alternate footprint `M24128-BFMH6TG` (STMicro) is
  pin-compatible; not assembled by default.
* **Kconfig**: enable `CONFIG_ALP_SDK_HW_INFO=y`, set
  `CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` to the bus id matching
  ALP_E1M_I2C0 in the studio-generated DT alias.

## V2N-M1 specifics

Same EEPROM manifest; the manifest's `family` field reads
`v2n-m1` and the `sku` field reads `E1M-V2M*`.  Application code
that handles both base + M1 in one image should branch on `family`,
not `sku`.

## See also

* [`<alp/hw_info.h>`](../include/alp/hw_info.h) -- public API.
* [`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c) --
  Zephyr-side reader.
* [`scripts/program_eeprom.py`](../scripts/program_eeprom.py) --
  production-test programmer.
* [`metadata/e1m_modules/v2n/hw-revisions.yaml`](../metadata/e1m_modules/v2n/hw-revisions.yaml) --
  V2N hw-rev registry (revision ids + SDK-version gating).
