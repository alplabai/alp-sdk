# AEN401 xHCI Core (host-testable, TDD) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Factor the host-deterministic xHCI logic (TRB rings, DCBAA/contexts, the init register sequence) into a pure-C `xhci_core` unit and validate it with a `native_sim` ztest — turning unvalidated skeleton into host-tested code.

**Architecture:** `xhci_core.{c,h}` (pure C, arch-neutral — stdint + structs + pointer math, no MMIO `volatile`/Zephyr/Cortex) holds ring/context/init-sequence logic, written from the public xHCI spec §5/§6. A `tests/unit/xhci_core/` native_sim ztest compiles `xhci_core.c` directly and exercises it with RAM-backed registers (no controller). The Zephyr driver `uhc_xhci_alif.c` calls `xhci_core`; live-HW paths (ISR, transfer completion, enumeration timing) stay `TODO(aen401-bench)`.

**Tech Stack:** C (C11, freestanding-friendly), Zephyr ztest on `native_sim`, twister, the public xHCI specification.

## Global Constraints

- xHCI register/struct semantics come from the **public xHCI specification** (cite §5/§6 per function). Base/IRQ already grounded (`0x48200000`/101) but `xhci_core` is **base-neutral** — it takes pointers.
- `xhci_core.{c,h}` must be **arch-neutral**: `#include <stdint.h>`/`<stddef.h>`/`<string.h>` only — NO MMIO `volatile`, NO `<zephyr/*>`, NO Cortex intrinsics — so it compiles on `native_sim` and the M55 alike. The driver supplies the real (volatile) base cast at the call site.
- Files under `zephyr/**` are EXCLUDED from the clang-format-22 gate; **`tests/**` C IS gated** — clang-format-22 the ztest.
- TDD: write the failing ztest case FIRST, run it RED, implement minimally, run GREEN.
- Validation gate = `twister` on `native_sim`. Local invocation per the repo's `reference_local_twister_invocation` (WSL host toolchain, `EXTRA_ZEPHYR_MODULES`); drive via a `.sh` through `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../script.sh`.
- Never invent silicon-specific values — DWC3 `G*`/PHY specifics stay `TODO(aen401-bench)` in the driver, OUT of `xhci_core`.
- "Alp Lab"; no `Co-Authored-By: Claude`; no binaries; no confidential HWRM prose/path. Branch `feat/aen401-usb-host` → PR #268.
- xHCI bit constants (public spec): TRB control cycle bit = bit 0 (`1u<<0`); TRB type = bits 15:10 (`type<<10`); Link TRB type = 6; Link Toggle-Cycle (TC) = bit 1 (`1u<<1`). Op regs (offset from op base): `USBCMD`=0x00 (R/S=bit0), `USBSTS`=0x04 (HCH=bit0), `CRCR`=0x18 (RCS=bit0), `DCBAAP`=0x30, `CONFIG`=0x38 (MaxSlotsEn=bits7:0).

---

### Task 1: `xhci_core` ring — TRB ring enqueue + cycle-bit + Link-TRB wrap (TDD)

**Files:**
- Create: `zephyr/drivers/usb/uhc/xhci_core.h`, `zephyr/drivers/usb/uhc/xhci_core.c`
- Create: `tests/unit/xhci_core/CMakeLists.txt`, `prj.conf`, `testcase.yaml`, `src/test_xhci_core.c`

**Interfaces:**
- Produces: `struct xhci_trb`, `struct xhci_ring`, `xhci_ring_init(ring, seg, n)`, `xhci_ring_enqueue(ring, trb_in)`; macros `XHCI_TRB_CYCLE`, `XHCI_TRB_TYPE(t)`, `XHCI_TRB_TYPE_LINK`, `XHCI_TRB_LINK_TC`.

- [ ] **Step 1: Create the test harness + the failing ring test**

