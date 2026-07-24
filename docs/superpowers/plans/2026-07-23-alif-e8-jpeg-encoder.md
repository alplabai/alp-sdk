# Alif E8 JPEG Encoder Surface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a portable `<alp/jpeg.h>` JPEG-**encoder** surface to alp-sdk — hardware-accelerated on the E1M-AEN801 (Alif Ensemble E8 Hantro VC9000E) via the vendored Zephyr driver, with a software baseline-JPEG fallback on every other SoM.

**Architecture:** Follows the established portable-peripheral pattern (camera/i2c precedent): public header → class dispatcher (`ALP_BACKEND_DEFINE_CLASS`) → per-silicon backends chosen at runtime by `alp_backend_select(class, ALP_SOC_REF_STR)`. The Alif backend wraps the Apache-2.0 Zephyr video driver `jpeg_hantro_vc9000e.c` (vendored from `alifsemi/zephyr_alif`, exactly as `video_alif.c`/`isp_pico.c` already were) which links hal_alif's prebuilt SW-helper blob. The SW fallback vendors a compact public-domain baseline encoder.

**Tech Stack:** C (C11), Zephyr `<zephyr/drivers/video.h>` device API, alp backend-registry, hal_alif west module (`modules/hal/alif`, Apache-2.0), Zephyr `ztest` + native_sim, clang-format-22 house style.

## Global Constraints

- **Encode only.** The E8 has no hardware JPEG decoder (datasheet §3.19.4). No decode API is added.
- **No DFP source in this repo.** The Alif CMSIS DFP (`alif_ensemble-cmsis-dfp`) is under the restrictive Alif Software License Agreement (clause 4 field-of-use, clause 5 anti-copyleft) — incompatible with the public repo. Only Apache-2.0 zephyr_alif driver sources + the hal_alif blob (consumed as a west module) may be used.
- **House style:** clang-format-22, tabs, Consecutive alignment, BinPack off (`applying-the-alp-sdk-c-house-style`). Every public symbol gets Doxygen `@brief`/`@param`/`@return`.
- **Copyright header:** `Copyright 2026 Alp Lab AB` + `SPDX-License-Identifier: Apache-2.0` on every new SDK file. Vendored files keep their original `Copyright (c) 2026 Alif Semiconductor` + Apache-2.0 header plus a provenance comment.
- **Silicon values verbatim:** JPEG Encoder base `0x4904_4000`, PD-6, 128-bit AXI; IRQs (M55-HP/M55-HE, Level) `JPEG_XINT_IRQ` 360, `JPEG_XINT_NORM_IRQ` 361, `JPEG_XINT_ABN_IRQ` 362, `JPEG_IDLE_IRQ` 363; `silicon_ref="alif:ensemble:e8"`.
- **Error/status idiom:** ops return `alp_status_t`; values live in **`include/alp/peripheral.h`** (there is NO `include/alp/error.h` — an earlier draft of this plan said so and was wrong) — `ALP_OK`, `ALP_ERR_INVAL`, `ALP_ERR_NOSUPPORT`, `ALP_ERR_NOMEM`, `ALP_ERR_IO`, `ALP_ERR_NOT_READY`, `ALP_ERR_TIMEOUT`, `ALP_ERR_NOT_IMPLEMENTED`, `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`.
- **Backend `.vendor` convention:** class **stubs** register `.vendor = "stub"` (matching `src/backends/camera/zephyr_stub.c`); real backends use their own vendor name (`sw_baseline` → `"alp"`, `alif_hantro` → `"alif"`). Settled 2026-07-24.
- **Gate coupling — any change to `metadata/catalog.json` `portable_api` drags three more files.** Adding public symbols to the catalog makes `check_plain_cmake_link_complete.py` require the class be linked into **`src/yocto/CMakeLists.txt` and `src/baremetal/CMakeLists.txt`**, and adding a `src/common/stub/stub_<class>.c` makes `check_stub_symbol_matrix.py` require a regenerated golden at **`tests/fixtures/stub-symbol-matrix/symbols.json`** (`python3 scripts/check_stub_symbol_matrix.py --update`, review, commit). Both gates run in `.github/workflows/pr-metadata-validate.yml`. Neither runs natively on Windows (no gcc) — run them under WSL.
- **Every new `src/backends/<class>/zephyr_stub.c` needs a `@par Tracking: github.com/alplabai/alp-sdk/issues/<N>` tag** in its file banner or `check_stub_issues.py` fails. For this work the issue is **#898** (`jpeg: wire Alif E8 Hantro JPEG hardware backend`).
- **Branch:** all work on `feat/aen-jpeg-encoder` (already created). Frequent commits, one per task-step group.
- **No Claude/AI attribution** in commits or PR body; attribute to alpCaner.

---

## File Structure

