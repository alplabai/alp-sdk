# catch2-selftest

A minimal [Catch2](https://github.com/catchorg/Catch2) v3
unit-test binary, built from Catch2's own "amalgamated" single-TU
distribution: 3 `TEST_CASE`s over a trivial `clamp()` helper.

**[UNTESTED]** -- host-run only. This is a HOST unit-test example,
not a firmware demo: the binary builds and runs on `native_sim` and
exits 0. It has never run on real E1M silicon and there is no
board-specific behavior to bench-verify -- Catch2 is a pure C++ test
framework with no hardware surface.

## What this shows

* How a Catch2-based test binary is wired into an Alp SDK project
  via `board.yaml`'s `libraries: [catch2]` -- no `<alp/*>` wrapper,
  just Catch2's own API (`TEST_CASE`, `CHECK`, `Catch::Session`).
* The **west-module-without-a-Zephyr-module** case: Catch2
  (`catchorg/Catch2`, west.yml path `modules/lib/catch2`) is a
  large, generic CMake project with no `zephyr/module.yml`, so it
  cannot ride in via Zephyr's usual
  `ZEPHYR_EXTRA_MODULES`/module-scan auto-include the way `nanopb`
  or `hal_ethos_u` do. This example's own `CMakeLists.txt` locates
  the fetched checkout (via the `ZEPHYR_MODULES` CMake cache
  variable -- see Build below) and compiles Catch2's recommended
  amalgamated pair, `extras/catch_amalgamated.{hpp,cpp}`, directly
  as extra sources on the `app` target.
* A custom `main()` (`CATCH_AMALGAMATED_CUSTOM_MAIN`, not the
  amalgamated file's built-in default `main()`) so the example can
  print the SDK's uniform `[catch2-selftest] done` marker only after
  Catch2 reports zero test failures.
* **The `int main(void)` gotcha.** Zephyr's kernel init
  (`kernel/init.c`) calls the application's `main()` as `extern int
  main(void); main();` unless `CONFIG_BOOTARGS=y` -- no argc/argv are
  ever placed on the stack/registers. `src/main.cpp` deliberately
  declares `int main(void)`, NOT `main(int argc, char *argv[])`: the
  first draft of this example used the latter and read garbage
  argc/argv values straight into `Catch::Session::applyCommandLine()`,
  which walked the bogus `argv` array and segfaulted before printing
  anything. There's no host command line to parse on native_sim
  anyway, so the fix is simply not to ask for one.

## Build

Catch2 has no `zephyr/module.yml`, so a plain `west build` won't
locate it -- you must point `ZEPHYR_MODULES` at BOTH the SDK root
and the Catch2 checkout (`modules/lib/catch2` in a west workspace
that pins `catchorg/Catch2`):

```bash
west twister -T examples/testing/catch2-selftest -p native_sim/native/64 \
    -x ZEPHYR_MODULES="<alp-sdk-root>;<path-to-catch2-checkout>" -v
```

Equivalently with a direct `west build` (bypassing twister):

```bash
west build -b native_sim/native/64 examples/testing/catch2-selftest \
    -- -DZEPHYR_MODULES="<alp-sdk-root>;<path-to-catch2-checkout>"
west build -t run
```

If Catch2 isn't fetched as a west module in your workspace, pass its
path directly instead: `-DCATCH2_MODULE_DIR=<path-to-catch2-checkout>`.

## Expected output

`Randomness seeded to: <N>` varies run to run (Catch2 shuffles test
order by a random seed unless told otherwise) -- everything else is
fixed:

```
Randomness seeded to: 1405010408
===============================================================================
All tests passed (6 assertions in 3 test cases)

[catch2-selftest] done
```

## Adding tests

1. Add a `TEST_CASE("description", "[tag]") { CHECK(...); }` block
   to `src/main.cpp` (or a new `.cpp` added to `target_sources` in
   `CMakeLists.txt` -- Catch2 test cases self-register across
   translation units via static initializers, no manual list to
   maintain).
2. Use `CHECK()` to record a failure and keep running the rest of
   the test case, or `REQUIRE()` to abort the test case on first
   failure (e.g. before dereferencing a pointer `REQUIRE()`d
   non-null).
3. Tags (the `"[clamp]"` string in this example) let you filter a
   run: `./catch2_selftest "[clamp]"` runs only tests carrying that
   tag -- handy once a suite grows past a couple of dozen cases.

## HW swap note

This example never touches `peripherals:` or a chip driver, so
there's no hardware config to swap -- `board.yaml`'s `som.sku`/
`preset` are only present because `scripts/alp_project.py` needs a
SoM to derive the baseline SoC config from. Point a real test binary
at whichever `<alp/*>` peripheral APIs it needs to exercise, same as
any other example.
