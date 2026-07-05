# AEN401 xHCI core — host-testable logic (TDD)

**Date:** 2026-06-26
**Branch:** `feat/aen401-usb-host` (off `dev`) · **PR:** #268
**Extends:** `2026-06-26-aen401-xhci-usb-host-design.md` (the xHCI driver skeleton).

## Goal

Turn the host-deterministic parts of the xHCI host driver from unvalidated skeleton
into **host-unit-tested** code: factor a pure-C, arch-neutral `xhci_core` unit (TRB
rings, DCBAA/contexts, the init register sequence written against a pointer) and
exercise it with a `native_sim` ztest. The Zephyr driver calls `xhci_core` for that
logic; only true hardware interaction (the event ISR firing, transfer completion, port
enumeration timing) stays `TODO(aen401-bench)`.

Rationale: an xHCI host stack can't be runtime-validated without the controller, but
the **ring management, context construction, and the init register sequence are
deterministic data-structure/pointer logic** — testable on the host with mocked
(RAM-backed) registers. This validates them now instead of shipping them blind
("clean link ≠ runs" applies only to the genuine HW-interaction layer).

## Non-goals

- The event ISR, transfer (doorbell→TRB→event) completion, and port enumeration
  *timing* — these need the live controller (`TODO(aen401-bench)`).
- Any DWC3 `G*`/PHY register *values* that need silicon confirmation.
- Changing the `uhc_api` surface or the backend wiring (done in the prior tasks).

## Architecture

```
uhc_xhci_alif.c  (Zephyr driver: real MMIO base, IRQ 101, DWC3 G* glue, uhc_api, ISR)
      │  calls
      ▼
xhci_core.{c,h}  (pure C, arch-neutral: TRB/ring structs, ring enqueue/cycle/wrap,
                  DCBAA + slot/endpoint context builders, init sequence against a
                  `struct xhci_op_regs *` pointer)
      ▲  exercised by
tests/unit/xhci_core/  (native_sim ztest: RAM-backed registers, no controller)
```

## Components

### C1 — `zephyr/drivers/usb/uhc/xhci_core.{c,h}` (pure-C, host-testable)

Arch-neutral (**`stdint.h` + plain structs + pointer math only — no MMIO `volatile`,
no Zephyr runtime, no Cortex intrinsics**), so it compiles for `native_sim` and the
M55 alike. Contents, grounded from the **public xHCI spec** §5 (registers) / §6 (data
structures):
- `struct xhci_trb` (4×u32: params-lo/hi, status, control incl. the **cycle bit**);
  ring segment + `struct xhci_ring { trb*, size, enqueue_idx, cycle_state }`.
- `xhci_ring_init(ring, seg, n)` and `xhci_ring_enqueue(ring, trb_in)` — advances the
  enqueue pointer, sets the TRB's cycle bit to the producer cycle, and on reaching the
  last (Link) TRB **wraps to segment start and toggles the cycle state** (the xHCI
  producer-cycle invariant).
- `xhci_dcbaa_set(dcbaa, slot, ctx_phys)` + `xhci_build_slot_context()` /
  `xhci_build_ep_context()` — pack the slot/endpoint context fields (route string,
  context entries, EP type, max packet size, the dequeue pointer + DCS) at the spec
  offsets.
- `struct xhci_op_regs` (USBCMD/USBSTS/CRCR/DCBAAP/CONFIG at their spec offsets) +
  `xhci_init_sequence(op, dcbaa_phys, cmd_ring_phys, max_slots)` — the deterministic
  power-on programming (write CONFIG.MaxSlotsEn, DCBAAP, CRCR with RCS=1, set
  USBCMD.R/S) written **through the `op` pointer** so a test can point it at RAM.
  (Actually touching real MMIO + waiting on USBSTS.CNR/HCH is the driver's job, bench.)

### C2 — wire the driver to `xhci_core`

`uhc_xhci_alif.c`: include `xhci_core.h`; in `*_init` call `xhci_init_sequence(...)`
against the real op-regs base (derived from the controller base + `CAPLENGTH`) after
the DWC3 `G*` glue; hold the ring/DCBAA/context state via `xhci_core` structs. The
ISR / transfer-completion / enumeration paths stay `TODO(aen401-bench)` but now call
into `xhci_ring_enqueue` etc. where the data-structure step is deterministic.

### C3 — `tests/unit/xhci_core/` native_sim ztest

Mirror an existing `tests/unit/<name>/` (CMakeLists.txt + prj.conf + src + testcase.yaml).
Compile `xhci_core.c` directly; tests use stack/heap RAM for the rings/registers (no
controller):
- **ring wrap + cycle toggle:** enqueue past the segment end hits the Link TRB →
  enqueue wraps to index 0 and the producer cycle state flips; enqueued TRBs carry the
  correct cycle bit.
- **context packing:** `xhci_build_slot_context` / `_ep_context` set the expected
  fields at the expected offsets (assert specific u32 words).
- **init sequence:** `xhci_init_sequence` against a RAM `struct xhci_op_regs` writes
  the expected CONFIG/DCBAAP/CRCR/USBCMD values (assert the RAM words).

## Validation

- **`twister native_sim` runs `tests/unit/xhci_core` GREEN** (the load-bearing gate;
  real host-validated logic — RED→GREEN per task). This is the no-board proof, stronger
  than "compiles."
- The AEN401 `west build` still links (the driver now calls `xhci_core`).
- clang-format: `xhci_core.*` + the test are under `zephyr/**` / `tests/**`; follow the
  applicable style (`zephyr/**` is excluded from the alp clang-format-22 gate; `tests/**`
  C is gated — format the ztest with clang-format-22).

## Risks

- **xHCI-spec fidelity.** The cycle-bit/Link-TRB and context-offset details must match
  the xHCI spec exactly — the tests encode the spec's expected values, so a wrong
  reading fails the test (the point of TDD here). Cite the spec section per function.
- **native_sim compilability.** `xhci_core` must avoid MMIO `volatile`/intrinsics so it
  builds host-side; the driver supplies the real (volatile) pointer cast at the call
  site.
- **`tests/**` clang-format gate.** The ztest C is gated — keep it clang-format-22 clean.

## Constraints

- xHCI register/struct semantics from the **public xHCI specification** (cite §5/§6);
  base/IRQ already grounded (`0x48200000`/101) but `xhci_core` is base-neutral (pointer).
- Never invent silicon-specific values — DWC3 `G*`/PHY specifics stay `TODO(aen401-bench)`
  in the driver, out of `xhci_core`.
- "Alp Lab"; no `Co-Authored-By: Claude`. No binaries. No confidential HWRM prose/path.
- Branch `feat/aen401-usb-host` → PR #268.