Created:
- `include/alp/jpeg.h` — public API (`alp_jpeg_open/encode/capabilities/close`), `[ABI-EXPERIMENTAL]`.
- `src/jpeg_dispatch.c` — class dispatcher + handle pool + slot-claim guards.
- `src/backends/jpeg/jpeg_ops.h` — internal vtable + `struct alp_jpeg` handle layout.
- `src/backends/jpeg/zephyr_stub.c` — `silicon_ref="*"` prio 0, every op `ALP_ERR_NOT_IMPLEMENTED`.
- `src/backends/jpeg/sw_baseline.c` — `silicon_ref="*"` prio 50, compact baseline encoder adapter.
- `src/backends/jpeg/vendor/toojpeg_baseline.{c,h}` — vendored public-domain baseline encoder.
- `src/backends/jpeg/alif_hantro.c` — `silicon_ref="alif:ensemble:e8"` prio 100, wraps the Zephyr video driver.
- `src/common/stub/stub_jpeg.c` — native_sim host stub registration.
- `zephyr/drivers/video/jpeg_hantro_vc9000e.c` + `jpeg_hantro_vc9000e_regs.h` — vendored Apache-2.0 driver.
- `zephyr/dts/bindings/video/verisilicon,hantro-vc9000e-jpeg.yaml` — vendored DT binding.
- `zephyr/kconfigs/jpeg.kconfig` — `CONFIG_ALP_SDK_JPEG_*` symbols.
- `tests/unit/jpeg_registry/` — backend-selection + SW-encode ztest (native_sim).
- `examples/aen/aen-jpeg-regcheck/` — AEN801 bring-up reg-check example.

Modified:
- `zephyr/CMakeLists.txt` — driver sources + `zephyr_library_sources_ifdef` for the three backends + dispatcher.
- `metadata/catalog.json` — add the `jpeg` class entry.
- `docs/abi-markers.md` — `[ABI-EXPERIMENTAL]` entry for `<alp/jpeg.h>`.

---

### Task 1: Public surface + dispatcher + stub (native_sim registry test)

Delivers a compilable, selectable `jpeg` class whose only backend is the NOT_IMPLEMENTED stub, proven by a native_sim registry test. No hardware, no encode yet.

**Files:**
- Create: `include/alp/jpeg.h`
- Create: `src/backends/jpeg/jpeg_ops.h`
- Create: `src/jpeg_dispatch.c`
- Create: `src/backends/jpeg/zephyr_stub.c`
- Create: `src/common/stub/stub_jpeg.c`
- Modify: `zephyr/CMakeLists.txt` (add dispatcher + stub to the always-built set; mirror the camera lines ~87–90)
- Modify: `metadata/catalog.json` (add `jpeg` class block, mirror the camera entry)
- Modify: `docs/abi-markers.md`
- Test: `tests/unit/jpeg_registry/` (`CMakeLists.txt`, `prj.conf`, `src/test_jpeg_registry.c`, `testcase.yaml`)

**Interfaces:**
- Produces (public, from `include/alp/jpeg.h`):
  ```c
  typedef struct alp_jpeg alp_jpeg_t;

  typedef enum {
      ALP_JPEG_SUBSAMPLE_400 = 0,   /* monochrome / Y-only */
      ALP_JPEG_SUBSAMPLE_420 = 1,   /* 4:2:0 */
      ALP_JPEG_SUBSAMPLE_422 = 2,   /* 4:2:2 */
  } alp_jpeg_subsample_t;

  typedef struct {
      uint32_t engine_id;           /* 0 = default JPEG engine */
      uint16_t max_width;           /* expected max frame width; 0 = backend default */
      uint16_t max_height;
  } alp_jpeg_config_t;

  #define ALP_JPEG_CONFIG_DEFAULT \
      ((alp_jpeg_config_t){ .engine_id = 0u, .max_width = 0u, .max_height = 0u })

  typedef struct {
      uint16_t             width;   /* 8..16384 */
      uint16_t             height;
      alp_jpeg_subsample_t subsample;
      uint8_t              quality; /* 1..100 */
      const void          *y_plane; uint32_t y_stride;
      const void          *u_plane; uint32_t u_stride;   /* NULL when _400 */
      const void          *v_plane; uint32_t v_stride;   /* NULL when _400 */
  } alp_jpeg_encode_req_t;

  typedef struct {
      bool     hw_accelerated;
      bool     mjpeg_supported;
      uint16_t max_width;
      uint16_t max_height;
      uint32_t subsample_mask;      /* bit i set => (1u<<ALP_JPEG_SUBSAMPLE_x) supported */
  } alp_jpeg_caps_t;

  alp_jpeg_t  *alp_jpeg_open(const alp_jpeg_config_t *cfg);
  alp_status_t alp_jpeg_encode(alp_jpeg_t *h, const alp_jpeg_encode_req_t *req,
                               void *out_buf, size_t out_cap, size_t *out_len);
  alp_status_t alp_jpeg_capabilities(const alp_jpeg_t *h, alp_jpeg_caps_t *out);
  void         alp_jpeg_close(alp_jpeg_t *h);
  ```
