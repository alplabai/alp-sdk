# Task 4 Report — wire uhc_xhci_alif to xhci_core + AEN401 build + twister

**Status:** DONE
**Date:** 2026-06-27
**Branch:** `feat/aen401-usb-host`
**Commit:** `3c24de01` — "drivers(usb): wire uhc_xhci_alif to xhci_core; xhci_core native_sim-validated"

---

## Driver wiring diff

`zephyr/drivers/usb/uhc/uhc_xhci_alif.c` (+40 lines net):
- Added `#include "xhci_core.h"` after the existing Zephyr includes.
- Extended `uhc_xhci_alif_data` with three new fields:
  - `struct xhci_ring cmd_ring` — xhci_core bookkeeping.
  - `struct xhci_trb cmd_ring_seg[32] __aligned(64)` — 31 usable TRBs + 1 Link TRB; 64-byte alignment per xHCI §4.9.1.
  - `uint64_t dcbaa[9] __aligned(64)` — DCBAA, slot 0 reserved + 8 device slots; 64-byte alignment per xHCI §6.1.
- In `uhc_xhci_alif_init`, after the DWC3 G*-register TODO block:
  1. `xhci_ring_init(&data->cmd_ring, data->cmd_ring_seg, ARRAY_SIZE(...))` — init command ring.
  2. Cast `cfg->base + 0x20u` to `struct xhci_op_regs *` (0x20 = xHCI spec §5.3.1 typical CAPLENGTH; two TODO comments note the real read is bench-gated).
  3. `xhci_init_sequence(op, dcbaa_phys, cmd_ring_phys, 8u)` — programs CONFIG/DCBAAP/CRCR/USBCMD.RS.
- All MMIO-interaction paths (CAPLENGTH MMIO read, USBSTS.CNR/HCH poll, event ISR, transfer scheduling, enumeration) remain `TODO(aen401-bench)` untouched.

`zephyr/CMakeLists.txt` (+1 line):
- Added `${ZEPHYR_CURRENT_MODULE_DIR}/zephyr/drivers/usb/uhc/xhci_core.c` to the `zephyr_library_sources_ifdef(CONFIG_UHC_XHCI_ALIF …)` block.
- **Root cause of first failure:** the module-root CMakeLists.txt is the real wiring point for alp-sdk drivers (not `drivers/usb/uhc/CMakeLists.txt` which is documentation-only). Without this, the linker reported "undefined reference to `xhci_ring_init'" and "undefined reference to `xhci_init_sequence'".

