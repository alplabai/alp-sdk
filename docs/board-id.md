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
    uint32_t magic;             /* "ALPH" -- 0x41 0x4C 0x50 0x48 */
    uint16_t schema_version;    /* currently 1 */
    char     family[16];        /* e.g. "v2n", "v2n-m1", "aen" */
    char     sku[24];           /* e.g. "E1M-V2N101"            */
    char     hw_rev[8];         /* e.g. "r1"                    */
    char     serial[32];        /* free-form, vendor-defined    */
    uint16_t mfg_year;
    uint8_t  mfg_month;
    uint8_t  mfg_day;
    uint8_t  reserved[__];
    uint32_t crc32;             /* ISO-3309 over offset 0..crc32 */
} alp_hw_info_eeprom_t;
```

The CRC32 polynomial matches Python's `zlib.crc32` (poly
`0xEDB88320`, init `0xFFFFFFFF`, xor-out `0xFFFFFFFF`) so the
production-test programmer (`scripts/program_eeprom.py`) and the
runtime reader cannot disagree.

## Programming flow

```
production tool                          on-module EEPROM
─────────────────                        ────────────────
$ python scripts/program_eeprom.py \
      --bus /dev/i2c-N \
      --addr 0x50 \
      --family v2n \
      --sku E1M-V2N101 \
      --hw-rev r1 \
      --serial ALP-V2N101-26W19-00042 \
      --mfg-date 2026-05-09
        │
        ├── pack 128 bytes per <alp/hw_info.h>
        ├── append zlib.crc32 over offset 0..(crc32-1)
        └── i2c write to offset 0
                                          ┌──────────────┐
                                          │ offset 0:    │
                                          │  ALPH..CRC32 │
                                          └──────────────┘
```

The maintainer runs this script during board assembly QC.  Failed
boards (CRC mismatch on read-back) are quarantined for rework.

## Runtime read flow

```
                                        ┌──────────────────────────────┐
                                        │  on-module 24C128 EEPROM     │
                                        │  (offset 0, 128 bytes)        │
                                        └───────────┬──────────────────┘
                                                    │ I2C read
                                                    ▼
alp_hw_info_read(out)
   ├── eeprom_24c128_init(...) on ALP_E1M_I2C0 (V2N) / Alif LPI2C (AEN)
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
