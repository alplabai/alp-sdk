# vendors/doctest

Vendored copy of **doctest** -- <https://github.com/doctest/doctest>,
tag `v2.4.11`, MIT licensed.

## Why vendored instead of west-fetched

doctest is a genuine single-header C++ test framework with no
Zephyr `module.yml`, so it cannot ride in via an `extras-tier1`-style
west project. A `west.yml` pin is still recorded (behind the
disabled `extras-cpp` group) as the audit trail / version
source-of-truth, but the actual build resolves
`#include <doctest/doctest.h>` against the copy here.

## What's here

`include/doctest/doctest.h` is the complete, unmodified upstream
single header. `LICENSE.txt` is the upstream MIT license text,
unmodified.

## Compile-time configuration

`metadata/library-profiles/doctest/doctest_config.h` sets
`DOCTEST_CONFIG_NO_POSIX_SIGNALS` / `DOCTEST_CONFIG_NO_MULTITHREADING`
/ `DOCTEST_CONFIG_NO_INCLUDE_TYPETRAITS` so the test runner builds
clean on Cortex-M targets with no POSIX signal handling. An app
must `#include "doctest_config.h"` before `#include <doctest/doctest.h>`
so the defines take effect (`metadata/library-profiles/doctest` is
wired alongside `vendors/doctest/include` in `zephyr/CMakeLists.txt`).

## Updating

Bump the `revision:` in `west.yml`'s `extras-cpp` group entry, then
re-copy `doctest.h` from the matching upstream tag and refresh
`LICENSE.txt` here in the same commit.

## License

MIT (see `LICENSE.txt`). Public/redistributable per
`classifying-public-vs-internal`.
