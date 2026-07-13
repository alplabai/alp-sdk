# doctest-selftest

A minimal [doctest](https://github.com/doctest/doctest) v2.4.11
unit-test binary: 3 `TEST_CASE`s over a trivial `clamp()` helper.

**[UNTESTED]** -- host-run only. This is a HOST unit-test example,
not a firmware demo: the binary builds and runs on `native_sim` and
exits 0. It has never run on real E1M silicon and there is no
board-specific behavior to bench-verify -- doctest is a pure C++
test framework with no hardware surface.

## What this shows

* How a doctest-based test binary is wired into an Alp SDK project
  via `board.yaml`'s `libraries: [doctest]` -- no `<alp/*>` wrapper,
  just doctest's own API (`TEST_CASE`, `CHECK`, `doctest::Context`).
* The **vendoring** mechanism: doctest is a genuine single-header
  library with no Zephyr `zephyr/module.yml`, so it can't ride in as
  a normal west-fetched module. It's vendored verbatim at
  `vendors/doctest/include/doctest/doctest.h` (Task 0 of this
  library-examples batch); `zephyr/CMakeLists.txt` adds that path
  plus `metadata/library-profiles/doctest/` to every app's include
  path unconditionally.
* The **profile header** contract: `doctest_config.h` must be
  `#include`d before `<doctest/doctest.h>` so its
  `DOCTEST_CONFIG_NO_POSIX_SIGNALS` / `DOCTEST_CONFIG_NO_MULTITHREADING`
  defines take effect ahead of doctest's own defaults -- see
  `metadata/library-profiles/doctest/doctest_config.h`.
* A custom `main()` (not `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`) so the
  example can print the SDK's uniform `[doctest-selftest] done`
  marker only after doctest reports zero test failures.
* **The `int main(void)` gotcha.** `src/main.cpp` deliberately
  declares `int main(void)`, not `main(int argc, char **argv)`:
  Zephyr's kernel init (`kernel/init.c`) calls the application's
  `main()` as `extern int main(void); main();` unless
  `CONFIG_BOOTARGS=y` -- no argc/argv are ever placed on the
  stack/registers, so asking for them reads garbage. See
  `catch2-selftest`'s README for the segfault this exact mistake
  caused there.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/testing/doctest-selftest \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

doctest needs no extra `ZEPHYR_MODULES` entry beyond the SDK itself
-- unlike Catch2 (`catch2-selftest`), it's vendored in-tree rather
than fetched as a west module.

## Expected output

```
[doctest] doctest version is "2.4.11"
[doctest] run with "--help" for options
===============================================================================
[doctest] test cases:  3 |  3 passed | 0 failed | 0 skipped
[doctest] assertions:  6 |  6 passed | 0 failed |
[doctest] Status: SUCCESS!
[doctest-selftest] done
```

## Adding tests

1. Add a `TEST_CASE("description") { CHECK(...); }` block to
   `src/main.cpp` (or a new `.cpp` added to `target_sources` in
   `CMakeLists.txt` -- doctest test cases self-register across
   translation units via static initializers, no manual list to
   maintain).
2. Use `CHECK()` to record a failure and keep running the rest of
   the test case, or `REQUIRE()` to abort the test case on first
   failure (e.g. before dereferencing a pointer `REQUIRE()`d
   non-null).
3. `SUBCASE("variant")` blocks inside a `TEST_CASE` share setup code
   but branch for each nested `SUBCASE` -- useful once a helper has
   more than a couple of input partitions worth covering.

## HW swap note

This example never touches `peripherals:` or a chip driver, so
there's no hardware config to swap -- `board.yaml`'s `som.sku`/
`preset` are only present because `scripts/alp_project.py` needs a
SoM to derive the baseline SoC config from. Point a real test binary
at whichever `<alp/*>` peripheral APIs it needs to exercise, same as
any other example.
