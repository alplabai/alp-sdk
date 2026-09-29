# vendors/optiga-trust-m

Vendored subset of Infineon's **OPTIGA Trust M host library**, the
transport under `chips/optiga_trust_m`.

- Upstream: <https://github.com/Infineon/optiga-trust-m>
- Version: tag `release-v5.8.3`, commit `03a3ebe8e87216cb60b8572eafa1e46c13fb87fb`
- License: MIT (`LICENSE`, upstream text unmodified)

**Status: SDK-internal dependency.**  It is compiled in whenever the
`optiga_trust_m` chip driver is (`CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M` on
Zephyr, `-DALP_SDK_CHIP_OPTIGA_TRUST_M=ON` for `libalp_chips`); apps do
not enable it through `board.yaml`.  The source list lives in
`sources.cmake`, shared by both builds.

## What is here

| Path | Origin |
|---|---|
| `include/` | upstream `include/`, minus `pal/pal_linux.h` |
| `src/cmd`, `src/common`, `src/comms`, `src/util` | upstream, byte-identical |
| `pal_alp/pal_os_datastore.c` | upstream `extras/pal/zephyr/pal_os_datastore.c` (portable RAM store), byte-identical |
| `pal_alp/pal_alp.c`, `pal_alp.h` | Alp Lab: the PAL on `alp_i2c_*` + `alp_uptime_ms` / `alp_delay_ms` |
| `pal_alp/optiga_lib_config_alp.h` | Alp Lab: upstream's Trust M V3 config, Shielded Connection off |

Not vendored: `src/crypt` (`optiga_crypt_*`, lands with the PSA driver),
upstream's other PALs, examples, tests and `external/` (mbedTLS).

## Why a custom PAL

Upstream's Linux PAL opens `/dev/i2c-N` itself and its Zephyr PAL binds
`DT_ALIAS(optiga_i2c)`; both would drive the bus behind the SDK's
`alp_i2c` handle.  `pal_alp.c` goes through the portable API instead,
so one driver serves the A55 and MCU cores.  It is threadless: the
library's timed callbacks are recorded with a due time and run from the
driver's wait loop (`pal_alp.h`).

## Updating

Replace the upstream-origin files from a new release tag, keep them
byte-identical, and update the version line above.  They are built with
`-w` and excluded from the clang-format gate (`vendors/**`), so no
local edits are needed to keep the gates green.
