# Backend Registry Architecture

The Alp SDK dispatches every peripheral call through a linker-section
backend registry. This page tells you how to add a backend for a new
silicon family without editing existing files.

## Concepts

* **Class** — a peripheral category (`adc`, `spi`, `inference`, …).
  Each class has its own public header (`<alp/adc.h>`) and its own
  per-class linker section (`.alp_backends_adc`).

* **Backend** — one row in a class's section. Declares the silicon
  ref it supports, the vendor name, base capabilities, a priority,
  an ops vtable, and an optional probe function for per-instance
  refinement.

* **Capabilities** — two distinct layers:
  * `ALP_CAP_*` macros in `<alp/cap.h>` are SoC-level. They answer
    "does this silicon have an NPU at all?"
  * `ALP_INSTANCE_CAP_*` flags in `<alp/cap_instance.h>` are
    handle-level. They answer "does THIS opened ADC handle support
    DMA?" Populated by the backend's `ops->probe()` at open time.

## Adding a backend

1. Drop a new file under `src/backends/<class>/<vendor>.c`.
2. Implement the class's ops struct (signature is documented in the
   class's internal ops header — e.g. `src/backends/adc/adc_ops.h`).
3. Register it with `ALP_BACKEND_REGISTER(<class>, <name>, { … });`.
4. Declare the class table entry with `ALP_BACKEND_DEFINE_CLASS(<class>)`
   in the class dispatcher (only the dispatcher file does this, once).
5. Add the new file to the appropriate Kconfig / CMake list so it
   builds for the right SoM target.

That's it. The registry walker at `alp_backend_select()` picks up the
new entry automatically.

Adding a **new class** additionally needs one
`ALP_BACKEND_SECTION_ROM(<class>)` line in
`zephyr/linker/alp-backend-sections.ld`. Zephyr's ld-script boards
link with `--orphan-handling=warn` plus `--fatal-warnings`, so an
undeclared `alp_backends_<class>` section is a hard link error on
real hardware — and the omission never shows up in native CI, because
the host linker script used by `native_sim` auto-places orphan
sections.

A new class reachable from the **plain-CMake static build** also needs
a **static-archive anchor** (see below) so its section survives a
static-library link.

## Static-archive anchors (plain-CMake) — issue #368

Zephyr links whole objects (`zephyr_library`), so every backend
translation unit joins the link and its `alp_backends_<class>` section
is always present. A plain-CMake build instead links `libalp_sdk.a` as
an ordinary **static archive**, and a static link only pulls an archive
member when an already-included object references one of its symbols.

A backend TU (typically `sw_fallback.c`) is *nothing but* an
`ALP_BACKEND_REGISTER` row in the `alp_backends_<class>` section — no
code references it. So on a static link the member is never pulled, the
section is absent, and the `__start_/__stop_alp_backends_<class>` bounds
the (always-linked) dispatcher reads through `ALP_BACKEND_DEFINE_CLASS`
go **undefined** — the consumer link fails with
`undefined reference to __start_alp_backends_<class>`.

The fix is an **anchor**: the section-carrying backend TU exports one
global symbol via `ALP_BACKEND_ANCHOR_DEFINE(<class>)`, and the
dispatcher takes its address via `ALP_BACKEND_ANCHOR(<class>)`. The
dispatcher is always pulled (it owns `alp_<class>_open`), so the
address-of forces the backend member — and with it the section and its
bound symbols — into the link. Both macros are inert unless the
top-level CMake defines `ALP_BACKEND_STATIC_ANCHORS` (the non-Zephyr
build), so they are no-ops on Zephyr and can sit unconditionally next to
the `ALP_BACKEND_REGISTER` / `ALP_BACKEND_DEFINE_CLASS` they pair with.
The anchor pulls the portable `sw_fallback` (the guaranteed priority-0
catch-all); higher-priority vendor backends still win selection whenever
they are also in the link (always true for the whole-archive shared
`libalp_sdk.so` that is the real Linux/Yocto shipping artifact).

`src/backend.c` carries the same anchor for the `alp_backend_classes`
class table, so a consumer that calls only `alp_backend_count()` /
`ALP_BACKEND_AVAILABLE(...)` (e.g. a diagnostic tool) without pulling
any dispatcher still links.

The regression is guarded by `tests/yocto/registry_static_link.c`, which
links a bare consumer against only the static archive (compiling no
backend TU of its own) — the one test in the suite that exercises the
bare-archive link model #368 is about.

## Selection tiebreaker

When multiple backends match the active silicon, `alp_backend_select()`
resolves ties in three deterministic tiers:

1. **Higher priority wins.** Vendor-specific real backends register at
   priority 100; portable real backends at priority 50; SW fallbacks
   at priority 0.
2. **At equal priority, exact `silicon_ref` match beats `*` wildcard.**
   A backend that names a specific silicon overrides a generic
   catch-all that registered at the same priority.
3. **At equal priority and same match-type, alphabetic on `vendor`.**
   Deterministic regardless of linker section order. Should be rare in
   practice (two backends with the same vendor + class at the same
   priority).

The contract is documented at the top of `src/backend.c` and on
`alp_backend_select()` in `<alp/backend.h>`. The tiebreaker is exercised
by `tests/unit/backend_registry/src/test_registry.c` (one case per
tier) and `tests/unit/backend_registry/src/test_bridge_selection.c`
(end-to-end for the 3 GD32-bridge classes).

## Adding a software fallback

Same pattern, with `silicon_ref = "*"` and `priority = 0`. The file
MUST live at `src/backends/<class>/sw_fallback.c` and MUST carry
`@par Cost:` and `@par Performance:` tags in its top comment block
(enforced by `scripts/check_sw_fallback_tags.py` at CI time).

If the class is reachable from the plain-CMake static build, add the
static-archive anchor pair: `ALP_BACKEND_ANCHOR_DEFINE(<class>)` in the
`sw_fallback.c` (next to `ALP_BACKEND_REGISTER`) and
`ALP_BACKEND_ANCHOR(<class>)` in the dispatcher (next to
`ALP_BACKEND_DEFINE_CLASS`). See *Static-archive anchors* above.

## Adding a vendor extension

Vendor-specific knobs that can't be portable live under
`include/alp/ext/<vendor>/<class>.h`. Naming: `alp_<vendor>_<class>_<verb>`.
Every public extension function MUST carry an `@par Supported silicon:`
Doxygen tag listing the silicon refs it works on (enforced by
`scripts/check_vendor_ext_tags.py` at CI time).

## Stubs

A backend file whose `ops` field is NULL is a stub: `alp_<class>_open`
returns `ALP_ERR_NOT_IMPLEMENTED`. Stub files MUST live at
`src/backends/<class>/<vendor>_stub.c` and MUST carry an
`@par Tracking: github.com/alplabai/alp-sdk/issues/<N>` reference
(enforced by `scripts/check_stub_issues.py` at CI time).

## Three error codes — what they mean to a customer

| Code | Meaning |
|---|---|
| `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` | Silicon physically lacks this block. Use a different SKU or accept the limitation. Paired with `ALP_HAS()` at compile time, or `ALP_BACKEND_AVAILABLE()` at runtime. |
| `ALP_ERR_NOSUPPORT` | Backend exists, rejected the specific config. Adjust your config. |
| `ALP_ERR_NOT_IMPLEMENTED` | Backend is a tracked stub. Consult the linked GitHub issue. |

## Reference spec

`docs/superpowers/specs/2026-05-21-backend-registry-design.md`