`tests/unit/xhci_core/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_xhci_core_test LANGUAGES C)
# xhci_core.c is pure C; compile it directly into the test (no DT/MMIO needed).
target_sources(app PRIVATE
  src/test_xhci_core.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/usb/uhc/xhci_core.c)
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/drivers/usb/uhc)
```
`prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```
`testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.xhci_core.unit:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp-sdk
      - usb
      - xhci
      - unit
```
`src/test_xhci_core.c`:
```c
/*
 * SPDX-License-Identifier: Apache-2.0
 * Host unit tests for xhci_core (rings/contexts/init) -- native_sim, no controller.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include "xhci_core.h"

ZTEST_SUITE(alp_xhci_core, NULL, NULL, NULL, NULL, NULL);

/* A ring of N TRBs: N-1 usable + 1 Link TRB. Enqueuing N-1 entries fills it to
 * the Link TRB; the next enqueue wraps to index 0 and toggles the producer cycle. */
ZTEST(alp_xhci_core, test_ring_enqueue_cycle_and_link_wrap)
{
	struct xhci_trb seg[4];
	struct xhci_ring ring;

	xhci_ring_init(&ring, seg, 4);
	zassert_equal(ring.cycle, 1, "initial producer cycle is 1");
	zassert_equal(ring.enqueue, 0u, "starts at index 0");

	struct xhci_trb in = { .param_lo = 0xAA, .control = 0 };

	/* 3 usable slots (indices 0,1,2); index 3 is the Link TRB. */
	xhci_ring_enqueue(&ring, &in);
	zassert_true((seg[0].control & XHCI_TRB_CYCLE) != 0, "TRB0 carries producer cycle=1");
	xhci_ring_enqueue(&ring, &in);
	xhci_ring_enqueue(&ring, &in);              /* this fills to the Link TRB -> wrap */

	zassert_equal(ring.enqueue, 0u, "wrapped back to index 0");
	zassert_equal(ring.cycle, 0, "producer cycle toggled after the Link TRB");
	/* The Link TRB (seg[3]) must be a Link type with TC set, cycle = pre-toggle (1). */
	zassert_equal(XHCI_TRB_GET_TYPE(seg[3].control), XHCI_TRB_TYPE_LINK, "seg[3] is a Link TRB");
	zassert_true((seg[3].control & XHCI_TRB_LINK_TC) != 0, "Link TRB has Toggle-Cycle set");
	zassert_true((seg[3].control & XHCI_TRB_CYCLE) != 0, "Link TRB cycle was the producer cycle (1)");
}
```

- [ ] **Step 2: Run it RED**

Write `scratchpad/twister_xhci.sh` (per `reference_local_twister_invocation`: WSL host toolchain, `EXTRA_ZEPHYR_MODULES=<repo>`, run twister on `tests/unit/xhci_core`). Run via `MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/twister_xhci.sh`.
Expected: FAIL — `xhci_core.h` not found / undefined symbols.

- [ ] **Step 3: Write `xhci_core.h` (structs + ring API + macros) — minimal**

```c
/* Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure-C, arch-neutral xHCI host logic (rings/contexts/init sequence) per the
 * public xHCI specification.  No MMIO/Zephyr/Cortex deps -- host-unit-testable.
 */
#ifndef ALP_XHCI_CORE_H
#define ALP_XHCI_CORE_H
#include <stdint.h>
#include <stddef.h>

/* xHCI Transfer Request Block (spec §6.4): 4 x 32-bit. */
struct xhci_trb {
	uint32_t param_lo;
	uint32_t param_hi;
	uint32_t status;
	uint32_t control;
};

/* TRB control-word fields (spec §6.4.4). */
#define XHCI_TRB_CYCLE        (1u << 0)
#define XHCI_TRB_LINK_TC      (1u << 1)        /* Link TRB Toggle Cycle */
#define XHCI_TRB_TYPE(t)      ((uint32_t)(t) << 10)
#define XHCI_TRB_GET_TYPE(c)  (((c) >> 10) & 0x3Fu)
#define XHCI_TRB_TYPE_LINK    6u               /* spec Table 6-91 */

/* A producer ring: `size` TRBs, the last reserved as a Link TRB. */
struct xhci_ring {
	struct xhci_trb *seg;
	uint32_t         size;     /* total TRBs incl. the Link TRB */
	uint32_t         enqueue;  /* producer index */
	int              cycle;    /* producer cycle state (0/1) */
};

void xhci_ring_init(struct xhci_ring *ring, struct xhci_trb *seg, uint32_t size);
void xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *in);

#endif /* ALP_XHCI_CORE_H */
```