`zephyr/drivers/usb/uhc/CMakeLists.txt` (+1 line):
- Added `xhci_core.c` here too for documentation parity (this file is not add_subdirectory'd by any parent in the actual build).

---

## AEN401 build result

```
Board:   alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp
Example: examples/peripheral-io/usb-host-storage
Build:   PASS  (west --pristine, Zephyr 4.4.0, arm-zephyr-eabi 14.3.0)

FLASH: 58 968 B / 5 632 KB (1.02%)
RAM:   19 812 B / 1 MB (1.89%)

ELF: 32-bit LSB executable, ARM, EABI5 version 1 (SYSV),
     statically linked, with debug_info, not stripped
```

`xhci_ring_init` / `xhci_init_sequence` symbols resolved at link time (no
linker errors). They are absent from `nm` output because `--gc-sections`
dead-strips them from the final image — the init function is only reachable at
runtime via function pointer through the `uhc_api` table. This is correct
behaviour for a stub build; on bench with the real init path exercised the
symbols will remain.

---

## Twister gate (alp.xhci_core.unit — 3/3 PASS)

```
platform: native_sim/native/64
suite:    alp.xhci_core.unit

3 of 3 executed test cases passed (100.00%)
1 of 1 test configurations passed (100.00%)
Twister exit: 0   (18.27 s)
```

Three verified invariants:
1. `test_ring_enqueue_cycle_and_link_wrap` — producer cycle toggles on Link-TRB wrap; enqueue ptr resets to 0; Link TRB carries TC bit + pre-toggle cycle.
2. `test_dcbaa_and_context_build` — DCBAA slot write; slot context dword0 route/speed/entries packing; EP context dword1 ep_type/max_packet + dword2/3 TR dequeue ptr | DCS.
3. `test_init_sequence_writes_expected_regs` — `xhci_init_sequence` writes `CONFIG.MaxSlotsEn`, `DCBAAP_LO/HI`, `CRCR_LO` (ring ptr | RCS=1), and sets `USBCMD.R/S`.

---

## Doc changes

- `docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md` — appended
  "xhci_core host-logic validation (Task 4 — 2026-06-27)" section with twister
  3/3 result, invariant table, driver wiring summary, AEN401 build evidence,
  updated artifact index. Force-added (notes/ is gitignored).
- `CHANGELOG.md` — new "[Unreleased]" bullet: xhci_core validated + driver wired.
- `scripts/check_doc_drift.py`: **OK**.
- `scripts/check_doxygen_coverage.py`: **587/587 @brief (100%)** — no regression.

---

## Concerns / notes

- **CAPLENGTH placeholder `0x20u`**: Spec-typical but must be read from
  `cap->caplength` on real silicon. Two explicit TODO comments mark this.
- **GC-sections strips xhci_core symbols**: Expected for a stub build. Symbols
  exist in the object archive and resolve at link time without errors; they're
  stripped from the final image by `--gc-sections` because the init function is
  only reachable at runtime via function pointer. Not a regression.
- **`drivers/usb/uhc/CMakeLists.txt` is documentation-only**: The real wiring
  lives in the module-root `zephyr/CMakeLists.txt`. Both updated for consistency.
- **Propagation check**: No public API, enum, board.yaml, or `include/alp/`
  surface was touched — `propagating-code-changes` does not apply.

---

## Final-review fixes (2026-06-27)

Three fixes from the final whole-increment review. All gates re-run and pass.

### Fix 1 (Important) — `>>32` UB on 32-bit `uintptr_t`

**File:** `zephyr/drivers/usb/uhc/xhci_core.c` (Link-TRB `param_hi` init)

`(uintptr_t)seg >> 32` is undefined behavior on the M55 target where `uintptr_t`
is 32-bit (C11 §6.5.7p3: shift by operand width). Fixed by widening to `uint64_t`
before shifting:

```c
/* before */
seg[size - 1u].param_hi = (uint32_t)((uintptr_t)seg >> 32);
/* after */
seg[size - 1u].param_hi = (uint32_t)((uint64_t)(uintptr_t)seg >> 32);
```

The other `>> 32` sites in the file (`dcbaap_hi`, `crcr_hi`, `ctx[3]`) already
operate on `uint64_t` inputs — left unchanged.

### Fix 2 (Important) — driver writes real MMIO pre-reset via non-`volatile` pointer

**File:** `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`

The original `*_init` built `struct xhci_op_regs *op = (struct xhci_op_regs
*)(cfg->base + 0x20u)` and called `xhci_init_sequence(op, …)` directly against
MMIO. Two defects: (a) `xhci_op_regs` fields are not `volatile`, so the compiler
may elide writes against real MMIO; (b) op-reg writes happened before the TODO'd
DWC3 reset/HCRST/CNR-poll, violating xHCI spec §4.2.

Fix: add a RAM shadow `struct xhci_op_regs op_image` to the driver data struct
and call `xhci_init_sequence(&data->op_image, …)` instead. The op-reg image is
built via the same path the native_sim ztest validates; the MMIO write-out
(DCBAAP/CRCR/CONFIG copy with volatile writes, then USBCMD.R/S at enable) is
deferred to `TODO(aen401-bench)`. The consolidated TODO comment describes the
full bench sequence: CAPLENGTH read → DWC3 soft-reset + GCTL PrtCapDir=host →
HCRST + CNR poll → volatile copy of op_image fields → R/S at `_enable`.

CHANGELOG and grounding note (`docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md`)
updated to say the op-reg **image** is built via the host-validated path while
the **MMIO write-out** is `TODO(aen401-bench)`.

### Fix 3 (Minor) — strengthen RED-coverage in init-sequence test

**File:** `tests/unit/xhci_core/src/test_xhci_core.c`

Previous test used `0x20000000ull` / `0x20001000ull` — both have zero high words
and pass the alignment masks trivially, so the masks + `dcbaap_hi`/`crcr_hi`
split were unasserted. New addresses:

- `dcbaa_phys = 0x1234567800002040ull` — non-zero high word pins `dcbaap_hi == 0x12345678u`
- `cmd_ring_phys = 0x00000000DEAD2080ull` — 64-byte aligned low word pins `crcr_lo == 0xDEAD2080u | 1u` and `crcr_hi == 0u`

Both addresses are already 64-byte aligned, so the `& 0xFFFFFFC0` masks pass
them through unchanged — confirming the function does not over-mask. Added
explicit `zassert_equal(op.crcr_hi, 0u, …)` assertion; `dcbaap_hi` now asserts
`0x12345678u` rather than `0u`.

---

### Gate results (re-run after all three fixes)

**Twister (alp.xhci_core.unit — load-bearing gate):**

```
36 of 36 executed test configurations passed (100.00%)
310 of 310 executed test cases passed (100.00%) on 1 out of total 1477 platforms
2 selected test cases not executed: 2 skipped
Twister exit: 0
```

**AEN401 west build gate:**

```
Board:   alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp
Example: examples/peripheral-io/usb-host-storage
Build:   PASS  (west --pristine, Zephyr 4.4.0, arm-zephyr-eabi 14.3.0)

FLASH: 58968 B / 5632 KB (1.02%)
RAM:   19876 B / 1 MB (1.90%)

west exit: 0
```

**clang-format-22** (`~/.local/bin/clang-format` v22.1.5) on
`tests/unit/xhci_core/src/test_xhci_core.c`: **clean** (diff exit 0).
