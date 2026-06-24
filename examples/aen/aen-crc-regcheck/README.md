# aen-crc-regcheck -- hardware CRC engine (alif,crc) known-value check

On-silicon validation for the **E1M-AEN801** (Alif Ensemble E8, M55-HE),
via the bench RAM-run + RAM-console flow.

This app binds the hardware **CRC engine** at `crc0@48107000` through the
portable Zephyr **CRC class** API (`crc_begin` / `crc_update` /
`crc_finish` over `struct crc_ctx`, from `<zephyr/drivers/crc.h>`) over
the alp-sdk clean-room `crc_alif` driver. It computes **CRC32/IEEE** over
a fixed 16-byte buffer and checks the result against a host-precomputed
reference -- both in firmware (`crc_verify()`) and over J-Link
(`mem32 0x48107018`, the `CRC_OUT` register).

## This is a CLEAN-ROOM driver, not a vendored one

The Apache-2.0 `zephyr_alif` fork ships a CRC driver, but it targets a
**fork-private API** that does **not** exist in pinned upstream Zephyr
v4.4: `crc_compute()` + `struct crc_params` + `crc_set_seed()`, gated by
`CONFIG_CRC_DRV` (see `sdk-alif/samples/drivers/crc`). It therefore
**cannot be consumed verbatim** and there is no Tier-2 fork path to
retire onto.

The alp-sdk driver (`zephyr/drivers/crc/crc_alif.c`) is instead
**re-authored from scratch** against the **upstream** CRC class API
(`crc_begin`/`update`/`finish`), with the register model transcribed
**value-only** (clean-room) from the proprietary Alif DFP and the
programming **sequence** cross-checked against the fork CMSIS reference
driver. ADR 0017 **Tier-1.5** (in-tree-thin over the silicon).

## Grounded facts (every concrete value cited)

| Fact | Value | Source |
|------|-------|--------|
| CRC node (E8) | `crc0: crc@48107000` `alif,crc` (also `crc1@48108000`) | DFP `soc.h` `CRC0_BASE 0x48107000` / `CRC1_BASE 0x48108000`, transcribed into `zephyr/dts/alif/ensemble_e8_peripherals.dtsi` |
| reg size | `0x100` | DFP `soc.h` `CRC_Type` "Size = 256 (0x100)" |
| `CRC_CONTROL` offset | `+0x00` | DFP `soc.h` `CRC_Type` |
| `CRC_SEED` offset | `+0x10` | DFP `soc.h` `CRC_Type` |
| `CRC_POLY_CUSTOM` offset | `+0x14` | DFP `soc.h` `CRC_Type` |
| `CRC_OUT` offset | `+0x18` -> J-Link `mem32 0x48107018` | DFP `soc.h` `CRC_Type` |
| 8-bit data-in offset | `+0x20` | DFP `crc.h` `CRC_DATA_IN_8BIT_REG_OFFSET` |
| 32-bit data-in offset | `+0x60` | DFP `crc.h` `CRC_DATA_IN_32BIT_REG_OFFSET` |
| algo selects | `CRC_8_CCITT 0<<3`, `CRC_16 2<<3`, `CRC_16_CCITT 3<<3`, `CRC_32 4<<3`, `CRC_32C 5<<3` | DFP `crc.h` |
| size selects | `8: 0<<1`, `16: 1<<1`, `32: 2<<1` | DFP `crc.h` |
| option bits | `REFLECT 1<<11`, `INVERT 1<<10`, `CUSTOM_POLY 1<<9`, `BIT_SWAP 1<<8`, `BYTE_SWAP 1<<7`, `INIT_BIT 1<<0` | DFP `crc.h` |
| interrupt | none -- polled engine | DFP `crc.c` / `Driver_CRC.c` use no IRQ |
| Driver IP | CRC class, `DT_DRV_COMPAT alif_crc` | clean-room `crc_alif.c` (upstream `crc_begin/update/finish` API) |
| Distinct from fork | fork `crc_compute()` / `struct crc_params` / `CONFIG_CRC_DRV` -- NOT upstream | `sdk-alif/samples/drivers/crc` |
| Kconfig | `CRC_ALIF`, `depends on CRC_DRIVER` + `DT_HAS_ALIF_CRC_ENABLED` | `zephyr/Kconfig` |
| reference value | `0x684FC31C` = `zlib.crc32(fixed_buf)` | Python host precompute (see `src/main.c`) |

**No register value is invented** -- each offset/bit is the DFP `#define`
value with an inline citation in `crc_alif.c`. The needed IRQ would also
be transcribed, but the CRC engine **has none** (it is polled).

## How the upstream `enum crc_type` maps to the engine

| `enum crc_type` | engine algorithm | polynomial (validated) | input width |
|-----------------|------------------|------------------------|-------------|
| `CRC8_CCITT` | `CRC_8_CCITT` | `0x07` | byte-fed |
| `CRC16` | `CRC_16` | `0x8005` | byte-fed |
| `CRC16_CCITT` | `CRC_16_CCITT` | `0x1021` | byte-fed |
| `CRC32_IEEE` | `CRC_32` | `0x04C11DB7` | word-fed (len % 4 == 0) |
| `CRC32_C` | `CRC_32C` (custom-poly reg) | `0x1EDC6F41` | word-fed (len % 4 == 0) |

Any other `enum crc_type` returns **`-ENOTSUP`**.

## BENCH-CONFIRM: reflect / final-XOR ordering (the open item)

The canonical CRC-32 reflects input + output **and** one's-complements
the result. The Alif engine drives reflection from its `CRC_CONTROL`
`REFLECT` bit and final inversion from the `INVERT` bit. The exact
**order** the silicon applies reflect vs. invert (and whether the seed is
pre- or post-reflected) is the single thing this regcheck settles on the
bench. The driver maps `CRC_FLAG_REVERSE_*` -> `REFLECT`; if the computed
value does **not** equal `0x684FC31C`:

1. read `CRC_OUT` (`mem32 0x48107018`) over J-Link;
2. compare the **raw** (non-reflected, non-inverted) hardware value to
   the host's intermediate CRC to localise which post-processing step the
   driver must add;
3. **do NOT guess** the order without the silicon answer.

The `RESULT FAIL` branch in `src/main.c` calls this out explicitly. This
is a documented bench-confirm item, **not** a code defect.

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```sh
export ZEPHYR_BASE=<zephyr-base>
west build -p always -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-crc-regcheck -d build/aen-crc-regcheck -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>"
```

The board overlay `boards/alp_e1m_aen801_m55_he.overlay` auto-applies for
the matching board target (Zephyr's per-board overlay convention) -- it
`okay`s `crc0` and retargets `zephyr,flash = &itcm` for the RAM-run.

## Bench run (human-operated; not done by the SDK)

1. Build (above) -> `build/aen-crc-regcheck/zephyr/zephyr.bin`.
2. J-Link (generic `Cortex-M55`): `loadbin` the image to ITCM, set PC,
   run.
3. Read the RESULT line from the `ram_console_buf` symbol over SWD
   (`mem8`, ASCII-decode) -- the bench UART is not USB-routed.
4. Ground truth: `mem32 0x48107018` (engine `CRC_OUT`) after the run
   holds the last computed CRC.
5. `RESULT PASS:` = computed CRC matches `0x684FC31C`. `RESULT FAIL: ...
   crc mismatch` -> follow the reflect/invert bench-confirm steps above.

**BENCH-VALIDATION app -- not a customer teaching example.**
ADR 0017 Tier-1.5 (clean-room in-tree-thin over the silicon), task #21.