- Produces (internal, from `src/backends/jpeg/jpeg_ops.h`):
  ```c
  typedef struct alp_jpeg_ops alp_jpeg_ops_t;

  typedef struct alp_jpeg_backend_state {
      void                 *be_data;
      const alp_jpeg_ops_t *ops;
  } alp_jpeg_backend_state_t;

  struct alp_jpeg_ops {
      alp_status_t (*open)(const alp_jpeg_config_t *cfg,
                           alp_jpeg_backend_state_t *state,
                           alp_jpeg_caps_t *caps_out);
      alp_status_t (*encode)(alp_jpeg_backend_state_t *state,
                             const alp_jpeg_encode_req_t *req,
                             void *out_buf, size_t out_cap, size_t *out_len);
      void (*close)(alp_jpeg_backend_state_t *state);
  };

  struct alp_jpeg {
      alp_jpeg_backend_state_t state;
      const alp_backend_t     *backend;
      alp_jpeg_caps_t          cached_caps;
      uint8_t                  lifecycle;
      uint32_t                 active_ops;
      bool                     in_use;   /* MUST be the last member (slot-claim zeroing) */
  };
  ```

- [ ] **Step 1: Confirm the error enum + capability-flags idioms**

Read `include/alp/error.h` (confirm the `ALP_ERR_*` spellings in Global Constraints) and `include/alp/cap_instance.h` (`alp_capabilities_t`/`alp_status_t`). The `jpeg` caps here are a **self-contained struct** (`alp_jpeg_caps_t`), not the generic `alp_capabilities_t` flags word — JPEG has structured caps (dimensions, subsample mask) that don't fit a flags bitfield. No `base_caps` wiring needed; set caps fields directly in each backend's `open`.

- [ ] **Step 2: Write the failing registry test**

`tests/unit/jpeg_registry/src/test_jpeg_registry.c`:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <alp/jpeg.h>
#include <alp/backend.h>

ZTEST(jpeg_registry, test_class_has_a_backend)
{
	/* At least the stub registers for class "jpeg". */
	zassert_true(ALP_BACKEND_AVAILABLE(jpeg), "no jpeg backend linked");
}

ZTEST(jpeg_registry, test_open_then_close_stub)
{
	/* native_sim has no HW backend; open resolves the stub, whose
	 * encode returns ALP_ERR_NOT_IMPLEMENTED but open/caps still work. */
	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t *h = alp_jpeg_open(&cfg);
	zassert_not_null(h, "stub open must succeed");

	alp_jpeg_caps_t caps;
	zassert_equal(alp_jpeg_capabilities(h, &caps), ALP_OK);
	zassert_false(caps.hw_accelerated, "stub is not hw");

	uint8_t out[16];
	size_t out_len = 0;
	alp_jpeg_encode_req_t req = { .width = 16, .height = 16,
	                              .subsample = ALP_JPEG_SUBSAMPLE_420,
	                              .quality = 75 };
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len),
	              ALP_ERR_NOT_IMPLEMENTED);
	alp_jpeg_close(h);
}

ZTEST_SUITE(jpeg_registry, NULL, NULL, NULL, NULL, NULL);
```
Add `tests/unit/jpeg_registry/CMakeLists.txt`, `prj.conf`, `testcase.yaml` mirroring `tests/unit/camera_registry/` (copy that dir's four files and rename symbols/paths).

- [ ] **Step 3: Run the test to verify it fails**

Run (WSL, per `reference_local_twister_invocation`):
```
west twister -T tests/unit/jpeg_registry -p native_sim --EXTRA_ZEPHYR_MODULES=$PWD -v
```
Expected: FAIL — `alp/jpeg.h: No such file` / undefined `alp_jpeg_open`.

- [ ] **Step 4: Write `include/alp/jpeg.h`**

Full header: copyright + `[ABI-EXPERIMENTAL]` Doxygen block (mirror `include/alp/camera.h:1-29`), include guard `ALP_JPEG_H`, `extern "C"`, the types + `ALP_JPEG_CONFIG_DEFAULT` + four prototypes from the Interfaces block, each prototype Doxygen-documented (`@brief`/`@param`/`@return`). Document `alp_jpeg_encode` return set: `ALP_OK`, `ALP_ERR_INVAL` (NULL/zero-dim/NULL req), `ALP_ERR_NOSUPPORT` (subsample the backend can't do), `ALP_ERR_NOMEM` (out_cap too small — `*out_len` set to required size when known), `ALP_ERR_IO` (HW fault), `ALP_ERR_NOT_IMPLEMENTED`, `ALP_ERR_NOT_READY`. Include `<stdbool.h>`, `<stdint.h>`, `<stddef.h>`, `alp/cap_instance.h` (for `alp_status_t`).

- [ ] **Step 5: Write `src/backends/jpeg/jpeg_ops.h`**

Copyright + "internal ABI, not public" banner (mirror `src/backends/camera/camera_ops.h:1-25`), guard `ALP_BACKENDS_JPEG_OPS_H`, includes (`alp/backend.h`, `alp/jpeg.h`), the vtable + handle struct from the Interfaces block. `in_use` MUST be the struct's last member.

- [ ] **Step 6: Write `src/jpeg_dispatch.c`**

Mirror `src/camera_dispatch.c` structure exactly, reduced to the three ops:
```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * JPEG-encoder class dispatcher.  Handle layout lives in
 * src/backends/jpeg/jpeg_ops.h.
 */
#include <stddef.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/jpeg.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "alp_z_last_error.h"
#include "backends/jpeg/jpeg_ops.h"

ALP_BACKEND_DEFINE_CLASS(jpeg);
ALP_BACKEND_ANCHOR(jpeg);

