# vendors/etl

Vendored copy of the **Embedded Template Library** (ETL) --
<https://github.com/ETLCPP/etl>, tag `20.39.4`, MIT licensed.

## Why vendored instead of west-fetched

ETL is header-only C++ (no `.c`/`.cpp` compiled) and has no Zephyr
`module.yml`, so it cannot ride in via the existing
`extras-tier1`-style west projects. A `west.yml` pin is still
recorded (behind the disabled `extras-cpp` group, see the repo-root
`west.yml`) as the audit trail / version source-of-truth, but the
actual build resolves `#include "etl/vector.h"` etc. against the
copy here -- not the pin. This matches the existing
`vendors/lwrb/` and `vendors/nanopb/` precedent for
libraries with no fetched module.

## What's here

`include/etl/` is the upstream `include/etl/` header tree (363
files as of 20.39.4). ETL's containers have deep header
interdependencies (`etl::vector` alone pulls in ~15 supporting
headers for error handling, type traits, iterators, etc.) -- there
is no meaningful "just the headers I need" subset, so the whole
public header tree is vendored, matching how every other
ETL-consuming project treats it. Pruned from the upstream drop:
`.vscode/settings.json` (editor config), `generators/*.bat`
(Windows code-gen launcher scripts, not headers -- the
`generators/*_generator.h` headers they invoke are kept), and three
`experimental/mid_point*.png` doc screenshots -- none of the three
have any build value.

`LICENSE` is the upstream MIT license text, unmodified.

## Compile-time configuration

`metadata/library-profiles/etl/etl_profile.h` is ETL's expected
config-header filename (ETL's core headers
`#include "etl_profile.h"` unqualified) -- the SDK's profile sets
`ETL_NO_STL` / `ETL_NO_EXCEPTIONS` to match the no-heap-on-hot-path
/ no-exceptions invariant. Both `vendors/etl/include` and
`metadata/library-profiles/etl` are on the Zephyr build's include
path (see `zephyr/CMakeLists.txt`) whenever `alp-sdk` is enabled,
so the profile header wins over any app-local default.

## Updating

Bump the `revision:` in `west.yml`'s `extras-cpp` group entry, then
re-copy `include/etl/` from the matching upstream tag and refresh
`LICENSE` here in the same commit.

## License

MIT (see `LICENSE`). Public/redistributable -- passes every
`classifying-public-vs-internal` gate (ALP has no modifications,
no vendor-gated content, no SoM design/identity detail).