- [ ] **Step 4: Write `xhci_core.c` ring impl — minimal to pass**

```c
/* Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "xhci_core.h"

void xhci_ring_init(struct xhci_ring *ring, struct xhci_trb *seg, uint32_t size)
{
	memset(seg, 0, (size_t)size * sizeof(*seg));
	ring->seg = seg;
	ring->size = size;
	ring->enqueue = 0u;
	ring->cycle = 1;
	/* Last TRB is a Link TRB pointing back to seg[0] (spec §4.11.5.1). Its
	 * cycle bit is set to the producer cycle as the ring crosses it. */
	seg[size - 1u].param_lo = (uint32_t)(uintptr_t)seg;
	seg[size - 1u].param_hi = 0u;
	seg[size - 1u].control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_LINK) | XHCI_TRB_LINK_TC;
}

void xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *in)
{
	struct xhci_trb *slot = &ring->seg[ring->enqueue];

	slot->param_lo = in->param_lo;
	slot->param_hi = in->param_hi;
	slot->status = in->status;
	/* Producer owns the slot by setting its cycle bit to the producer cycle. */
	slot->control = (in->control & ~XHCI_TRB_CYCLE) |
			(ring->cycle ? XHCI_TRB_CYCLE : 0u);

	ring->enqueue++;
	if (ring->enqueue == ring->size - 1u) {
		/* Reached the Link TRB: stamp its cycle with the producer cycle,
		 * then toggle and wrap (spec §4.11.5.1). */
		struct xhci_trb *link = &ring->seg[ring->size - 1u];

		link->control = (link->control & ~XHCI_TRB_CYCLE) |
				(ring->cycle ? XHCI_TRB_CYCLE : 0u);
		ring->cycle ^= 1;
		ring->enqueue = 0u;
	}
}
```

- [ ] **Step 5: Run it GREEN**

Run the twister script. Expected: `alp.xhci_core.unit` PASS (1 case).

