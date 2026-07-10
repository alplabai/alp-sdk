# vendors/catch2

Vendored copy of Catch2's **amalgamated single-TU distribution** --
<https://github.com/catchorg/Catch2>, tag `v3.7.1`, Boost Software
License 1.0.

## Why vendored instead of west-fetched

Catch2 is a generic CMake project, not a Zephyr module -- it ships no
`zephyr/module.yml`, so it cannot ride in via Zephyr's usual
`ZEPHYR_MODULES` CMakeLists.txt/Kconfig auto-include (it never becomes
a `zephyr_library()`). A `west.yml` pin is still recorded (behind the
disabled `extras-tier1` group) as the audit trail / version
source-of-truth for a customer who wants the *full* Catch2 source tree
(the generator-based multi-TU build, CMake package config, etc.); the
`examples/testing/catch2-selftest` teaching example itself builds
against the copy here so it needs no external checkout in CI. This
matches the `vendors/doctest/` precedent for the SDK's other
single/few-file C++ test framework.

## What's here

Catch2 ships an official "amalgamated" distribution
(`extras/catch_amalgamated.{hpp,cpp}`) specifically for consumers who
don't want the generator/multi-TU build -- a single header +
single translation unit carrying the whole framework (v3, C++14).
That's what's vendored here, unmodified:

- `include/catch_amalgamated.hpp` -- declarations (`#include
  <catch_amalgamated.hpp>`, flat, matching upstream's own convention
  for this distribution -- NOT `<catch2/catch_all.hpp>`, which is the
  *source-tree* convention this vendored copy does not use).
- `src/catch_amalgamated.cpp` -- the one implementation TU (`#include
  "catch_amalgamated.hpp"` relative, so both files must share a
  directory or the include path must cover both -- see
  `zephyr/CMakeLists.txt`'s wiring).

## Compile-time configuration

No profile header. The example's own `target_compile_definitions`
(`CATCH_AMALGAMATED_CUSTOM_MAIN`, `CATCH_CONFIG_NO_POSIX_SIGNALS`) are
per-consumer choices (main-function ownership, native_sim POSIX-signal
conflict), not SDK-wide invariants -- see
`examples/testing/catch2-selftest/CMakeLists.txt`.

## License

Boost Software License 1.0 (see `LICENSE.txt`) -- permissive,
compatible with the SDK's Apache-2.0 terms. Public/redistributable per
`classifying-public-vs-internal`.

## Updating

Bump the `revision:` in `west.yml`'s `extras-tier1` group entry, then
re-copy both `extras/catch_amalgamated.{hpp,cpp}` from the matching
upstream tag and refresh `LICENSE.txt` here in the same commit.

## See also

- [`examples/testing/catch2-selftest/README.md`](../../examples/testing/catch2-selftest/README.md)
  -- the teaching example this vendored copy backs.
- [`vendors/doctest/README.md`](../doctest/README.md) -- companion
  single-header C++ test framework, same vendoring rationale.