#ifndef CONFIG_ALP_SDK_MAX_JPEG_HANDLES
#define CONFIG_ALP_SDK_MAX_JPEG_HANDLES 1
#endif

static struct alp_jpeg _pool[CONFIG_ALP_SDK_MAX_JPEG_HANDLES];

static struct alp_jpeg *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_JPEG_HANDLES; ++i) {
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_jpeg, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_jpeg *h) { alp_slot_release(&h->in_use); }

alp_jpeg_t *alp_jpeg_open(const alp_jpeg_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("jpeg", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_jpeg_ops_t *ops = (const alp_jpeg_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_jpeg *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend   = be;
	h->state.ops = ops;
	alp_jpeg_caps_t caps = { 0 };
	alp_status_t    rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_jpeg_encode(alp_jpeg_t *h, const alp_jpeg_encode_req_t *req,
			     void *out_buf, size_t out_cap, size_t *out_len)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (req == NULL || out_buf == NULL || out_len == NULL ||
	    req->width == 0u || req->height == 0u) {
		rc = ALP_ERR_INVAL;
	} else {
		rc = h->state.ops->encode(&h->state, req, out_buf, out_cap, out_len);
	}
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_jpeg_capabilities(const alp_jpeg_t *h, alp_jpeg_caps_t *out)
{
	if (h == NULL || out == NULL) {
		return ALP_ERR_INVAL;
	}
	*out = h->cached_caps;
	return ALP_OK;
}

void alp_jpeg_close(alp_jpeg_t *h)
{
	if (h == NULL) {
		return;
	}
	if (!alp_handle_begin_close_blocking(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(h);
}
```

- [ ] **Step 7: Write `src/backends/jpeg/zephyr_stub.c`**

Mirror `src/backends/camera/zephyr_stub.c`. Ops: `open` fills `*caps_out = (alp_jpeg_caps_t){0}` and returns `ALP_OK` (so caps queries work); `encode` returns `ALP_ERR_NOT_IMPLEMENTED`; `close` is a no-op. Register:
```c
static const alp_jpeg_ops_t _ops = { .open = stub_open, .encode = stub_encode, .close = stub_close };

ALP_BACKEND_REGISTER(jpeg, zephyr_stub,
		     { .silicon_ref = "*", .vendor = "alp", .base_caps = 0u,
		       .priority = 0u, .ops = &_ops, .probe = NULL });
ALP_BACKEND_ANCHOR_DEFINE(jpeg);
```

- [ ] **Step 8: Write `src/common/stub/stub_jpeg.c`**

Mirror `src/common/stub/stub_camera.c` — the native/host build's inclusion of the stub backend TU. (If `stub_camera.c` merely `#include`s the zephyr_stub or is a separate registration, copy that exact shape.)

- [ ] **Step 9: Wire CMake — dispatcher + stub always built**

In `zephyr/CMakeLists.txt`, next to the camera dispatcher line (~87–90) add `src/jpeg_dispatch.c` and `src/backends/jpeg/zephyr_stub.c` to the always-built sources (the JPEG class must exist on every board, defaulting to the stub). Add `src/common/stub/stub_jpeg.c` under the same native/host guard `stub_camera.c` uses.

- [ ] **Step 10: Add the catalog + ABI-marker entries**

`metadata/catalog.json`: add a `jpeg` class block mirroring the camera entry (~1088–1097) — `header: "include/alp/jpeg.h"`, `symbols: ["alp_jpeg_open","alp_jpeg_encode","alp_jpeg_capabilities","alp_jpeg_close"]`. Run `python scripts/gen_catalog.py` (or `py -3.14 scripts/gen_catalog.py`) if the catalog is generated, and commit the regenerated file. Add an `[ABI-EXPERIMENTAL]` row for `<alp/jpeg.h>` to `docs/abi-markers.md`.

- [ ] **Step 11: Run the test to verify it passes**

Run the Step-3 twister command. Expected: PASS (`test_class_has_a_backend`, `test_open_then_close_stub`).

- [ ] **Step 12: Commit**

```bash
git add include/alp/jpeg.h src/jpeg_dispatch.c src/backends/jpeg/ src/common/stub/stub_jpeg.c \
        zephyr/CMakeLists.txt metadata/catalog.json docs/abi-markers.md tests/unit/jpeg_registry/
git commit -m "feat(jpeg): portable <alp/jpeg.h> encoder surface + dispatcher + stub"
```

---

### Task 2: Software baseline-JPEG fallback backend

Delivers a working software encoder selected on any SoM without a JPEG-HW backend. Proven by encoding a known pattern to a decodable JPEG on native_sim.

**Files:**
- Create: `src/backends/jpeg/vendor/toojpeg_baseline.h`
- Create: `src/backends/jpeg/vendor/toojpeg_baseline.c`
- Create: `src/backends/jpeg/sw_baseline.c`
- Modify: `zephyr/CMakeLists.txt` (`zephyr_library_sources_ifdef(CONFIG_ALP_SDK_JPEG_SW_BASELINE ...)`)
- Create: `zephyr/kconfigs/jpeg.kconfig`
- Modify: `zephyr/Kconfig` (or the kconfigs include point) to `rsource` the new `jpeg.kconfig`
- Test: extend `tests/unit/jpeg_registry/src/test_jpeg_registry.c`

**Interfaces:**
- Consumes: `alp_jpeg_ops_t`, `alp_jpeg_encode_req_t`, `alp_jpeg_caps_t` (Task 1).
- Produces: a registered `jpeg` backend `sw_baseline` (`silicon_ref="*"`, priority 50) — no new public symbols.

- [ ] **Step 1: Vendor the baseline encoder**

Add `src/backends/jpeg/vendor/toojpeg_baseline.{h,c}` — a compact public-domain baseline-sequential JPEG encoder (TooJpeg, Stephan Brumme, public-domain / zlib — verify the upstream licence permits redistribution and **keep its licence header verbatim** at the top of both files, plus a provenance comment: source repo + commit + "vendored, not modified except the entry-point rename"). Expose one entry point:
```c
/* Encodes YUV planar (4:2:0 or 4:0:0) to baseline JPEG via `sink`.
 * Returns bytes written, or 0 on error. */
size_t toojpeg_encode_yuv420(void *dst, size_t dst_cap,
                             const uint8_t *y, uint32_t y_stride,
                             const uint8_t *u, uint32_t u_stride,
                             const uint8_t *v, uint32_t v_stride,
                             uint16_t width, uint16_t height,
                             int mono, int quality);
```
If upstream is RGB-input only, add a thin YUV→RGB-free path (baseline JPEG stores YCbCr natively — feed Y/Cb/Cr directly, do NOT round-trip through RGB). `ponytail:` comment at the top of `sw_baseline.c` naming the ceiling: "baseline sequential, 4:2:0 + 4:0:0 only, no MJPEG/progressive/4:2:2/rate-control; swap for libjpeg-turbo only if a customer measurably needs more."

- [ ] **Step 2: Write the failing SW-encode test**

Append to `test_jpeg_registry.c`:
```c
ZTEST(jpeg_registry, test_sw_baseline_encodes_valid_jpeg)
{
	/* 16x16 solid mid-grey YUV420: Y=128, U=V=128. */
	static uint8_t y[16 * 16], u[8 * 8], v[8 * 8];
	memset(y, 128, sizeof(y));
	memset(u, 128, sizeof(u));
	memset(v, 128, sizeof(v));

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t *h = alp_jpeg_open(&cfg);
	zassert_not_null(h);

	alp_jpeg_caps_t caps;
	alp_jpeg_capabilities(h, &caps);
	zassert_false(caps.hw_accelerated);
	zassert_true(caps.subsample_mask & (1u << ALP_JPEG_SUBSAMPLE_420));

	uint8_t out[4096];
	size_t out_len = 0;
	alp_jpeg_encode_req_t req = {
		.width = 16, .height = 16, .subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality = 80,
		.y_plane = y, .y_stride = 16,
		.u_plane = u, .u_stride = 8,
		.v_plane = v, .v_stride = 8,
	};
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_OK);
	zassert_true(out_len > 4, "empty output");
	/* SOI ffd8 ... EOI ffd9 markers. */
	zassert_equal(out[0], 0xFF); zassert_equal(out[1], 0xD8);
	zassert_equal(out[out_len - 2], 0xFF); zassert_equal(out[out_len - 1], 0xD9);

	/* 4:2:2 not supported in software -> NOSUPPORT, not a silent resample. */
	req.subsample = ALP_JPEG_SUBSAMPLE_422;
	zassert_equal(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_ERR_NOSUPPORT);
	alp_jpeg_close(h);
}
```
This test currently resolves the **stub** (prio 0) → `test_sw_baseline_encodes_valid_jpeg` fails at the `ALP_OK` assert (stub returns NOT_IMPLEMENTED). That is the intended red state.

- [ ] **Step 3: Write `src/backends/jpeg/sw_baseline.c`**

Ops:
- `open`: fill caps — `hw_accelerated=false`, `mjpeg_supported=false`, `max_width=16384`, `max_height=16384`, `subsample_mask=(1u<<ALP_JPEG_SUBSAMPLE_400)|(1u<<ALP_JPEG_SUBSAMPLE_420)`. Return `ALP_OK`.
- `encode`: reject `ALP_JPEG_SUBSAMPLE_422` with `ALP_ERR_NOSUPPORT`; require `y_plane` non-NULL (and u/v non-NULL unless `_400`) else `ALP_ERR_INVAL`; call `toojpeg_encode_yuv420(...)`; if it returns 0 → `ALP_ERR_IO`; if the caller's `out_cap` was insufficient (encoder signals overflow) → `ALP_ERR_NOMEM`; else set `*out_len` and return `ALP_OK`.
- `close`: no-op.
Register `silicon_ref="*"`, `.vendor="alp"`, `.priority=50u`, `.ops=&_ops`. (This outranks the stub's 0, so `"*"` SoMs now get real encode; AEN's prio-100 HW backend from Task 4 still wins there.)

- [ ] **Step 4: Add Kconfig**

`zephyr/kconfigs/jpeg.kconfig`:
```
config ALP_SDK_JPEG_SW_BASELINE
	bool "Software baseline-JPEG encoder fallback"
	default y
	help
	  Portable baseline-sequential JPEG encoder used on any SoM
	  without a hardware JPEG backend.  4:2:0 and monochrome only.

config ALP_SDK_JPEG_ALIF_HANTRO
	bool "Alif Ensemble E8 Hantro VC9000E hardware JPEG encoder"
	depends on VIDEO_JPEG_HANTRO_VC9000E
	default n
	help
	  Hardware-accelerated JPEG encode on E1M-AEN801 (Alif E8).
	  Tier-2 opt-in; requires the vendored Zephyr video driver and
	  the hal_alif JPEG SW-helper library.
```
Wire it into the build's kconfig include set the way `power-camera-display.kconfig` is included. Add `zephyr_library_sources_ifdef(CONFIG_ALP_SDK_JPEG_SW_BASELINE src/backends/jpeg/sw_baseline.c src/backends/jpeg/vendor/toojpeg_baseline.c)` in `zephyr/CMakeLists.txt` near the camera backend block (~672–682).

- [ ] **Step 5: Run tests to verify pass**

Run the Step-3 twister command from Task 1. Expected: all four `jpeg_registry` tests PASS.

- [ ] **Step 6: clang-format + commit**

```bash
git clang-format --diff HEAD~1   # confirm house style clean; apply if needed
git add src/backends/jpeg/ zephyr/kconfigs/jpeg.kconfig zephyr/CMakeLists.txt tests/unit/jpeg_registry/
git commit -m "feat(jpeg): software baseline-JPEG fallback backend (4:2:0 + mono)"
```

---

### Task 3: Vendor the Apache-2.0 Zephyr Hantro VC9000E driver

Delivers the hardware driver + DT binding + blob link in the tree, building for the AEN board — no alp backend yet. Isolated so a reviewer can gate the vendored-source provenance/licence separately from the wrapper.

**Files:**
- Create: `zephyr/drivers/video/jpeg_hantro_vc9000e.c` (vendored from `alifsemi/zephyr_alif` `drivers/video/jpeg_hantro_vc9000e.c`)
- Create: `zephyr/drivers/video/jpeg_hantro_vc9000e_regs.h` (vendored `_regs.h`)
- Create: `zephyr/drivers/video/Kconfig.jpeg_hantro_vc9000e` (vendored) — provides `CONFIG_VIDEO_JPEG_HANTRO_VC9000E`
- Create: `zephyr/dts/bindings/video/verisilicon,hantro-vc9000e-jpeg.yaml` (vendored binding)
- Modify: `zephyr/CMakeLists.txt` (add the driver .c under `zephyr_library_sources_ifdef(CONFIG_VIDEO_JPEG_HANTRO_VC9000E ...)`, mirror the CPI/ISP provenance block ~431–478)
- Modify: `west.yml` — confirm the hal_alif module already exposes `drivers/jpeg`; enable `CONFIG_USE_ALIF_JPEG_SW_LIB` path (no manifest change expected — hal_alif is already pinned at `modules/hal/alif`; verify the pinned rev contains `drivers/jpeg`).

- [ ] **Step 1: Confirm the pinned hal_alif rev carries `drivers/jpeg`**

```bash
grep -nA6 'name: hal_alif' west.yml     # note the pinned revision
```
Fetch that exact rev's tree and confirm `drivers/jpeg/inc/jpeg_hantro_vc9000e_sw.h` + `Lib/libjpeg_hantro_sw_gcc.a` exist. If the pinned rev predates the JPEG driver, bump the `revision:` to a rev that includes it and note the bump in the commit message. **Risk:** bumping hal_alif can shift the ISP libisp ABI — run the existing camera/ISP build after any bump.

- [ ] **Step 2: Vendor the driver sources verbatim**

Copy the four files from `alifsemi/zephyr_alif` (same rev family as the already-vendored `video_alif.c` — check the provenance comment in `zephyr/CMakeLists.txt:431-478` for the exact upstream ref used). Keep each file's `Copyright (c) 2026 Alif Semiconductor / SPDX Apache-2.0` header. Prepend a provenance comment matching the existing vendored-driver convention: upstream repo, path, commit SHA, "vendored unmodified". Adjust only `#include` paths if the SDK's video-driver dir layout differs (compare against `zephyr/drivers/video/isp_pico.c`).

- [ ] **Step 3: Wire CMake + Kconfig**

Add to `zephyr/CMakeLists.txt` in the camera/video block: `zephyr_library_sources_ifdef(CONFIG_VIDEO_JPEG_HANTRO_VC9000E drivers/video/jpeg_hantro_vc9000e.c)`, with a provenance comment. Ensure `Kconfig.jpeg_hantro_vc9000e` is sourced by the video-drivers Kconfig aggregation (mirror how `Kconfig.isp_pico` / the ISP driver Kconfig is pulled in). Confirm `CONFIG_USE_ALIF_JPEG_SW_LIB` is reachable (it lives in the hal_alif module Kconfig, `depends on VIDEO_JPEG_HANTRO_VC9000E`).

- [ ] **Step 4: Add the DT node to the AEN overlay (build-only proof)**

In `examples/aen/aen-jpeg-regcheck/` (created in Task 4) — but for this task, add the JPEG node to the AEN board overlay used by the reg-check example's board dir. Node (verbatim silicon values):
```dts
jpeg0: jpeg@49044000 {
	compatible = "verisilicon,hantro-vc9000e-jpeg";
	reg = <0x49044000 0x1000>;
	interrupts = <360 0>;          /* JPEG_XINT_IRQ, M55 */
	quality-factor = <75>;
	status = "okay";
};
```
(The full 360–363 IRQ set + AXI-burst props go in the example's overlay in Task 4; this step only needs the node to compile the driver.)

- [ ] **Step 5: Build-only verify for the AEN board**

Build a minimal app with `CONFIG_VIDEO=y`, `CONFIG_VIDEO_JPEG_HANTRO_VC9000E=y`, `CONFIG_USE_ALIF_JPEG_SW_LIB=y` for the AEN801 board target (`alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he`). Expected: **compiles + links** (the hal_alif `.a` resolves `jpeg_qf_scaling`/`jpeg_calc_q_table`/`jpeg_set_q_table`/`jpeg_header_generation`). No run yet.

- [ ] **Step 6: Commit**

```bash
git add zephyr/drivers/video/jpeg_hantro_vc9000e.c zephyr/drivers/video/jpeg_hantro_vc9000e_regs.h \
        zephyr/drivers/video/Kconfig.jpeg_hantro_vc9000e \
        zephyr/dts/bindings/video/verisilicon,hantro-vc9000e-jpeg.yaml zephyr/CMakeLists.txt
git commit -m "feat(jpeg): vendor Apache-2.0 Hantro VC9000E Zephyr driver + DT binding"
```

---

### Task 4: Alif Hantro alp backend + AEN reg-check example (bench-gated)

Delivers `alif_hantro.c` wrapping the vendored Zephyr video driver, plus the AEN801 bring-up example. HW path is **not done until validated on real silicon**.

**Files:**
- Create: `src/backends/jpeg/alif_hantro.c`
- Modify: `zephyr/CMakeLists.txt` (`zephyr_library_sources_ifdef(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO src/backends/jpeg/alif_hantro.c)`)
- Create: `examples/aen/aen-jpeg-regcheck/` (`CMakeLists.txt`, `prj.conf`, `board.yaml`, `README.md`, `src/main.c`, `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`, `testcase.yaml`)

**Interfaces:**
- Consumes: `alp_jpeg_ops_t` (Task 1); the vendored driver's Zephyr `video` device API (Task 3); `DEVICE_DT_GET(DT_NODELABEL(jpeg0))`.
- Produces: registered `jpeg` backend `alif_hantro` (`silicon_ref="alif:ensemble:e8"`, priority 100).

- [ ] **Step 1: Read the vendored driver's exact device API**

Open `zephyr/drivers/video/jpeg_hantro_vc9000e.c` and identify how encode is driven: which `video_driver_api` callbacks it implements (`set_format`, `set_stream`/`enqueue`/`dequeue`) and any custom exported function. The register-level entry points seen upstream: `jpeg_hantro_vc9000e_set_format`, `jpeg_start_encode` (writes input ptr → `JPEG_SWREG12`, output ptr → `JPEG_SWREG8`, kicks via `JPEG_SWREG20`). Map the standard Zephyr `video` buffer flow: `video_set_format()` → `video_enqueue(input)` + `video_enqueue(output)` → `video_stream_start()` → `video_dequeue(output, K_MSEC(timeout))`. Write the backend against whichever surface the driver actually exposes — do not invent calls.

- [ ] **Step 2: Write `src/backends/jpeg/alif_hantro.c`**

- `open(cfg, state, caps_out)`: `const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(jpeg0));` — if `!device_is_ready(dev)` return `ALP_ERR_NOT_READY`. Stash `dev` in `state->be_data` (a small backend-state struct). Fill caps: `hw_accelerated=true`, `mjpeg_supported=true`, `max_width=16384`, `max_height=2048`, `subsample_mask` = 400|420|422. Return `ALP_OK`.
- `encode(...)`: translate `alp_jpeg_subsample_t` → the driver's pixelformat; `video_set_format()` with width/height/subsample + quality (via the node's `quality-factor` or a runtime control if the driver exposes one); enqueue the caller's YUV planes as the input buffer and a `video_buffer` over `out_buf`/`out_cap` as output; start; dequeue with `req`-derived timeout; on `-EAGAIN`/timeout → `ALP_ERR_TIMEOUT`; on abnormal-IRQ error path → `ALP_ERR_IO`; on success set `*out_len` = produced bytes and return `ALP_OK`. If `out_cap` < produced size the driver reports short-write → `ALP_ERR_NOMEM`.
- `close(state)`: `video_stream_stop()` if started; clear `be_data`.
- Register: `ALP_BACKEND_REGISTER(jpeg, alif_hantro, { .silicon_ref = "alif:ensemble:e8", .vendor = "alif", .base_caps = 0u, .priority = 100u, .ops = &_ops, .probe = NULL });`
- Guard the whole TU in `#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)`.

- [ ] **Step 3: Create the AEN reg-check example**

Mirror `examples/aen/aen-isp-regcheck/` (copy its file set). `src/main.c`: open `<alp/jpeg.h>`, query caps, print `hw_accelerated` + the block ID read the driver logs at init (proves the `0x49044000` mapping + clock-enable), then encode a small synthetic YUV420 frame and print `out_len` + first bytes (`FF D8`). `prj.conf`: `CONFIG_VIDEO=y`, `CONFIG_VIDEO_JPEG_HANTRO_VC9000E=y`, `CONFIG_USE_ALIF_JPEG_SW_LIB=y`, `CONFIG_ALP_SDK_JPEG_ALIF_HANTRO=y`. Overlay: the full `jpeg@49044000` node with `interrupts = <360 0>, <361 0>, <362 0>, <363 0>;` and the AXI props. `board.yaml` mirrors the other AEN examples. `testcase.yaml`: `build_only` for the AEN board + a `native_sim` variant that exercises the SW fallback (so CI builds it without hardware).

- [ ] **Step 4: Build-only CI verify**

`west twister -T examples/aen/aen-jpeg-regcheck -p native_sim --EXTRA_ZEPHYR_MODULES=$PWD` (SW-fallback variant runs) and a `build_only` AEN board build. Expected: both green.

- [ ] **Step 5: Commit (marks HW path bench-pending)**

```bash
git add src/backends/jpeg/alif_hantro.c zephyr/CMakeLists.txt examples/aen/aen-jpeg-regcheck/
git commit -m "feat(jpeg): Alif Hantro VC9000E hardware backend + aen-jpeg-regcheck example"
```

- [ ] **Step 6: BENCH VALIDATION (alp-bench-runner, real AEN801 — required before merge)**

Hardware step, serial, reservation-gated — dispatched to `alp-bench-runner`, never inside a workflow. Flash the reg-check app to a reserved AEN801, confirm over SWD/console: (a) driver logs the expected VC9000E hardware ID from `JPEG_SWREG0`; (b) `hw_accelerated=true`; (c) encode of a real captured frame returns `ALP_OK` with `out_len>0` and the bytes open as a valid JPEG in a standard viewer off-target. Record the ID/`out_len` verbatim in the PR. **The HW backend is not "done" until this passes** (`feedback_no_workarounds_real_som`, `feedback_prove_on_real_models`).

---

### Task 5: Docs, full gate, PR

**Files:**
- Modify: `docs/cli.md` / `docs/getting-started.md` only if a JPEG example is referenced there (per `updating-docs` — enumerate, don't assume).
- Modify: `CHANGELOG.md`, `VERSIONS.md` (new experimental surface entry).
- Modify: `tests/fixtures/ws6c-emit-parity/` (add a fixture only if `aen-jpeg-regcheck` joins the emit set).

- [ ] **Step 1: Run the doc + propagation gates**

`python scripts/check_doc_drift.py` and `scripts/check_doxygen_coverage.py` (per `updating-docs`); confirm `<alp/jpeg.h>` Doxygen coverage passes. Update `CHANGELOG.md`/`VERSIONS.md` with the `[ABI-EXPERIMENTAL]` JPEG surface line.

- [ ] **Step 2: Full local CI (per `running-local-ci`)**

Run the complete gate set — `bash scripts/test-all.sh` (no `--quick`) on Windows + the WSL twister gate. All green. Do NOT rely on a single test file.

- [ ] **Step 3: Reviewer pass**

Dispatch `alp-reviewer` over the branch diff (runs `reviewing-alp-changes` + the `check_*.py` gates + the licence/public-vs-internal lens on the vendored driver). Address findings.

- [ ] **Step 4: Open the PR (per `opening-github-prs-and-issues`)**

`feat/aen-jpeg-encoder` → `dev`. Body: what/why, the licence decision (DFP excluded, zephyr_alif Apache-2.0 vendored + hal_alif blob), the bench evidence from Task 4 Step 6, area/SoM labels (AEN, E8), linked issue. No AI attribution. Merge per `finalizing-a-merged-pr` (plain squash) only after the bench validation is recorded.

---

## Self-Review

- **Spec coverage:** §4.1 API → Task 1; §4.2 HW backend → Tasks 3+4; §4.3 SW fallback → Task 2; §4.4 metadata/catalog → Task 1 Step 10; §5 error handling → dispatch (Task 1) + per-backend (2,4); §6 testing → registry+SW test (1,2), reg-check example (4), bench (4 Step 6); §3 licence → Global Constraints + Tasks 3/5. All covered.
- **Placeholder scan:** no TBD/TODO; the one "read the real API first" step (Task 4 Step 1) points at a concrete vendored file with the exact functions named, not a hand-wave.
- **Type consistency:** `alp_jpeg_ops_t`/`alp_jpeg_backend_state_t`/`alp_jpeg_caps_t`/`alp_jpeg_encode_req_t` used identically across Tasks 1–4; `subsample_mask` bit convention `(1u<<ALP_JPEG_SUBSAMPLE_x)` consistent; error codes from the Global Constraints set throughout.
- **Open confirmations (cheap, first-use):** exact `ALP_ERR_*` spellings (`include/alp/error.h`); `stub_camera.c` inclusion shape; the vendored driver's exact `video` API surface; hal_alif pinned-rev contains `drivers/jpeg`.