- [ ] **Step 6: clang-format-22 the test (tests/** is gated) + commit**

```
MSYS_NO_PATHCONV=1 wsl bash -lc "cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && ~/.local/bin/clang-format -i tests/unit/xhci_core/src/test_xhci_core.c && ~/.local/bin/clang-format --dry-run --Werror tests/unit/xhci_core/src/test_xhci_core.c && echo CLEAN"
git add zephyr/drivers/usb/uhc/xhci_core.h zephyr/drivers/usb/uhc/xhci_core.c tests/unit/xhci_core/
git commit -m "drivers(usb): xhci_core ring (enqueue/cycle/Link-wrap) + native_sim ztest"
```

---

### Task 2: `xhci_core` DCBAA + slot/EP context builders (TDD)

**Files:**
- Modify: `zephyr/drivers/usb/uhc/xhci_core.{c,h}`, `tests/unit/xhci_core/src/test_xhci_core.c`

**Interfaces:**
- Consumes: Task 1's header.
- Produces: `xhci_dcbaa_set(uint64_t *dcbaa, uint32_t slot, uint64_t ctx_phys)`; `xhci_build_slot_context(uint32_t *ctx, uint32_t route_string, uint32_t speed, uint32_t ctx_entries)`; `xhci_build_ep_context(uint32_t *ctx, uint32_t ep_type, uint32_t max_packet, uint64_t tr_dequeue_phys, int dcs)`.

- [ ] **Step 1: Add the failing context test**

Append to `test_xhci_core.c`:
```c
ZTEST(alp_xhci_core, test_dcbaa_and_context_build)
{
	uint64_t dcbaa[8] = {0};
	xhci_dcbaa_set(dcbaa, 1, 0x12340000ull);
	zassert_equal(dcbaa[1], 0x12340000ull, "DCBAA slot 1 holds the context phys");

	/* Slot context dword0 (spec §6.2.2): route(19:0) | speed(23:20) | ctx_entries(31:27). */
	uint32_t sc[8] = {0};
	xhci_build_slot_context(sc, 0x5u /*route*/, 3u /*HS*/, 1u /*entries*/);
	zassert_equal(sc[0] & 0xFFFFFu, 0x5u, "route string");
	zassert_equal((sc[0] >> 20) & 0xFu, 3u, "speed");
	zassert_equal((sc[0] >> 27) & 0x1Fu, 1u, "context entries");

	/* EP context (spec §6.2.3): dword1 ep_type(5:3) | max_packet(31:16);
	 * dword2/3 = TR dequeue ptr | DCS(bit0). */
	uint32_t ep[8] = {0};
	xhci_build_ep_context(ep, 4u /*Control? use Bulk-IN=6*/, 64u, 0xCAFE0000ull, 1);
	zassert_equal((ep[1] >> 3) & 0x7u, 4u, "ep type");
	zassert_equal((ep[1] >> 16) & 0xFFFFu, 64u, "max packet size");
	zassert_equal(ep[2] & 0x1u, 1u, "dequeue cycle state (DCS)");
	zassert_equal(ep[2] & ~0xFu, (uint32_t)(0xCAFE0000ull & ~0xFull), "TR dequeue ptr lo");
}
```

- [ ] **Step 2: Run RED** (twister) — FAIL (undefined builders).

- [ ] **Step 3: Implement the builders in `xhci_core.c` + declare in `.h`**

```c
void xhci_dcbaa_set(uint64_t *dcbaa, uint32_t slot, uint64_t ctx_phys)
{
	dcbaa[slot] = ctx_phys;
}

void xhci_build_slot_context(uint32_t *ctx, uint32_t route_string,
			     uint32_t speed, uint32_t ctx_entries)
{
	/* §6.2.2 dword0: RouteString[19:0], Speed[23:20], ContextEntries[31:27]. */
	ctx[0] = (route_string & 0xFFFFFu) |
		 ((speed & 0xFu) << 20) |
		 ((ctx_entries & 0x1Fu) << 27);
}

