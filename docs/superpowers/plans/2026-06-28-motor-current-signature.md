# DC Motor Current-Signature Health Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DC motor/load current-signature health monitor: sample an INA236 current/voltage/power monitor, extract a feature window, classify operating state (OFF/NORMAL/INRUSH/OVERLOAD/STALL) deterministically, and emit an AI anomaly score for off-taxonomy faults.

**Architecture:** One pure-C, arch-neutral, host-unit-tested core — `current_features` (windowed current/power features + a deterministic 5-state classifier + a deterministic anomaly fallback) — plus an `<alp/inference.h>` anomaly model (stub) and a thin Zephyr `main.c`. Single electrical modality → one core.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/chips/ina236.h>` (current/voltage/power, I2C), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core (`current_features.{c,h}`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55. A `#ifndef M_PI` fallback near the top.
- App peripherals via portable `<alp/*>` APIs only (I2C via the `ina236_*` chip driver, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `CURR_WINDOW_N 256`, `CURR_SR_HZ 200.0f`, `CURR_FEATURE_DIM 7`, `CURR_STATE_COUNT 5`; config defaults `off_a 0.05f`, `overload_a 2.5f`, `ripple_min_a 0.05f`, `inrush_slope_a 1.0f`.
- At 200 Hz the Nyquist limit is 100 Hz; synthetic ripple in tests/demo uses **40 Hz** (resolvable). Faster commutation ripple (sensorless RPM) needs a higher readout rate — bench-gated, not a core claim.
- TDD: the core is RED-first, host-validated on `native_sim/native/64`. Sensor I/O + the AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude`; NO binaries (model is a 1-byte stub; recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- Example dir: `examples/ai/motor-current-signature/`. Primary target E1M-AEN; V2N via `som.sku` flip. INA236 on the EVK sensor I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT `/usr/bin/clang-format-14`).
- Unit test compiles the core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists (for `M_PI` on the host) — same pattern as the rail/acoustic/wearable examples. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-curr/twister.json`):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-curr'
  ```

---

## File Structure

- `examples/ai/motor-current-signature/src/current_features.{c,h}` — windowed features + classifier + anomaly fallback (Tasks 1-2).
- `examples/ai/motor-current-signature/src/main.c` — Zephyr glue (Task 3).
- `examples/ai/motor-current-signature/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.{conf,overlay}` + `models/README.md` (Task 3).
- `tests/unit/current_features/` — ztest suite (Tasks 1-2).
- `CHANGELOG.md` — entry (Task 3).

---

### Task 1: `current_features` — windowed current/power feature extraction + host tests

**Files:**
- Create: `examples/ai/motor-current-signature/src/current_features.h`
- Create: `examples/ai/motor-current-signature/src/current_features.c`
- Create: `tests/unit/current_features/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_current_features.c}`

**Interfaces:**
- Produces (Tasks 2/3): `CURR_WINDOW_N 256`, `CURR_SR_HZ 200.0f`, `CURR_FEATURE_DIM 7`; `struct curr_sample`; `struct curr_window_state`; `struct curr_features`; `curr_window_reset/push/full`; `curr_feat_extract`; `curr_feat_pack`.

- [x] **Step 1: Write the failing test**

Create `tests/unit/current_features/src/test_current_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for current_features (INA236 DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "current_features.h"

ZTEST_SUITE(current_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window with constant current + a 40 Hz ripple of the given amplitude. */
static void fill_current(struct curr_window_state *st, float dc_a, float ripple_a)
{
	curr_window_reset(st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float t = (float)i / CURR_SR_HZ;
		struct curr_sample s = {
		    .current_a = dc_a + ripple_a * sinf(2.0f * (float)M_PI * 40.0f * t),
		    .bus_v     = 12.0f,
		    .power_w   = 12.0f * dc_a,
		};
		curr_window_push(st, s);
	}
}

ZTEST(current_features, test_fill_and_pack_dim)
{
	struct curr_window_state st;
	struct curr_features     f;
	float                    vec[CURR_FEATURE_DIM];

	curr_window_reset(&st);
	zassert_false(curr_window_full(&st), "empty window not full");
	fill_current(&st, 1.0f, 0.1f);
	zassert_true(curr_window_full(&st), "full window reports full");

	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_equal(curr_feat_pack(&f, vec, CURR_FEATURE_DIM), (size_t)CURR_FEATURE_DIM,
	              "pack writes CURR_FEATURE_DIM");
}

ZTEST(current_features, test_means_and_ripple)
{
	struct curr_window_state st;
	struct curr_features     f;

	fill_current(&st, 1.0f, 0.1f); /* 1 A DC + 0.1 A ripple at 40 Hz */
	curr_feat_extract(&st, CURR_SR_HZ, &f);

	zassert_within((double)f.mean_current_a, 1.0, 0.02, "mean current ~1 A");
	zassert_within((double)f.rms_ac_a, 0.0707, 0.02, "ripple RMS ~ 0.1/sqrt(2)");
	zassert_within((double)f.ripple_freq_hz, 40.0, 3.0, "ripple at ~40 Hz");
	zassert_within((double)f.mean_bus_v, 12.0, 0.1, "mean bus voltage");
}

ZTEST(current_features, test_inrush_slope_is_negative)
{
	struct curr_window_state st;
	struct curr_features     f;

	/* Startup inrush: 5 A decaying linearly to 1 A across the window. */
	curr_window_reset(&st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float frac = (float)i / (float)(CURR_WINDOW_N - 1);
		struct curr_sample s = { .current_a = 5.0f - 4.0f * frac, .bus_v = 12.0f,
		                         .power_w = 36.0f };
		curr_window_push(&st, s);
	}
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_true(f.slope_a < -1.0f, "decaying inrush -> strongly negative slope");
}
```

- [x] **Step 2: Write the test scaffolding**

Create `tests/unit/current_features/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_current_features)

set(CURR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/motor-current-signature/src)
target_include_directories(app PRIVATE ${CURR_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_current_features.c
    ${CURR_SRC}/current_features.c
)
```

Create `tests/unit/current_features/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/current_features/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.current_features:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - power
      - predictive-maintenance
      - unit
```

- [x] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.current_features` build failure (`current_features.h`/`.c` missing).

- [x] **Step 4: Write the header**

Create `examples/ai/motor-current-signature/src/current_features.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * current_features -- pure-C windowed current/voltage/power feature extraction
 * for the DC motor current-signature health example.  Arch-neutral (stdint/math
 * only): builds for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef CURRENT_FEATURES_H
#define CURRENT_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CURR_WINDOW_N    256
#define CURR_SR_HZ       200.0f
/** mean_current + rms_ac + crest + slope + mean_power + mean_bus + ripple_freq. */
#define CURR_FEATURE_DIM 7

/** One INA236 sample, SI units. */
struct curr_sample {
	float current_a;
	float bus_v;
	float power_w;
};

struct curr_window_state {
	struct curr_sample s[CURR_WINDOW_N];
	uint16_t           count;
};

struct curr_features {
	float mean_current_a;
	float rms_ac_a;       /**< RMS of (current - mean): the ripple magnitude. */
	float crest;          /**< peak|current-mean| / rms_ac (0 when no ripple). */
	float slope_a;        /**< last-quarter mean - first-quarter mean (inrush < 0). */
	float mean_power_w;
	float mean_bus_v;
	float ripple_freq_hz; /**< dominant FFT bin of the AC current. */
};

void   curr_window_reset(struct curr_window_state *st);
void   curr_window_push(struct curr_window_state *st, struct curr_sample s);
bool   curr_window_full(const struct curr_window_state *st);
void   curr_feat_extract(const struct curr_window_state *st, float sr_hz,
                         struct curr_features *out);
size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_FEATURES_H */
```

- [x] **Step 5: Write the implementation**

Create `examples/ai/motor-current-signature/src/current_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * current_features implementation -- see current_features.h.
 */
#include "current_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void curr_window_reset(struct curr_window_state *st)
{
	st->count = 0;
}

void curr_window_push(struct curr_window_state *st, struct curr_sample s)
{
	if (st->count < CURR_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool curr_window_full(const struct curr_window_state *st)
{
	return st->count >= CURR_WINDOW_N;
}

/* In-place iterative radix-2 FFT (same proven routine as the rail example). */
static void fft_radix2(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float tr = re[i]; re[i] = re[j]; re[j] = tr;
			float ti = im[i]; im[i] = im[j]; im[j] = ti;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang);
		float wli = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float wr = 1.0f, wi = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int   a  = i + k;
				int   b  = i + k + len / 2;
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
}

void curr_feat_extract(const struct curr_window_state *st, float sr_hz, struct curr_features *out)
{
	const int n = (st->count < CURR_WINDOW_N) ? st->count : CURR_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	float sum_i = 0.0f, sum_p = 0.0f, sum_v = 0.0f;
	for (int i = 0; i < n; i++) {
		sum_i += st->s[i].current_a;
		sum_p += st->s[i].power_w;
		sum_v += st->s[i].bus_v;
	}
	out->mean_current_a = sum_i / (float)n;
	out->mean_power_w   = sum_p / (float)n;
	out->mean_bus_v     = sum_v / (float)n;

	float sum2 = 0.0f, peak = 0.0f;
	for (int i = 0; i < n; i++) {
		float ac = st->s[i].current_a - out->mean_current_a;
		sum2 += ac * ac;
		if (fabsf(ac) > peak) {
			peak = fabsf(ac);
		}
	}
	out->rms_ac_a = sqrtf(sum2 / (float)n);
	out->crest    = (out->rms_ac_a > 1e-6f) ? (peak / out->rms_ac_a) : 0.0f;

	/* Slope: last-quarter mean minus first-quarter mean (inrush -> negative). */
	int   q = n / 4;
	if (q < 1) {
		q = 1;
	}
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += st->s[i].current_a;
		last += st->s[n - 1 - i].current_a;
	}
	out->slope_a = (last - first) / (float)q;

	/* Dominant ripple frequency via FFT of the AC current. */
	static float re[CURR_WINDOW_N];
	static float im[CURR_WINDOW_N];
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->s[i].current_a - out->mean_current_a) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, CURR_WINDOW_N);
	const int half = CURR_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = re[k] * re[k] + im[k] * im[k];
		if (m2 > dom_val) {
			dom_val = m2;
			dom_bin = k;
		}
	}
	out->ripple_freq_hz = (float)dom_bin * sr_hz / (float)CURR_WINDOW_N;
}

size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)CURR_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++]  = f->mean_current_a;
	vec[i++]  = f->rms_ac_a;
	vec[i++]  = f->crest;
	vec[i++]  = f->slope_a;
	vec[i++]  = f->mean_power_w;
	vec[i++]  = f->mean_bus_v;
	vec[i++]  = f->ripple_freq_hz;
	return i; /* == CURR_FEATURE_DIM */
}
```

- [x] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.current_features` PASS, 3/3. (40 Hz at 200/256 → bin 51 → 39.8 Hz, within 3.)

- [x] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/ai/motor-current-signature/src/current_features.h \
        examples/ai/motor-current-signature/src/current_features.c \
        tests/unit/current_features
git commit -m "feat(curr): current_features windowed INA236 DSP (mean/ripple/crest/slope) + ztest"
```

---

### Task 2: `current_features` — 5-state classifier + anomaly fallback + host tests

**Files:**
- Modify: `examples/ai/motor-current-signature/src/current_features.{h,c}`
- Modify: `tests/unit/current_features/src/test_current_features.c`

**Interfaces:**
- Consumes: `struct curr_features` (Task 1).
- Produces (Task 3): `typedef enum { CURR_OFF=0, CURR_NORMAL=1, CURR_INRUSH=2, CURR_OVERLOAD=3, CURR_STALL=4, CURR_STATE_COUNT } curr_state_t;` `struct curr_config { float off_a; float overload_a; float ripple_min_a; float inrush_slope_a; };` `curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg);` `const char *curr_state_name(curr_state_t s);` `float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg);`

- [x] **Step 1: Write the failing test**

Append to `tests/unit/current_features/src/test_current_features.c`:

```c
static const struct curr_config CFG = {
    .off_a = 0.05f, .overload_a = 2.5f, .ripple_min_a = 0.05f, .inrush_slope_a = 1.0f
};

static struct curr_features feat_of(float dc_a, float ripple_a)
{
	struct curr_window_state st;
	struct curr_features     f;
	fill_current(&st, dc_a, ripple_a);
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	return f;
}

ZTEST(current_features, test_classify_off_and_normal)
{
	struct curr_features off = feat_of(0.01f, 0.0f);
	zassert_equal(current_classify(&off, &CFG), CURR_OFF, "near-zero -> OFF");

	struct curr_features nom = feat_of(1.0f, 0.1f);
	zassert_equal(current_classify(&nom, &CFG), CURR_NORMAL, "1 A + ripple -> NORMAL");
}

ZTEST(current_features, test_classify_overload_vs_stall)
{
	/* Both draw 3 A (> overload), but stall has NO ripple. */
	struct curr_features over  = feat_of(3.0f, 0.1f);
	struct curr_features stall = feat_of(3.0f, 0.0f);
	zassert_equal(current_classify(&over, &CFG), CURR_OVERLOAD, "high A + ripple -> OVERLOAD");
	zassert_equal(current_classify(&stall, &CFG), CURR_STALL, "high A + no ripple -> STALL");
}

ZTEST(current_features, test_classify_inrush)
{
	struct curr_window_state st;
	struct curr_features     f;
	curr_window_reset(&st);
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		float frac = (float)i / (float)(CURR_WINDOW_N - 1);
		struct curr_sample s = { .current_a = 5.0f - 4.0f * frac, .bus_v = 12.0f,
		                         .power_w = 36.0f };
		curr_window_push(&st, s);
	}
	curr_feat_extract(&st, CURR_SR_HZ, &f);
	zassert_equal(current_classify(&f, &CFG), CURR_INRUSH, "decaying startup -> INRUSH");
}

ZTEST(current_features, test_anomaly_and_names)
{
	struct curr_features nom   = feat_of(1.0f, 0.1f);
	struct curr_features stall = feat_of(3.0f, 0.0f);
	zassert_true(curr_anomaly_fallback(&nom, &CFG) < 0.2f, "healthy -> low anomaly");
	zassert_true(curr_anomaly_fallback(&stall, &CFG) > 0.8f, "stall -> high anomaly");

	zassert_true(strcmp(curr_state_name(CURR_OFF), "OFF") == 0, "name");
	zassert_true(strcmp(curr_state_name(CURR_STALL), "STALL") == 0, "name");
}
```

Add `#include <string.h>` at the top of the test if not already present.

- [x] **Step 2: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: build failure — `current_classify`/`curr_config`/`curr_state_t`/`curr_state_name`/`curr_anomaly_fallback` undeclared.

- [x] **Step 3: Add the API to the header**

Insert into `current_features.h` before the closing `#ifdef __cplusplus }`:

```c
/** Operating-state taxonomy (reference-grade; customers retune the config). */
typedef enum {
	CURR_OFF      = 0,
	CURR_NORMAL   = 1,
	CURR_INRUSH   = 2,
	CURR_OVERLOAD = 3,
	CURR_STALL    = 4,
	CURR_STATE_COUNT
} curr_state_t;

/** Motor-specific thresholds (Amps). */
struct curr_config {
	float off_a;          /**< below this mean current = OFF. */
	float overload_a;     /**< above this mean current = OVERLOAD/STALL. */
	float ripple_min_a;   /**< AC ripple below this at high current = STALL. */
	float inrush_slope_a; /**< slope below -this = decaying inrush. */
};

/** Classify the operating state from the features + config. */
curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg);

/** Stable upper-case state name for the record. */
const char *curr_state_name(curr_state_t s);

/**
 * Deterministic 0..1 anomaly score (overcurrent severity, saturating high on a
 * stall).  Used when no AI model is loaded.
 */
float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg);
```

- [x] **Step 4: Implement**

Append to `current_features.c`:

```c
curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg)
{
	if (f->mean_current_a < cfg->off_a) {
		return CURR_OFF;
	}
	if (f->slope_a < -cfg->inrush_slope_a) {
		return CURR_INRUSH; /* current decaying from a startup spike */
	}
	if (f->mean_current_a > cfg->overload_a) {
		return (f->rms_ac_a < cfg->ripple_min_a) ? CURR_STALL : CURR_OVERLOAD;
	}
	return CURR_NORMAL;
}

const char *curr_state_name(curr_state_t s)
{
	switch (s) {
	case CURR_OFF:
		return "OFF";
	case CURR_NORMAL:
		return "NORMAL";
	case CURR_INRUSH:
		return "INRUSH";
	case CURR_OVERLOAD:
		return "OVERLOAD";
	case CURR_STALL:
		return "STALL";
	default:
		return "UNKNOWN";
	}
}

float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg)
{
	float score = 0.0f;
	if (f->mean_current_a > cfg->overload_a && cfg->overload_a > 1e-6f) {
		score = (f->mean_current_a - cfg->overload_a) / cfg->overload_a;
	}
	/* High current with no ripple = stalled rotor: a strong anomaly. */
	if (f->mean_current_a > cfg->overload_a && f->rms_ac_a < cfg->ripple_min_a) {
		score = fmaxf(score, 0.9f);
	}
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
```

`fmaxf` needs `<math.h>` (already included in Task 1).

- [x] **Step 5: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.current_features` PASS, 7/7. If `test_classify_inrush` mis-labels (the inrush window's mean ≈ 3 A also exceeds overload), confirm the INRUSH slope check runs BEFORE the overload check (it does in the order above) — do not reorder.

- [x] **Step 6: Format + commit**

```bash
git add examples/ai/motor-current-signature/src/current_features.h \
        examples/ai/motor-current-signature/src/current_features.c \
        tests/unit/current_features/src/test_current_features.c
git commit -m "feat(curr): 5-state current classifier + anomaly fallback + tests"
```

---

### Task 3: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/ai/motor-current-signature/src/main.c`
- Create: `examples/ai/motor-current-signature/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/ai/motor-current-signature/boards/native_sim_native_64.{conf,overlay}`
- Create: `examples/ai/motor-current-signature/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `current_features.h`; portable `<alp/peripheral.h>` (I2C), `<alp/inference.h>`, `<alp/board.h>`, `<alp/chips/ina236.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `CURR,...` record per window, ending `[curr] done`.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/ai/motor-current-signature/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${CMAKE_CURRENT_BINARY_DIR}/alp.conf
            --emit zephyr-conf
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_BINARY_DIR}/alp.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(motor_current_signature LANGUAGES C)

target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/main.c
    src/current_features.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/motor-current-signature/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml**

Create `examples/ai/motor-current-signature/board.yaml`:
```yaml
# board.yaml -- DC motor current-signature health monitor.
#
# An M55 node reads the on-board INA236 high-side current/voltage/power monitor,
# extracts a feature window, classifies the motor operating state
# (OFF/NORMAL/INRUSH/OVERLOAD/STALL), and emits an AI anomaly score for
# off-taxonomy faults.  Same source targets the V2N DEEPX path when som.sku is
# flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

pins:
  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS, doc: "INA236 current monitor bus" }

cores:
  a32_cluster:
    os: "off"
  m55_hp:
    app: ./src
    inference:
      default_arena_kib: 64
    libraries:
      - tflite_micro
    peripherals:
      - i2c                   # INA236 link.

chips:
  - ina236                    # high-side current/voltage/power monitor.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim overlay + conf**

Create `examples/ai/motor-current-signature/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# native_sim has no real I2C controller; pull in the emul drivers so the INA236
# chip driver can open the sensor bus at boot.  No emul target is attached, so
# main.c falls back to synthetic current.
CONFIG_EMUL=y
CONFIG_I2C_EMUL=y
```

Create `examples/ai/motor-current-signature/boards/native_sim_native_64.overlay`:
```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-build overlay -- exposes one emulated I2C controller via the alp-i2c0
 * alias so alp_i2c_open(BOARD_I2C_SENSORS) resolves and the INA236 bring-up
 * runs.  No emul target is attached, so the chip-ID read fails and main.c
 * falls back to synthetic current.  On real silicon this file is NOT applied.
 */
#include <zephyr/dt-bindings/i2c/i2c.h>

/ {
	aliases {
		alp-i2c0 = &i2c0_emul;
	};

	soc {
		i2c0_emul: i2c@100 {
			compatible = "zephyr,i2c-emul-controller";
			status = "okay";
			reg = <0 0x100 4>;
			#address-cells = <1>;
			#size-cells = <0>;
			clock-frequency = <I2C_BITRATE_FAST>;
		};
	};
};
```

- [ ] **Step 5: Write main.c**

Create `examples/ai/motor-current-signature/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motor-current-signature
 * =======================
 *
 * DC motor / load current-signature health monitor.  Pipeline:
 *
 *   INA236 current/voltage/power (I2C, ~200 Hz read_all) --256-sample window-->
 *     current_features (mean/ripple/crest/slope/power) -> current_classify
 *       (OFF/NORMAL/INRUSH/OVERLOAD/STALL) + <alp/inference.h> anomaly score
 *       (deterministic fallback) --> one CURR record per window.
 *
 * Honest scope: DC-rail / brushed-DC-motor current-signature analysis (the
 * INA236's domain) -- NOT AC-mains NILM.  Monitor-only.  The model is a stub
 * (see models/README.md); with no model the deterministic classifier + anomaly
 * fallback run.  At 200 Hz the resolvable ripple is < 100 Hz (Nyquist);
 * sensorless RPM from faster commutation ripple is bench-gated.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/ina236.h"

#include "current_features.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(curr, LOG_LEVEL_INF);

#define INA236_ADDR     0x40u
#define INA236_MAX_A    8.0f
#define N_WINDOWS       5

static const struct curr_config CFG = {
    .off_a = 0.05f, .overload_a = 2.5f, .ripple_min_a = 0.05f, .inrush_slope_a = 1.0f
};

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic anomaly fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Synthetic current per window: one window per operating state. */
static struct curr_sample synth_sample(int window, int i)
{
	float t   = (float)i / CURR_SR_HZ;
	float rip = 0.1f * sinf(2.0f * (float)M_PI * 40.0f * t);
	struct curr_sample s = { .current_a = 0.0f, .bus_v = 12.0f, .power_w = 0.0f };
	switch (window) {
	case 0: /* OFF */
		s.current_a = 0.01f;
		break;
	case 1: /* NORMAL: 1 A + ripple */
		s.current_a = 1.0f + rip;
		break;
	case 2: { /* INRUSH: 5 A decaying to 1 A */
		float frac  = (float)i / (float)(CURR_WINDOW_N - 1);
		s.current_a = 5.0f - 4.0f * frac;
		break;
	}
	case 3: /* OVERLOAD: 3 A + ripple */
		s.current_a = 3.0f + rip;
		break;
	default: /* STALL: 3 A, no ripple */
		s.current_a = 3.0f;
		break;
	}
	s.power_w = s.current_a * s.bus_v;
	return s;
}

static float anomaly_score(alp_inference_t *inf, const struct curr_features *f)
{
	if (inf != NULL) {
		float vec[CURR_FEATURE_DIM];
		(void)curr_feat_pack(f, vec, CURR_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= sizeof(float)) {
					return ((const float *)out.data)[0];
				}
			}
		}
	}
	return curr_anomaly_fallback(f, &CFG);
}

