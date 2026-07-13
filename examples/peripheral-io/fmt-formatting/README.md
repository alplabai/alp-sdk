# fmt-formatting

Formats an `int` + a `float` + a `const char *` into a fixed
`char[64]` stack buffer using `fmt::format_to` -- {fmt}
(https://github.com/fmtlib/fmt)'s API for writing formatted text
through a caller-supplied output iterator instead of returning an
owning `std::string`. No heap allocation, no `<iostream>`.

**[UNTESTED]** on real silicon -- this example is verified on
`native_sim` only, per the 3rd-party-library examples plan. fmt is
header-only and portable; on-target the same `#include`s and API
calls build and run unchanged (only the C++ toolchain knobs in
`prj.conf` matter, and those are native_sim/host-libstdc++-specific --
see prj.conf's comment on `CONFIG_REQUIRES_FULL_LIBCPP`).

## What this shows

* `board.yaml`'s `libraries: [fmt]` -> the loader adds
  `vendors/fmt/include` (the vendored fmt 11.0.2 headers, Task 0) and
  `metadata/library-profiles/fmt/fmt_config.h` to the include path.
* `fmt_config.h`'s three invariants: `FMT_HEADER_ONLY=1` (no separate
  compiled library, no static-init concern), `FMT_USE_IOSTREAM=0` (no
  `<iostream>`, saves ~30 KB on M-class), `FMT_EXCEPTIONS=0`
  (format-string errors fault instead of throwing).
* `fmt::format_to(buf, "...", args...)`: writes through a plain
  `char*` output iterator into stack memory the caller owns, and
  returns an iterator one-past-the-last-character-written -- the
  caller null-terminates (or bounds-checks) itself, exactly like
  `snprintf()`.
* Formatting three different types (`int`, `float` with `{:.2f}`
  precision, `const char *`) in a single format string.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/peripheral-io/fmt-formatting \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

```
[fmt-formatting] buffer: "count=7 ratio=3.14 label=sensor-a"
[fmt-formatting] done
```

## `fmt::format_to` vs `fmt::format_to_n`

This example uses `fmt::format_to`, which does **not** stop at a
buffer boundary -- it trusts the caller's format string + buffer size
the way `sprintf()` does. If the format string or argument sizes are
attacker- or runtime-controlled, use `fmt::format_to_n(buf, sizeof(buf)
- 1, ...)` instead, which truncates safely and returns how many
characters it *would* have written (`format_to_n_result::size`) so you
can detect truncation.

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
- [`metadata/library-profiles/README.md`](../../../metadata/library-profiles/README.md)
  -- the profile-header mechanism.
- Upstream fmt docs: https://fmt.dev/