void xhci_build_ep_context(uint32_t *ctx, uint32_t ep_type, uint32_t max_packet,
			   uint64_t tr_dequeue_phys, int dcs)
{
	/* §6.2.3 dword1: EPType[5:3], MaxPacketSize[31:16]. */
	ctx[1] = ((ep_type & 0x7u) << 3) | ((max_packet & 0xFFFFu) << 16);
	/* dword2/3: TR Dequeue Pointer (16-byte aligned) | DCS[bit0]. */
	ctx[2] = ((uint32_t)(tr_dequeue_phys & 0xFFFFFFF0u)) | (dcs ? 1u : 0u);
	ctx[3] = (uint32_t)(tr_dequeue_phys >> 32);
}
```
Add the three prototypes to `xhci_core.h`.

- [ ] **Step 4: Run GREEN** (twister) — both suites PASS.

- [ ] **Step 5: clang-format-22 the test + commit**

```bash
git add zephyr/drivers/usb/uhc/xhci_core.{c,h} tests/unit/xhci_core/src/test_xhci_core.c
git commit -m "drivers(usb): xhci_core DCBAA + slot/EP context builders + tests"
```

---

### Task 3: `xhci_core` init register sequence (mocked MMIO, TDD)

**Files:**
- Modify: `zephyr/drivers/usb/uhc/xhci_core.{c,h}`, `tests/unit/xhci_core/src/test_xhci_core.c`

**Interfaces:**
- Produces: `struct xhci_op_regs` (USBCMD/USBSTS/CRCR/DCBAAP/CONFIG at spec offsets) + `xhci_init_sequence(struct xhci_op_regs *op, uint64_t dcbaa_phys, uint64_t cmd_ring_phys, uint32_t max_slots)`.

- [ ] **Step 1: Add the failing init-sequence test**

```c
ZTEST(alp_xhci_core, test_init_sequence_writes_expected_regs)
{
	struct xhci_op_regs op;
	memset(&op, 0, sizeof(op));

	xhci_init_sequence(&op, 0x20000000ull /*dcbaa*/, 0x20001000ull /*cmd ring*/, 8u);

	zassert_equal(op.config & 0xFFu, 8u, "CONFIG.MaxSlotsEn = 8");
	zassert_equal(op.dcbaap_lo, 0x20000000u, "DCBAAP low");
	zassert_equal(op.dcbaap_hi, 0u, "DCBAAP high");
	/* CRCR low = cmd_ring_phys (64-byte aligned) | RCS(bit0)=1. */
	zassert_equal(op.crcr_lo, 0x20001000u | 1u, "CRCR low with RCS=1");
	zassert_true((op.usbcmd & 1u) != 0, "USBCMD.R/S set");
}
```

- [ ] **Step 2: Run RED** — FAIL (no `xhci_op_regs`/`xhci_init_sequence`).

- [ ] **Step 3: Implement (`.h` struct + `.c` sequence)**

In `xhci_core.h` (a plain struct mirroring the spec op-register layout — the DRIVER casts the real MMIO base to this; the TEST uses a RAM instance):
```c
/* xHCI operational registers (spec §5.4), offsets from the op base
 * (= CAP base + CAPLENGTH).  Split 64-bit regs into lo/hi for portability. */
struct xhci_op_regs {
	uint32_t usbcmd;    /* 0x00 */
	uint32_t usbsts;    /* 0x04 */
	uint32_t pagesize;  /* 0x08 */
	uint32_t rsvd0[2];
	uint32_t dnctrl;    /* 0x14 */
	uint32_t crcr_lo;   /* 0x18 */
	uint32_t crcr_hi;   /* 0x1C */
	uint32_t rsvd1[4];
	uint32_t dcbaap_lo; /* 0x30 */
	uint32_t dcbaap_hi; /* 0x34 */
	uint32_t config;    /* 0x38 */
};
#define XHCI_USBCMD_RS   (1u << 0)
#define XHCI_CRCR_RCS    (1u << 0)

void xhci_init_sequence(struct xhci_op_regs *op, uint64_t dcbaa_phys,
			uint64_t cmd_ring_phys, uint32_t max_slots);
```
In `xhci_core.c`:
```c
void xhci_init_sequence(struct xhci_op_regs *op, uint64_t dcbaa_phys,
			uint64_t cmd_ring_phys, uint32_t max_slots)
{
	/* spec §4.2 init: program MaxSlotsEn, DCBAAP, CRCR (RCS=1), then run. */
	op->config = (op->config & ~0xFFu) | (max_slots & 0xFFu);
	op->dcbaap_lo = (uint32_t)(dcbaa_phys & 0xFFFFFFC0u);  /* 64-byte aligned */
	op->dcbaap_hi = (uint32_t)(dcbaa_phys >> 32);
	op->crcr_lo = (uint32_t)(cmd_ring_phys & 0xFFFFFFC0u) | XHCI_CRCR_RCS;
	op->crcr_hi = (uint32_t)(cmd_ring_phys >> 32);
	op->usbcmd |= XHCI_USBCMD_RS;
}
```

- [ ] **Step 4: Run GREEN** (twister — all 3 suites PASS).

- [ ] **Step 5: clang-format-22 the test + commit**

```bash
git add zephyr/drivers/usb/uhc/xhci_core.{c,h} tests/unit/xhci_core/src/test_xhci_core.c
git commit -m "drivers(usb): xhci_core init register sequence (mocked-MMIO tested)"
```

---

### Task 4: wire the driver to `xhci_core` + AEN401 build + final twister

**Files:**
- Modify: `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`
- Reference: `tests/unit/xhci_core/` (Tasks 1-3)

**Interfaces:**
- Consumes: `xhci_core.h` (rings/contexts/init sequence).

- [ ] **Step 1: Call `xhci_core` from the driver**

In `uhc_xhci_alif.c`: `#include "xhci_core.h"`. In `*_init`, after the DWC3 `G*` glue (which stays `TODO(aen401-bench)`), compute the op-regs base (`cfg->base + CAPLENGTH`, cast to `struct xhci_op_regs *`) and call `xhci_init_sequence(...)` with the DCBAA/command-ring physical addresses. Hold an `xhci_ring` for the command ring via `xhci_ring_init`. The replaced inline register-poking TODO becomes a call into `xhci_core` where deterministic; the actual MMIO read of `CAPLENGTH` + the post-init `USBSTS.CNR`/`HCH` waits stay `TODO(aen401-bench)`. Keep all genuinely-HW paths (ISR, transfer completion, enumeration) `TODO(aen401-bench)`.

