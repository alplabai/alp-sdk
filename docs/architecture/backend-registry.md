# Backend Registry Architecture

The ALP SDK dispatches every peripheral call through a linker-section
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
   class's internal header — e.g. `src/zephyr/peripheral_adc_ops.h`).
3. Register it with `ALP_BACKEND_REGISTER(<class>, <name>, { … });`.
4. Declare the class table entry with `ALP_BACKEND_DEFINE_CLASS(<class>)`
   in the class dispatcher (only the dispatcher file does this, once).
5. Add the new file to the appropriate Kconfig / CMake list so it
   builds for the right SoM target.

That's it. The registry walker at `alp_backend_select()` picks up the
new entry automatically.

## Adding a software fallback

Same pattern, with `silicon_ref = "*"` and `priority = 0`. The file
MUST live at `src/backends/<class>/sw_fallback.c` and MUST carry
`@par Cost:` and `@par Performance:` tags in its top comment block
(enforced by `scripts/check_sw_fallback_tags.py` at CI time).

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