int main(void)
{
	static ina236_t mon;
	static struct curr_window_state win;
	bool mon_ok = false;

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS,
	                                                   .bitrate_hz = 400000 });
	if (bus != NULL &&
	    ina236_init(&mon, bus, INA236_ADDR, INA236_MAX_A, INA236_ADCRANGE_0) == ALP_OK) {
		mon_ok = true;
	} else {
		LOG_WRN("INA236 unavailable; using synthetic current");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score\n");

	for (int w = 0; w < N_WINDOWS; w++) {
		curr_window_reset(&win);
		for (int i = 0; i < CURR_WINDOW_N; i++) {
			struct curr_sample s;
			if (mon_ok) {
				ina236_sample_t r;
				if (ina236_read_all(&mon, &r) == ALP_OK) {
					s.current_a = (float)r.current_ua / 1e6f;
					s.bus_v     = (float)r.bus_mv / 1e3f;
					s.power_w   = (float)r.power_uw / 1e6f;
				} else {
					s = synth_sample(w, i);
				}
			} else {
				s = synth_sample(w, i);
			}
			curr_window_push(&win, s);
		}

		struct curr_features f;
		curr_feat_extract(&win, CURR_SR_HZ, &f);
		curr_state_t st = current_classify(&f, &CFG);
		float        an = anomaly_score(inf, &f);

		printk("CURR,%.2f,%s,%.2f,%.1f,%.1f,%.2f\n", (double)(w * 1.28f),
		       curr_state_name(st), (double)f.mean_current_a, (double)f.mean_power_w,
		       (double)f.ripple_freq_hz, (double)an);
	}

	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (mon_ok) {
		ina236_reset(&mon);
	}
	if (bus != NULL) {
		alp_i2c_close(bus);
	}
	printk("[curr] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers (as the rail/wearable examples did). `alp_i2c_open` takes an `alp_i2c_config_t*` with `.bus_id`/`.bitrate_hz` (confirm against `include/alp/peripheral.h`; mirror `examples/ai/ai-anomaly-detection-vibration/src/main.c`). `ina236_init(ctx, bus, i2c_addr, max_current_a, adcrange)` + `ina236_read_all(ctx, &ina236_sample_t{bus_mv,shunt_uv,current_ua,power_uw})` + `ina236_reset` per `include/alp/chips/ina236.h` — confirm the `ina236_adcrange_t` enum value name (e.g. `INA236_ADCRANGE_0`); mirror the INA236 usage in `examples/display/drone-hud/src/sensors.c` if it differs. `BOARD_I2C_SENSORS` from `<alp/board.h>`. The `alp_inference_*` calls per the sibling examples. Add `<string.h>`/`<math.h>` if a symbol is unresolved. Keep `<alp/*>` portable — no vendor names.

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only`)

Create `examples/ai/motor-current-signature/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: motor-current-signature
  description: |
    DC motor current-signature health monitor: INA236 current/voltage/power
    -> windowed features -> 5-state classifier (OFF/NORMAL/INRUSH/OVERLOAD/
    STALL) + AI anomaly score (deterministic fallback).  native_sim runs
    synthetic current covering all five states.
common:
  tags: ai inference industrial predictive-maintenance power marketing showcase
tests:
  alp_sdk.example.motor_current_signature.e1m_evk:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
    platform_allow:
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp-sdk
      - example
      - ai
      - industrial
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[curr\\] done"

  alp_sdk.example.motor_current_signature.aen_build:
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - ai
      - industrial
```

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/ai/motor-current-signature/models/README.md`:
```markdown
# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic 5-state
classifier + anomaly fallback run without one. The named states
(OFF/NORMAL/INRUSH/OVERLOAD/STALL) are threshold rules; the AI adds an anomaly
score for **off-taxonomy** faults (early bearing wear, intermittent brush
arcing) that the thresholds miss.

1. **Record a healthy baseline** of the motor's current/voltage/power across its
   duty cycle, at the device window (256 samples @ 200 Hz). Tag with load.
2. **Extract the 7-feature `current_features` vector** per window.
3. **Train an autoencoder** (e.g. 7→4→2→4→7) on the healthy vectors; the
   reconstruction error is the anomaly score.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it in this folder and point `alp_inference_open` at it.

Tune the `curr_config` thresholds (`off_a`, `overload_a`, `ripple_min_a`,
`inrush_slope_a`) to your motor's nameplate current.

Honest scope: DC current-signature monitoring (the INA236's domain), NOT AC-mains
energy disaggregation. Sensorless RPM from commutation ripple needs a higher I2C
readout rate than 200 Hz (Nyquist 100 Hz) and is bench-gated.
```

- [ ] **Step 8: Write README.md**

Create `examples/ai/motor-current-signature/README.md`:
```markdown
# motor-current-signature

> ⚠️ **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `current_features`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic current covering all five states. HiL
> on a real motor + a trained model is bench-gated.

A DC motor / load **current-signature health monitor**: sample an INA236
high-side current/voltage/power monitor, extract a feature window, classify the
operating **state** (OFF/NORMAL/INRUSH/OVERLOAD/STALL), and emit an AI **anomaly
score** for off-taxonomy faults. The *electrical* PdM modality, complementing the
vibration and acoustic examples.

## Honest scope

The INA236 is a **DC** high-side shunt monitor. This is DC-rail / brushed-DC-motor
current-signature analysis (MCSA-style) — it is **NOT** AC-mains NILM / energy
disaggregation (that needs a dedicated AC energy-metering front-end). Monitor-only
(pairing with a `drv8833`/`a4988` driver to spin the motor is a noted extension).
At 200 Hz, resolvable ripple is < 100 Hz (Nyquist); sensorless RPM from faster
commutation ripple is bench-gated.

## Key discriminator

`rms_ac_a` (AC ripple magnitude) separates **STALL** (high current, *no*
commutation ripple → rotor not turning) from **OVERLOAD** (high current *with*
ripple → turning under load).

## Pipeline

```
INA236 (I2C, ~200 Hz) --window--> current_features (mean/ripple/crest/slope/power)
  -> current_classify (OFF/NORMAL/INRUSH/OVERLOAD/STALL)
  -> <alp/inference.h> anomaly score (deterministic fallback) -> CURR record
```

## Output

```
# CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score
CURR,1.28,NORMAL,1.02,12.3,39.8,0.08
CURR,5.12,STALL,3.10,37.2,0.0,0.90
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/motor-current-signature
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic classifier/fallback). See
`models/README.md` for the autoencoder training recipe.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/current_features
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **DC motor current-signature example** (`examples/ai/motor-current-signature/`):
  electrical-modality PdM — INA236 current/voltage/power → windowed
  `current_features` (mean/ripple-RMS/crest/slope/power + dominant ripple
  frequency) → a deterministic 5-state classifier (OFF/NORMAL/INRUSH/OVERLOAD/
  STALL; the ripple magnitude separates a stalled rotor from a turning overload)
  + an `<alp/inference.h>` anomaly score with a deterministic fallback. The core
  is host-unit-tested on `native_sim` (`tests/unit/current_features`); model is a
  stub with a training recipe in `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.current_features` (7/7) PASS.
- `alp_sdk.example.motor_current_signature.e1m_evk` PASS on `native_sim/native/64` (console `[curr] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-curr/twister.json`. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 5 notes) — do NOT change the portable-API contract or the core's logic. The local AEN link env may hit the shared `alp_backends_*` orphan-section issue (same as sibling examples); if AEN fails ONLY with that, note it — CI is the AEN gate.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22, then:
```bash
git add examples/ai/motor-current-signature CHANGELOG.md
git commit -m "feat(curr): DC motor current-signature example app (INA236 DSP + classifier + anomaly) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 current_features → Task 1; classifier + anomaly fallback (C1/C2) → Task 2; C2 AI dispatch + C3 main.c + scaffolding + models/README + README + CHANGELOG → Task 3. Output record + taxonomy → Task 3 (main.c + README). Validation (one ztest suite + native_sim run) → Tasks 1-2 tests + Task 3 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 3 board.yaml + testcase.yaml. Honest scope (DC-not-NILM, Nyquist, monitor-only) → Task 3 README + models/README + spec. All spec sections map to a task.

**Type consistency:** `CURR_WINDOW_N 256`, `CURR_SR_HZ 200.0f`, `CURR_FEATURE_DIM 7`, `CURR_STATE_COUNT 5` consistent across header/impl/tests/main. `curr_sample/curr_window_state/curr_features/curr_window_reset/push/full/curr_feat_extract/curr_feat_pack/curr_state_t/curr_config/current_classify/curr_state_name/curr_anomaly_fallback` — names + signatures identical across tasks. `curr_feat_pack` writes exactly 7 in the documented order. Config defaults identical in spec + main.c + tests. Output schema (7 columns) identical in main.c + README + spec. Classifier order (OFF → INRUSH → STALL/OVERLOAD → NORMAL) handles the high-current overlaps; INRUSH is checked before the overload band so the decaying-startup window is not mislabeled OVERLOAD.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The 1-byte model stub + the synthetic generators are deliberate, documented design decisions. Synthetic ripple is 40 Hz (under the 100 Hz Nyquist at 200 Hz sampling).
```