- [ ] **Step 2: Build for AEN401 (driver still links with xhci_core)**

`west build -b alp_e1m_aen401_m55_hp examples/peripheral-io/usb-host-storage -p always` (via the WSL `.sh`). Expected: M55 ELF, `xhci_core` symbols linked into the driver.

- [ ] **Step 3: Full twister run on the unit test**

`MSYS_NO_PATHCONV=1 wsl bash /mnt/c/.../scratchpad/twister_xhci.sh` — `alp.xhci_core.unit` PASS (all 3 ZTEST cases). This is the load-bearing no-board gate.

- [ ] **Step 4: Commit + docs**

Append the xhci_core validation (twister green, the 3 tested invariants) to `docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md`. CHANGELOG line (xhci_core host-tested logic landed). Run `py -3.14 scripts/check_doc_drift.py`.
```bash
git add zephyr/drivers/usb/uhc/uhc_xhci_alif.c CHANGELOG.md docs/superpowers/notes/2026-06-26-aen401-xhci-usb-grounding.md
git commit -m "drivers(usb): wire uhc_xhci_alif to xhci_core; xhci_core native_sim-validated"
```

---

## Self-Review

**Spec coverage:**
- C1 (xhci_core unit: rings → Task 1; DCBAA/contexts → Task 2; init sequence → Task 3). ✅
- C2 (driver calls xhci_core) → Task 4. ✅
- C3 (native_sim ztest) → Tasks 1-3 build it incrementally; Task 4 runs the full green gate. ✅
- Validation (twister native_sim GREEN) → Task 3 Step 4 + Task 4 Step 3. ✅
- Arch-neutral xhci_core (no MMIO/Zephyr/Cortex) → Global Constraints + the `.h` (stdint only). ✅

**Placeholder scan:** No "TBD"/"implement later". The `TODO(aen401-bench)` references are the spec's HW-interaction boundary (ISR/transfer/enumeration/CAPLENGTH-read), left in the DRIVER, never in `xhci_core` (which is fully implemented + tested). Every code step shows complete code; every test step shows the assertions.

**Type/name consistency:** `struct xhci_trb`/`xhci_ring`/`xhci_op_regs`, `xhci_ring_init`/`xhci_ring_enqueue`/`xhci_dcbaa_set`/`xhci_build_slot_context`/`xhci_build_ep_context`/`xhci_init_sequence`, and the macros `XHCI_TRB_CYCLE`/`XHCI_TRB_LINK_TC`/`XHCI_TRB_TYPE(_LINK)`/`XHCI_TRB_GET_TYPE`/`XHCI_USBCMD_RS`/`XHCI_CRCR_RCS` are used identically in the header, impl, and tests across Tasks 1-4. The test harness paths (`../../../zephyr/drivers/usb/uhc/xhci_core.c`) match the file locations.
