# vendors/fmt

Vendored copy of **{fmt}** -- <https://github.com/fmtlib/fmt>,
tag `11.0.2`, MIT licensed.

## Why vendored instead of west-fetched

fmt is header-only (used in `FMT_HEADER_ONLY` mode) and has no
Zephyr `module.yml`, so it cannot ride in via an `extras-tier1`-style
west project. A `west.yml` pin is still recorded (behind the
disabled `extras-cpp` group) as the audit trail / version
source-of-truth, but the actual build resolves `#include <fmt/core.h>`
against the copy here.

## What's here

`include/fmt/` is the complete, unmodified upstream `include/fmt/`
tree (14 headers as of 11.0.2 -- `core.h`/`format.h` plus the
optional `chrono.h` / `ranges.h` / `os.h` / ... headers an app may
pull in). `LICENSE` is the upstream MIT license text, unmodified.

## Compile-time configuration

`metadata/library-profiles/fmt/fmt_config.h` sets
`FMT_HEADER_ONLY=1`, `FMT_USE_IOSTREAM=0`, `FMT_EXCEPTIONS=0` to
match the SDK's no-`<iostream>` / no-exceptions invariant. Unlike
ETL, fmt has no auto-located config-header hook -- an app must
`#include "fmt_config.h"` (resolves via the include path;
`metadata/library-profiles/fmt` is wired alongside
`vendors/fmt/include` in `zephyr/CMakeLists.txt`) **before** any
`<fmt/...>` header so the defines take effect.

## Updating

Bump the `revision:` in `west.yml`'s `extras-cpp` group entry, then
re-copy `include/fmt/` from the matching upstream tag and refresh
`LICENSE` here in the same commit.

## License

MIT (see `LICENSE`). Public/redistributable per
`classifying-public-vs-internal`.
