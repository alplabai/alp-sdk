# Board identification — SoM EEPROM manifest + BOARD_ID ADC

Two-stage boot-time identification flow that the ALP SDK uses to
confirm what hardware the firmware is running on:

1. **EEPROM manifest** -- 128-byte programmed-once data block in
   the SoM's on-module 24C128 EEPROM that carries family, SKU,
   hardware revision, serial number, and manufacturing date.
   Implemented today in [`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c)
   as the EEPROM-side half of `alp_hw_info_read()`.
2. **BOARD_ID ADC cross-check** -- a per-revision resistor divider
   wired to a SoM ADC channel that the firmware samples at boot
   and compares against a generated table.  The ADC cross-check
   has runtime-readable hooks but is currently a **no-op stub**
   pending the per-family generated header from
   `scripts/alp_project.py` (TODO marker in
   [`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c)).

The two-stage design protects against:

* **Wrong EEPROM swap.** Someone moves an EEPROM from a V2N module
  to a V2N-M1 module.  EEPROM says "V2N base"; ADC says "V2N-M1".
  Mismatch caught.
* **Wrong firmware build.** Application links against the wrong
  metadata-driven config.  `alp_hw_info_assert_matches_build()` is
  the explicit check that catches a build pointed at the wrong
  SoM SKU.

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
production-test programmer (`tools/program_eeprom.py`) and the
runtime reader cannot disagree.

## Programming flow

```
production tool                          on-module EEPROM
─────────────────                        ────────────────
$ python tools/program_eeprom.py \
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
   ├── eeprom_24c128_init(...) on E1M_I2C0 (V2N) / Alif LPI2C (AEN)
   ├── eeprom_24c128_read(0, &manifest, 128)
   ├── verify manifest.magic == "ALPH"
   ├── verify manifest.schema_version == 1
   ├── verify zlib.crc32(0..crc32-1) == manifest.crc32
   ├── copy family / sku / hw_rev / serial into out
   ├── adc_cross_check(manifest, out)   ← currently no-op TODO
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

Mismatch returns `ALP_ERR_IO`; application code can choose to log
and continue, or halt boot, depending on safety requirements.

## BOARD_ID ADC cross-check (TODO)

Each SoM family has an 8-bin resistor divider tied to a single SoC
ADC channel.  The divider voltage encodes the hardware revision.
On V2N the divider is on **ADC2_CH7** per
[`metadata/e1m_modules/v2n/hw-revisions.yaml`](../metadata/e1m_modules/v2n/hw-revisions.yaml).

Today's hook in [`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c):

```c
static alp_status_t adc_cross_check(const alp_hw_info_eeprom_t *manifest,
                                    alp_hw_info_t *out)
{
    (void)manifest;
    (void)out;
    return ALP_OK; /* No-op until the generated header lands. */
}
```

To make this real, `scripts/alp_project.py` needs to emit a
per-family generated header listing `expected_mv / bin_radius_mv`
per `hw_rev`.  The header would look like:

```c
/* AUTO-GENERATED from metadata/e1m_modules/v2n/hw-revisions.yaml */
typedef struct { const char *hw_rev; uint16_t mv; uint16_t radius; } alp_hwrev_bin_t;
static const alp_hwrev_bin_t alp_v2n_hwrev_bins[] = {
    { "r1", 100, 50 },
    { "r2", 300, 50 },
    /* ... 8 bins total ... */
};
```

The `adc_cross_check` body would then `alp_adc_open(BOARD_ID_CHAN)`,
sample, look up the rev whose `(mv ± radius)` contains the reading,
and compare against `manifest->hw_rev`.  Mismatch -> `ALP_ERR_IO`.

The divider math is `expected_mv = 1800 × R_bot / (R_top + R_bot)`;
the JSON-schema validator that `scripts/check-hw-rev-bins.py` will
run in CI checks each `hw-revisions.yaml` row for non-overlap of
bin radii (since two adjacent bins must be reliably distinguishable
across resistor tolerance + ADC quantisation).

## V2N-specific specifics

* **EEPROM**: Onsemi `N24S128C4DYT3G` on `E1M_I2C0` (Renesas RIIC0,
  `P31`/`P30`).  Alternate footprint `M24128-BFMH6TG` (STMicro) is
  pin-compatible; not assembled by default.
* **BOARD_ID ADC**: ADC2 channel 7 on the Renesas RZ/V2N.  See
  [`metadata/e1m_modules/v2n/hw-revisions.yaml`](../metadata/e1m_modules/v2n/hw-revisions.yaml)
  for the 8-bin divider.
* **Kconfig**: enable `CONFIG_ALP_SDK_HW_INFO=y`, set
  `CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` to the bus id matching
  E1M_I2C0 in the studio-generated DT alias.

## V2N-M1 specifics

Same EEPROM and BOARD_ID ADC; the manifest's `family` field reads
`v2n-m1` and the `sku` field reads `E1M-V2M*`.  Application code
that handles both base + M1 in one image should branch on `family`,
not `sku`.

## See also

* [`<alp/hw_info.h>`](../include/alp/hw_info.h) -- public API.
* [`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c) --
  Zephyr-side reader.
* [`tools/program_eeprom.py`](../tools/program_eeprom.py) --
  production-test programmer.
* [`metadata/e1m_modules/v2n/hw-revisions.yaml`](../metadata/e1m_modules/v2n/hw-revisions.yaml) --
  V2N hw-rev divider definition (mirrored for V2N-M1 / AEN families).
