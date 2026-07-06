# Rail Predictive-Maintenance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A train-mounted edge demo that analyzes rail condition from vibration (DSP feature extraction → small AI classifier), and geotags each verdict to a track position (GNSS lat/lon + chainage), emitting one CSV record per track segment.

**Architecture:** Two pure-C, arch-neutral, host-unit-tested cores — `rail_features` (windowed DSP: RMS, crest, kurtosis, FFT band energies, dominant frequency, rail wavelength `λ=v/f`, plus a deterministic fallback classifier) and `rail_position` (haversine chainage accumulator + fixed-length segment binning + NMEA RMC parse). A thin Zephyr `src/main.c` wires the ICM-42670 accelerometer (I2C) and u-blox NEO-M9N GNSS (UART), runs the pipeline through the portable `<alp/inference.h>` classifier (deterministic fallback when no model), and prints the geotagged records.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/*>` portable peripheral + inference APIs, reused chip drivers `icm42670_*` / `ublox_neo_m9n_*`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core peripherals go through portable `<alp/*>` APIs only (I2C, UART, inference); no vendor (Ethos-U / DEEPX) name in app code — use `ALP_INFERENCE_BACKEND_AUTO`.
- `rail_features.{c,h}` and `rail_position.{c,h}` are pure C — only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>`, `<math.h>`. No Zephyr headers, no MMIO, no intrinsics. They must compile for `native_sim` AND the M55.
- TDD: each core is RED-first, host-validated on `native_sim/native/64`. The AI call and chip I/O are the only non-host-testable parts.
- "Alp Lab AB" in copyright headers (NOT "ALP Lab"); no `Co-Authored-By: Claude`; no binaries (model is a stub array); no confidential prose or local/OneDrive paths; no login-gated vendor links.
- Example dir: `examples/ai/rail-predictive-maintenance/`. Primary target E1M-AEN (Ethos-U); V2N (DEEPX) via `board.yaml` `som.sku` flip.
- Fixed constants: window `RAIL_WINDOW_N = 256`, sample rate `RAIL_ODR_HZ = 800.0f`, `RAIL_N_BANDS = 8`, `RAIL_FEATURE_DIM = 13` (3 scalars + 8 bands + dom_freq + wavelength), default segment length `25.0f` m, Earth radius `6371000.0` m.
- Format all new C (`examples/**` + `tests/**`) clang-format-22-clean.
- Twister `native_sim/native/64` is the load-bearing gate. Local invocation (literal paths, no `$VARS`, no pipe):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-rail'
  ```

---

## File Structure

- `examples/ai/rail-predictive-maintenance/src/rail_features.h` — DSP core API (Task 1 + 2).
- `examples/ai/rail-predictive-maintenance/src/rail_features.c` — DSP core impl (Task 1 + 2).
- `examples/ai/rail-predictive-maintenance/src/rail_position.h` — geotag core API (Task 3).
- `examples/ai/rail-predictive-maintenance/src/rail_position.c` — geotag core impl (Task 3).
- `examples/ai/rail-predictive-maintenance/src/main.c` — Zephyr glue (Task 4).
- `examples/ai/rail-predictive-maintenance/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` — scaffolding (Task 4).
- `examples/ai/rail-predictive-maintenance/boards/native_sim_native_64.{conf,overlay}` — hermetic host build (Task 4).
- `tests/unit/rail_features/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_rail_features.c}` — DSP core ztest (Task 1 + 2).
- `tests/unit/rail_position/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_rail_position.c}` — geotag core ztest (Task 3).
- `CHANGELOG.md` — entry (Task 4).

---

### Task 1: `rail_features` — DSP feature extraction + host tests

**Files:**
- Create: `examples/ai/rail-predictive-maintenance/src/rail_features.h`
- Create: `examples/ai/rail-predictive-maintenance/src/rail_features.c`
- Create: `tests/unit/rail_features/CMakeLists.txt`
- Create: `tests/unit/rail_features/prj.conf`
- Create: `tests/unit/rail_features/testcase.yaml`
- Create: `tests/unit/rail_features/src/test_rail_features.c`

**Interfaces:**
- Produces (consumed by Task 2, Task 4):
  - `#define RAIL_WINDOW_N 256`, `RAIL_N_BANDS 8`, `RAIL_FEATURE_DIM 13`, `RAIL_ODR_HZ 800.0f`
  - `struct rail_feat_state { float samples[RAIL_WINDOW_N]; uint16_t count; }`
  - `struct rail_features { float rms; float crest_factor; float kurtosis; float band_energy[RAIL_N_BANDS]; float dom_freq_hz; float rail_wavelength_m; }`
  - `void rail_feat_state_reset(struct rail_feat_state *st);`
  - `void rail_feat_window_push(struct rail_feat_state *st, float sample);`
  - `bool rail_feat_window_full(const struct rail_feat_state *st);`
  - `void rail_feat_extract(const struct rail_feat_state *st, float odr_hz, float speed_mps, struct rail_features *out);`
  - `size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap);`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/rail_features/src/test_rail_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rail_features (DSP feature extraction) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "rail_features.h"

ZTEST_SUITE(rail_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window by pushing N samples produced by gen(i). */
static void fill(struct rail_feat_state *st, float (*gen)(int))
{
	rail_feat_state_reset(st);
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		rail_feat_window_push(st, gen(i));
	}
}

static float gen_quiet(int i) { (void)i; return 0.001f; }

/* A pure tone at 100 Hz given ODR 800 -> 8 samples/cycle. */
static float gen_tone_100hz(int i)
{
	return sinf(2.0f * (float)M_PI * 100.0f * (float)i / RAIL_ODR_HZ);
}

ZTEST(rail_features, test_window_fill_and_pack_dim)
{
	struct rail_feat_state st;
	struct rail_features   f;
	float                  vec[RAIL_FEATURE_DIM];

	rail_feat_state_reset(&st);
	zassert_false(rail_feat_window_full(&st), "empty window not full");
	fill(&st, gen_quiet);
	zassert_true(rail_feat_window_full(&st), "full window reports full");

	rail_feat_extract(&st, RAIL_ODR_HZ, 0.0f, &f);
	size_t n = rail_feat_pack(&f, vec, RAIL_FEATURE_DIM);
	zassert_equal(n, (size_t)RAIL_FEATURE_DIM, "pack writes RAIL_FEATURE_DIM values");
}

ZTEST(rail_features, test_quiet_is_low_energy)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_quiet);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	zassert_true(f.rms < 0.01f, "quiet window has near-zero RMS");
}

ZTEST(rail_features, test_tone_dominant_frequency_and_wavelength)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_tone_100hz);
	/* speed 20 m/s, tone 100 Hz -> wavelength 0.20 m. */
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);

	/* FFT bin resolution = 800/256 = 3.125 Hz; 100 Hz lands at bin 32. */
	zassert_within(f.dom_freq_hz, 100.0f, 4.0f, "dominant frequency ~100 Hz");
	zassert_within(f.rail_wavelength_m, 0.20f, 0.02f, "wavelength = speed/freq");
}

ZTEST(rail_features, test_wavelength_guarded_on_zero_speed)
{
	struct rail_feat_state st;
	struct rail_features   f;

	fill(&st, gen_tone_100hz);
	rail_feat_extract(&st, RAIL_ODR_HZ, 0.0f, &f);
	zassert_equal(f.rail_wavelength_m, 0.0f, "zero speed -> wavelength 0 (guarded)");
}
```

- [ ] **Step 2: Write CMakeLists / prj.conf / testcase.yaml for the test**

Create `tests/unit/rail_features/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rail_features)

set(RAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/rail-predictive-maintenance/src)
target_include_directories(app PRIVATE ${RAIL_SRC})
target_sources(app PRIVATE
    src/test_rail_features.c
    ${RAIL_SRC}/rail_features.c
)
```

Create `tests/unit/rail_features/prj.conf`:

```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/rail_features/testcase.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.rail_features:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - dsp
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run the test to verify it fails (RED)**

Run the twister command from Global Constraints (testsuite-root `tests/unit` is enough for this task).
Expected: `alp.unit.rail_features` FAILS to build (`rail_features.h` / `rail_features.c` do not exist yet). Confirm the failure is a build error, not a pass.

- [ ] **Step 4: Write the header**

Create `examples/ai/rail-predictive-maintenance/src/rail_features.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features -- pure-C DSP feature extraction for rail-vibration
 * predictive maintenance.  Arch-neutral (stdint/math only): builds for
 * native_sim and the Cortex-M55 alike, and is host-unit-tested.
 *
 * Pipeline role: a sliding window of per-sample vibration magnitude is
 * reduced to a small fixed-length feature vector (the AI classifier's
 * input) plus the physically-meaningful dominant-frequency / rail-
 * wavelength pair.  Corrugation is fixed in wavelength (lambda = v/f),
 * so the speed-normalised wavelength is the speed-invariant feature.
 */
#ifndef RAIL_FEATURES_H
#define RAIL_FEATURES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAIL_WINDOW_N    256
#define RAIL_N_BANDS     8
#define RAIL_ODR_HZ      800.0f
/** 3 scalars (rms, crest, kurtosis) + 8 bands + dom_freq + wavelength. */
#define RAIL_FEATURE_DIM (3 + RAIL_N_BANDS + 2)

/** Accumulating sample window. */
struct rail_feat_state {
	float    samples[RAIL_WINDOW_N];
	uint16_t count;
};

/** Extracted per-window features. */
struct rail_features {
	float rms;               /**< AC RMS (DC removed) -- broadband energy. */
	float crest_factor;      /**< peak/RMS -- impulsive defects. */
	float kurtosis;          /**< 4th moment -- impulsiveness. */
	float band_energy[RAIL_N_BANDS]; /**< normalised log-band energy. */
	float dom_freq_hz;       /**< frequency of the peak spectral bin. */
	float rail_wavelength_m; /**< speed/dom_freq (0 when guarded). */
};

/** Reset the window (count = 0). */
void rail_feat_state_reset(struct rail_feat_state *st);

/** Append one vibration-magnitude sample; ignored once the window is full. */
void rail_feat_window_push(struct rail_feat_state *st, float sample);

/** True once RAIL_WINDOW_N samples have been pushed. */
bool rail_feat_window_full(const struct rail_feat_state *st);

/**
 * Reduce a full window to features.  @p odr_hz is the sample rate (Hz),
 * @p speed_mps the train speed (m/s, 0 if unknown).  Safe on a partial
 * window (treats count as the length).
 */
void rail_feat_extract(const struct rail_feat_state *st, float odr_hz,
                       float speed_mps, struct rail_features *out);

/**
 * Pack @p f into the AI feature vector.  Writes exactly RAIL_FEATURE_DIM
 * floats when @p cap is large enough; returns the number written.
 */
size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_FEATURES_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/ai/rail-predictive-maintenance/src/rail_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features implementation -- see rail_features.h.
 */
#include "rail_features.h"

#include <math.h>
#include <string.h>

void rail_feat_state_reset(struct rail_feat_state *st)
{
	st->count = 0;
}

void rail_feat_window_push(struct rail_feat_state *st, float sample)
{
	if (st->count < RAIL_WINDOW_N) {
		st->samples[st->count++] = sample;
	}
}

bool rail_feat_window_full(const struct rail_feat_state *st)
{
	return st->count >= RAIL_WINDOW_N;
}

/* In-place iterative radix-2 FFT, N = RAIL_WINDOW_N, re/im length N. */
static void fft_radix2(float *re, float *im, int n)
{
	/* Bit-reversal permutation. */
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
		float ang  = -2.0f * (float)M_PI / (float)len;
		float wlr  = cosf(ang);
		float wli  = sinf(ang);
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

void rail_feat_extract(const struct rail_feat_state *st, float odr_hz,
                       float speed_mps, struct rail_features *out)
{
	const int n = (st->count < RAIL_WINDOW_N) ? st->count : RAIL_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* Mean (DC) removal. */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	/* Time-domain moments: RMS, peak, kurtosis. */
	float sum2 = 0.0f, peak = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x  = st->samples[i] - mean;
		float ax = fabsf(x);
		sum2 += x * x;
		sum4 += x * x * x * x;
		if (ax > peak) {
			peak = ax;
		}
	}
	float var = sum2 / (float)n;
	out->rms  = sqrtf(var);
	out->crest_factor = (out->rms > 1e-9f) ? (peak / out->rms) : 0.0f;
	out->kurtosis = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/* Spectrum over a fixed RAIL_WINDOW_N FFT (zero-pad a short window). */
	static float re[RAIL_WINDOW_N];
	static float im[RAIL_WINDOW_N];
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, RAIL_WINDOW_N);

	/* Magnitude-squared over the first half; find the dominant bin. */
	const int half = RAIL_WINDOW_N / 2;
	float     mag2[RAIL_WINDOW_N / 2];
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 0; k < half; k++) {
		mag2[k] = re[k] * re[k] + im[k] * im[k];
		if (k >= 1 && mag2[k] > dom_val) { /* skip DC bin 0 */
			dom_val = mag2[k];
			dom_bin = k;
		}
	}
	out->dom_freq_hz = (float)dom_bin * odr_hz / (float)RAIL_WINDOW_N;
	out->rail_wavelength_m =
	    (out->dom_freq_hz > 1e-6f && speed_mps > 1e-6f) ? (speed_mps / out->dom_freq_hz) : 0.0f;

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	float total = 0.0f;
	for (int k = 1; k < half; k++) {
		total += mag2[k];
	}
	if (total < 1e-20f) {
		return; /* bands stay zero */
	}
	for (int k = 1; k < half; k++) {
		/* Map bin -> band by log position across [1, half). */
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)RAIL_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= RAIL_N_BANDS) {
			b = RAIL_N_BANDS - 1;
		}
		out->band_energy[b] += mag2[k];
	}
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		out->band_energy[b] /= total;
	}
}

size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)RAIL_FEATURE_DIM) {
		return 0;
	}
	size_t i  = 0;
	vec[i++]  = f->rms;
	vec[i++]  = f->crest_factor;
	vec[i++]  = f->kurtosis;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->dom_freq_hz;
	vec[i++] = f->rail_wavelength_m;
	return i; /* == RAIL_FEATURE_DIM */
}
```

- [ ] **Step 6: Run the test to verify it passes (GREEN)**

Run the twister command (testsuite-root `tests/unit`).
Expected: `alp.unit.rail_features` PASS, 4/4 cases. Read the result from `/tmp/tw-rail/twister.json` (no pipe).

- [ ] **Step 7: Format + commit**

Format the new files with clang-format-22 (`examples/**` + `tests/**` are in scope). Then:

```bash
git add examples/ai/rail-predictive-maintenance/src/rail_features.h \
        examples/ai/rail-predictive-maintenance/src/rail_features.c \
        tests/unit/rail_features
git commit -m "feat(rail): rail_features DSP extraction (RMS/crest/kurtosis/FFT bands) + native_sim ztest"
```

---

### Task 2: `rail_features` — deterministic fallback classifier + host tests

**Files:**
- Modify: `examples/ai/rail-predictive-maintenance/src/rail_features.h` (add the classifier API)
- Modify: `examples/ai/rail-predictive-maintenance/src/rail_features.c` (add the classifier)
- Modify: `tests/unit/rail_features/src/test_rail_features.c` (add classifier cases)

**Interfaces:**
- Consumes: `struct rail_features` (Task 1).
- Produces (consumed by Task 4):
  - `typedef enum { RAIL_HEALTHY=0, RAIL_CORRUGATION=1, RAIL_JOINT_WELD=2, RAIL_ROUGH_RCF=3, RAIL_CLASS_COUNT } rail_class_t;`
  - `struct rail_verdict { rail_class_t cls; float severity; };`
  - `struct rail_verdict rail_classify_fallback(const struct rail_features *f);`
  - `const char *rail_class_name(rail_class_t c);`

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/rail_features/src/test_rail_features.c`:

```c
/* Periodic unit impulses every 16 samples -> high crest + high kurtosis. */
static float gen_impulse_train(int i)
{
	return (i % 16 == 0) ? 1.0f : 0.0f;
}

/* Broadband: a cheap deterministic pseudo-noise (no Math.random). */
static float gen_broadband(int i)
{
	float s = sinf((float)i * 1.7f) + sinf((float)i * 0.37f) + sinf((float)i * 3.91f);
	return s * 0.5f;
}

ZTEST(rail_features, test_classify_quiet_is_healthy)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_quiet);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_HEALTHY, "quiet -> HEALTHY");
	zassert_true(v.severity < 0.2f, "healthy severity is low");
}

ZTEST(rail_features, test_classify_impulse_is_joint_weld)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_impulse_train);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	zassert_true(f.crest_factor > 6.0f, "impulse train has high crest");
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_JOINT_WELD, "impulsive -> JOINT_WELD");
}

ZTEST(rail_features, test_classify_tone_is_corrugation)
{
	struct rail_feat_state st;
	struct rail_features   f;
	fill(&st, gen_tone_100hz);
	rail_feat_extract(&st, RAIL_ODR_HZ, 20.0f, &f);
	struct rail_verdict v = rail_classify_fallback(&f);
	zassert_equal(v.cls, RAIL_CORRUGATION, "narrowband tone -> CORRUGATION");
}

ZTEST(rail_features, test_class_name_round_trip)
{
	zassert_true(strcmp(rail_class_name(RAIL_HEALTHY), "HEALTHY") == 0, "name");
	zassert_true(strcmp(rail_class_name(RAIL_JOINT_WELD), "JOINT_WELD") == 0, "name");
}
```

Add `#include <string.h>` at the top of the test if not present.

- [ ] **Step 2: Run the test to verify it fails (RED)**

Run twister (testsuite-root `tests/unit`).
Expected: build failure — `rail_classify_fallback` / `rail_verdict` / `rail_class_name` undeclared.

- [ ] **Step 3: Add the classifier API to the header**

Insert into `rail_features.h` before the closing `#ifdef __cplusplus }`:

```c
/** Rail-defect taxonomy (reference-grade; customers retrain/retune). */
typedef enum {
	RAIL_HEALTHY     = 0,
	RAIL_CORRUGATION = 1,
	RAIL_JOINT_WELD  = 2,
	RAIL_ROUGH_RCF   = 3,
	RAIL_CLASS_COUNT
} rail_class_t;

/** Classifier output: class + 0..1 severity (1 - P(healthy)). */
struct rail_verdict {
	rail_class_t cls;
	float        severity;
};

/**
 * Deterministic rule-of-thumb classifier over the feature vector.  Used
 * when no AI model is loaded (e.g. native_sim) so the demo still produces
 * sensible geotagged output; the AI path overrides it when present.
 */
struct rail_verdict rail_classify_fallback(const struct rail_features *f);

/** Stable upper-case name for a class (for the CSV record). */
const char *rail_class_name(rail_class_t c);
```

- [ ] **Step 4: Implement the classifier**

Append to `rail_features.c`:

```c
struct rail_verdict rail_classify_fallback(const struct rail_features *f)
{
	struct rail_verdict v = { RAIL_HEALTHY, 0.0f };

	/* Narrowband ratio: fraction of spectral energy in the strongest band. */
	float band_max = 0.0f;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		if (f->band_energy[b] > band_max) {
			band_max = f->band_energy[b];
		}
	}

	if (f->crest_factor > 6.0f && f->kurtosis > 5.0f) {
		v.cls      = RAIL_JOINT_WELD;
		v.severity = fminf(1.0f, (f->crest_factor - 6.0f) / 6.0f);
	} else if (band_max > 0.5f) {
		v.cls      = RAIL_CORRUGATION;
		v.severity = fminf(1.0f, band_max);
	} else if (f->rms > 0.30f) {
		v.cls      = RAIL_ROUGH_RCF;
		v.severity = fminf(1.0f, (f->rms - 0.30f) / 0.30f);
	} else {
		v.cls      = RAIL_HEALTHY;
		v.severity = 0.0f;
	}
	return v;
}

const char *rail_class_name(rail_class_t c)
{
	switch (c) {
	case RAIL_HEALTHY:
		return "HEALTHY";
	case RAIL_CORRUGATION:
		return "CORRUGATION";
	case RAIL_JOINT_WELD:
		return "JOINT_WELD";
	case RAIL_ROUGH_RCF:
		return "ROUGH_RCF";
	default:
		return "UNKNOWN";
	}
}
```

Add `#include <math.h>` is already present (Task 1). The classifier uses `fminf`.

- [ ] **Step 5: Run the test to verify it passes (GREEN)**

Run twister (testsuite-root `tests/unit`).
Expected: `alp.unit.rail_features` PASS, 8/8 cases. If `test_classify_tone_is_corrugation` fails because the tone's `band_max` ≤ 0.5, tighten the synthetic tone (it is a single bin, so band_max should dominate) — do NOT loosen the threshold without re-checking the impulse/quiet cases still pass.

- [ ] **Step 6: Format + commit**

```bash
git add examples/ai/rail-predictive-maintenance/src/rail_features.h \
        examples/ai/rail-predictive-maintenance/src/rail_features.c \
        tests/unit/rail_features/src/test_rail_features.c
git commit -m "feat(rail): deterministic fallback classifier over rail features + tests"
```

---

### Task 3: `rail_position` — haversine chainage + segment binning + NMEA RMC + host tests

**Files:**
- Create: `examples/ai/rail-predictive-maintenance/src/rail_position.h`
- Create: `examples/ai/rail-predictive-maintenance/src/rail_position.c`
- Create: `tests/unit/rail_position/CMakeLists.txt`
- Create: `tests/unit/rail_position/prj.conf`
- Create: `tests/unit/rail_position/testcase.yaml`
- Create: `tests/unit/rail_position/src/test_rail_position.c`

**Interfaces:**
- Produces (consumed by Task 4):
  - `struct rail_pos_state { double last_lat; double last_lon; bool have_last; double chainage_m; int32_t segment_index; float segment_len_m; }`
  - `void rail_pos_init(struct rail_pos_state *st, float segment_len_m);`
  - `double rail_pos_haversine_m(double lat1, double lon1, double lat2, double lon2);`
  - `bool rail_pos_update(struct rail_pos_state *st, double lat, double lon, bool has_fix);`
  - `bool rail_pos_parse_rmc(const char *nmea, double *lat, double *lon, float *speed_mps, bool *has_fix);`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/rail_position/src/test_rail_position.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rail_position (haversine chainage / segments / RMC).
 */
#include <math.h>
#include <string.h>
#include <zephyr/ztest.h>
#include "rail_position.h"

ZTEST_SUITE(rail_position, NULL, NULL, NULL, NULL, NULL);

ZTEST(rail_position, test_haversine_one_degree_latitude)
{
	/* 1 deg of latitude is ~111.19 km on a sphere of R=6371 km. */
	double d = rail_pos_haversine_m(0.0, 0.0, 1.0, 0.0);
	zassert_within(d, 111195.0, 200.0, "1 deg latitude ~= 111.2 km");
}

ZTEST(rail_position, test_chainage_accumulates_and_bins)
{
	struct rail_pos_state st;
	rail_pos_init(&st, 25.0f);

	/* First fix seeds position, no advance. */
	bool adv = rail_pos_update(&st, 0.0, 0.0, true);
	zassert_false(adv, "first fix does not advance a segment");
	zassert_equal(st.segment_index, 0, "start in segment 0");

	/* Step ~100 m north (0.0008993 deg lat ~= 100 m). */
	adv = rail_pos_update(&st, 0.0008993, 0.0, true);
	zassert_within(st.chainage_m, 100.0, 2.0, "chainage ~100 m");
	zassert_equal(st.segment_index, 4, "100 m / 25 m = segment 4");
	zassert_true(adv, "crossed into a new segment");
}

ZTEST(rail_position, test_no_fix_holds_chainage)
{
	struct rail_pos_state st;
	rail_pos_init(&st, 25.0f);
	rail_pos_update(&st, 0.0, 0.0, true);
	rail_pos_update(&st, 0.0008993, 0.0, true);
	double held = st.chainage_m;

	bool adv = rail_pos_update(&st, 5.0, 5.0, false); /* no fix: ignored */
	zassert_false(adv, "no-fix update does not advance");
	zassert_equal(st.chainage_m, held, "no-fix update does not move chainage");
}

ZTEST(rail_position, test_parse_rmc)
{
	/* A valid $GNRMC: status A, lat 5919.9999 N, lon 01803.7440 E, 12.0 kn. */
	const char *s =
	    "$GNRMC,083559.00,A,5919.99990,N,01803.74400,E,12.0,0.0,250626,,,A*XX";
	double lat = 0, lon = 0;
	float  spd = -1;
	bool   fix = false;
	bool   ok  = rail_pos_parse_rmc(s, &lat, &lon, &spd, &fix);
	zassert_true(ok, "RMC parsed");
	zassert_true(fix, "status A -> fix");
	zassert_within(lat, 59.3333, 0.01, "lat decimal degrees");
	zassert_within(lon, 18.0624, 0.01, "lon decimal degrees");
	zassert_within(spd, 6.17, 0.1, "12 knots -> ~6.17 m/s");
}

ZTEST(rail_position, test_parse_rmc_void_is_no_fix)
{
	const char *s = "$GNRMC,083559.00,V,,,,,,,250626,,,N*XX";
	double lat = 0, lon = 0;
	float  spd = -1;
	bool   fix = true;
	bool   ok  = rail_pos_parse_rmc(s, &lat, &lon, &spd, &fix);
	zassert_true(ok, "RMC recognised");
	zassert_false(fix, "status V -> no fix");
}
```

- [ ] **Step 2: Write the test scaffolding (CMakeLists/prj.conf/testcase.yaml)**

Create `tests/unit/rail_position/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rail_position)

set(RAIL_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/rail-predictive-maintenance/src)
target_include_directories(app PRIVATE ${RAIL_SRC})
target_sources(app PRIVATE
    src/test_rail_position.c
    ${RAIL_SRC}/rail_position.c
)
```

Create `tests/unit/rail_position/prj.conf`:

```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/rail_position/testcase.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.rail_position:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - gnss
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run the test to verify it fails (RED)**

Run twister (testsuite-root `tests/unit`).
Expected: `alp.unit.rail_position` build failure (`rail_position.c` missing).

- [ ] **Step 4: Write the header**

Create `examples/ai/rail-predictive-maintenance/src/rail_position.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_position -- pure-C geotagging for the rail survey: turn GNSS
 * fixes into an along-track distance (chainage) and a fixed-length
 * segment index, and parse the minimum NMEA needed (RMC).  Arch-neutral
 * (stdint/math only); host-unit-tested.
 */
#ifndef RAIL_POSITION_H
#define RAIL_POSITION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Running geotag state. */
struct rail_pos_state {
	double  last_lat;
	double  last_lon;
	bool    have_last;
	double  chainage_m;     /**< accumulated along-track distance. */
	int32_t segment_index;  /**< floor(chainage / segment_len_m). */
	float   segment_len_m;  /**< segment size (default 25 m). */
};

/** Initialise; @p segment_len_m <= 0 falls back to 25 m. */
void rail_pos_init(struct rail_pos_state *st, float segment_len_m);

/** Great-circle distance between two WGS84 points, metres. */
double rail_pos_haversine_m(double lat1, double lon1, double lat2, double lon2);

/**
 * Feed one GNSS sample.  With @p has_fix, accumulates distance from the
 * previous fixed point into chainage and recomputes the segment index;
 * without a fix the sample is ignored (chainage holds).  Returns true
 * iff the segment index advanced.
 */
bool rail_pos_update(struct rail_pos_state *st, double lat, double lon, bool has_fix);

/**
 * Parse a $--RMC sentence.  On success sets decimal-degree @p lat/@p lon,
 * @p speed_mps, and @p has_fix (status 'A' = fix, 'V' = void).  Returns
 * false if the sentence is not an RMC line.
 */
bool rail_pos_parse_rmc(const char *nmea, double *lat, double *lon, float *speed_mps,
                        bool *has_fix);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_POSITION_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/ai/rail-predictive-maintenance/src/rail_position.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_position implementation -- see rail_position.h.
 */
#include "rail_position.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAIL_EARTH_R_M 6371000.0
#define RAIL_DEG2RAD   (M_PI / 180.0)
#define RAIL_KNOT_MPS  0.514444f

void rail_pos_init(struct rail_pos_state *st, float segment_len_m)
{
	memset(st, 0, sizeof(*st));
	st->segment_len_m = (segment_len_m > 0.0f) ? segment_len_m : 25.0f;
}

double rail_pos_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
	double dlat = (lat2 - lat1) * RAIL_DEG2RAD;
	double dlon = (lon2 - lon1) * RAIL_DEG2RAD;
	double a    = sin(dlat / 2.0) * sin(dlat / 2.0) +
	           cos(lat1 * RAIL_DEG2RAD) * cos(lat2 * RAIL_DEG2RAD) * sin(dlon / 2.0) *
	               sin(dlon / 2.0);
	double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
	return RAIL_EARTH_R_M * c;
}

bool rail_pos_update(struct rail_pos_state *st, double lat, double lon, bool has_fix)
{
	if (!has_fix) {
		return false;
	}
	int32_t prev_seg = st->segment_index;
	if (st->have_last) {
		st->chainage_m += rail_pos_haversine_m(st->last_lat, st->last_lon, lat, lon);
		st->segment_index = (int32_t)(st->chainage_m / (double)st->segment_len_m);
	}
	st->last_lat  = lat;
	st->last_lon  = lon;
	st->have_last = true;
	return st->segment_index != prev_seg;
}

/* Convert ddmm.mmmm + hemisphere to signed decimal degrees. */
static double nmea_to_deg(const char *field, char hemi, int deg_digits)
{
	if (field == NULL || field[0] == '\0') {
		return 0.0;
	}
	double v       = atof(field);
	double degrees = floor(v / 100.0);
	double minutes = v - degrees * 100.0;
	double dec     = degrees + minutes / 60.0;
	(void)deg_digits;
	if (hemi == 'S' || hemi == 'W') {
		dec = -dec;
	}
	return dec;
}

bool rail_pos_parse_rmc(const char *nmea, double *lat, double *lon, float *speed_mps,
                        bool *has_fix)
{
	if (nmea == NULL) {
		return false;
	}
	/* Accept any talker: $__RMC (e.g. GNRMC, GPRMC). */
	if (!(nmea[0] == '$' && nmea[3] == 'R' && nmea[4] == 'M' && nmea[5] == 'C')) {
		return false;
	}

	/* Tokenise a local copy on commas.  RMC fields:
	 * 0:$xxRMC 1:time 2:status 3:lat 4:N/S 5:lon 6:E/W 7:speed(kn) ... */
	char buf[128];
	strncpy(buf, nmea, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	char  *fields[16] = { 0 };
	int    nf         = 0;
	char  *p          = buf;
	fields[nf++]      = p;
	for (; *p && nf < 16; p++) {
		if (*p == ',') {
			*p           = '\0';
			fields[nf++] = p + 1;
		}
	}

	bool fix = (nf > 2 && fields[2][0] == 'A');
	if (has_fix) {
		*has_fix = fix;
	}
	if (fix && nf > 7) {
		if (lat) {
			*lat = nmea_to_deg(fields[3], fields[4][0], 2);
		}
		if (lon) {
			*lon = nmea_to_deg(fields[5], fields[6][0], 3);
		}
		if (speed_mps) {
			*speed_mps = (float)atof(fields[7]) * RAIL_KNOT_MPS;
		}
	}
	return true;
}
```

- [ ] **Step 6: Run the test to verify it passes (GREEN)**

Run twister (testsuite-root `tests/unit`).
Expected: `alp.unit.rail_position` PASS, 5/5 cases.

- [ ] **Step 7: Format + commit**

```bash
git add examples/ai/rail-predictive-maintenance/src/rail_position.h \
        examples/ai/rail-predictive-maintenance/src/rail_position.c \
        tests/unit/rail_position
git commit -m "feat(rail): rail_position haversine chainage + segment binning + NMEA RMC parse + tests"
```

---

### Task 4: Example app — `main.c` glue, scaffolding, docs

**Files:**
- Create: `examples/ai/rail-predictive-maintenance/src/main.c`
- Create: `examples/ai/rail-predictive-maintenance/CMakeLists.txt`
- Create: `examples/ai/rail-predictive-maintenance/prj.conf`
- Create: `examples/ai/rail-predictive-maintenance/board.yaml`
- Create: `examples/ai/rail-predictive-maintenance/testcase.yaml`
- Create: `examples/ai/rail-predictive-maintenance/boards/native_sim_native_64.conf`
- Create: `examples/ai/rail-predictive-maintenance/boards/native_sim_native_64.overlay`
- Create: `examples/ai/rail-predictive-maintenance/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `rail_features.h` (Task 1+2) and `rail_position.h` (Task 3); the portable `<alp/peripheral.h>`, `<alp/inference.h>`, `<alp/board.h>`, chip headers `<alp/chips/icm42670.h>`, `<alp/chips/ublox_neo_m9n.h>`.
- Produces: a `native_sim/native/64` build that prints the header line and one `RAIL,...` record per segment, ending with `[rail] done`.

- [ ] **Step 1: Write the CMakeLists.txt**

Create `examples/ai/rail-predictive-maintenance/CMakeLists.txt` (mirrors the sibling vibration example's board.yaml→alp.conf flow):

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
set(OVERLAY_CONFIG ${CMAKE_CURRENT_BINARY_DIR}/alp.conf)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rail_predictive_maintenance LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/rail_features.c
    src/rail_position.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/rail-predictive-maintenance/prj.conf`:

```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

> The 16 KiB main stack covers the FFT scratch (`re`/`im` are file-static, but the
> feature/vector locals plus log formatting want headroom).

- [ ] **Step 3: Write board.yaml**

Create `examples/ai/rail-predictive-maintenance/board.yaml`:

```yaml
# board.yaml -- rail predictive-maintenance survey demo.
#
# A train-mounted node reads axlebox/carbody vibration from the on-board
# ICM-42670 accelerometer, extracts DSP features, classifies the rail
# defect (DSP -> small AI model, with a deterministic fallback), and
# geotags each verdict to a track position from the NEO-M9N GNSS
# (lat/lon + along-track chainage).  One CSV record per 25 m segment.
#
# Same source targets the V2N DEEPX path when som.sku is flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

pins:
  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS, doc: "ICM-42670 accelerometer bus" }
  - { e1m: E1M_UART1, macro: EVK_UART_PORT_ARDUINO, doc: "NEO-M9N GNSS NMEA (Arduino-header UART)" }

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
      - i2c                   # ICM-42670 link.
      - uart                  # NEO-M9N GNSS NMEA link.

chips:
  - icm42670                  # 6-axis IMU; accel only.
  - ublox_neo_m9n             # Multi-constellation GNSS.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim overlay + conf (hermetic host build)**

Create `examples/ai/rail-predictive-maintenance/boards/native_sim_native_64.conf`:

```
# SPDX-License-Identifier: Apache-2.0
#
# native_sim has no real I2C controller; pull in the emul drivers so the
# ICM-42670 chip driver can open the sensor bus at boot.  No GNSS UART is
# provided on native_sim -- main.c tolerates a NULL GNSS and replays a
# built-in canned NMEA track instead.
CONFIG_EMUL=y
CONFIG_I2C_EMUL=y
```

Create `examples/ai/rail-predictive-maintenance/boards/native_sim_native_64.overlay`:

```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-build overlay -- exposes one emulated I2C controller via the
 * alp-i2c0 alias so alp_i2c_open(BOARD_I2C_SENSORS) resolves and the
 * ICM-42670 bring-up runs.  No emul target is attached, so the WHO_AM_I
 * read fails and main.c falls back to its synthetic vibration generator.
 * The GNSS UART is intentionally absent here; main.c replays a canned
 * NMEA track when the GNSS port is unavailable.  On real silicon this
 * file is NOT applied.
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

Create `examples/ai/rail-predictive-maintenance/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail-predictive-maintenance
 * ===========================
 *
 * Train-mounted rail-condition survey.  Pipeline:
 *
 *   ICM-42670 accel (I2C) --256-sample window @ 800 Hz-->
 *     rail_features (DSP: RMS/crest/kurtosis/FFT bands/dom-freq/wavelength)
 *       --feature vector--> <alp/inference.h> AI classifier
 *                           (deterministic fallback when no model)
 *   NEO-M9N GNSS (UART NMEA) --> rail_position (haversine chainage + 25 m
 *                                segment binning)
 *     --> one CSV record per segment (worst class in the segment wins).
 *
 * The model is a stub: drop a Vela-compiled (AEN / Ethos-U) or DX-M1
 * (V2N) .tflite into models/ and point alp_inference_open at it.  With no
 * model (native_sim), the deterministic fallback classifier runs so the
 * survey still produces sensible geotagged output.
 *
 * Asset boundary: this surveys the RAIL (track), not the wheel.  Wheel
 * flats (periodic at wheel-rotation frequency) are out of scope.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/ublox_neo_m9n.h"

#include "rail_features.h"
#include "rail_position.h"

LOG_MODULE_REGISTER(rail_pdm, LOG_LEVEL_INF);

/* ICM-42670 ±2 g sensitivity: 16384 LSB/g (datasheet DS-000451). */
#define ICM_LSB_PER_G 16384.0f
#define IMU_I2C_ADDR  ICM42670_I2C_ADDR_HIGH /* E1M EVK straps AP_AD0 high. */

/* Number of windows to process in the bounded demo run. */
#define RAIL_DEMO_WINDOWS 64

/* A tiny canned NMEA track for native_sim (moving north at ~12 kn). */
static const char *const s_canned_track[] = {
	"$GNRMC,083559.00,A,5919.99990,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083600.00,A,5920.01000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083601.00,A,5920.02000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083602.00,A,5920.03000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
};

/* Synthetic vibration when no real IMU answers: alternate clean / tone /
 * impulse so the demo emits a mix of classes. */
static float synth_sample(int window, int i)
{
	switch (window % 3) {
	case 0: /* clean */
		return 0.001f;
	case 1: /* corrugation tone ~120 Hz */
		return sinf(2.0f * (float)M_PI * 120.0f * (float)i / RAIL_ODR_HZ);
	default: /* joint impulses */
		return (i % 16 == 0) ? 1.0f : 0.0f;
	}
}

struct rail_ctx {
	icm42670_t      imu;
	bool            imu_ok;
	ublox_neo_m9n_t gps;
	alp_uart_t     *gps_uart;
	alp_inference_t *inf;

	struct rail_pos_state pos;
	/* worst verdict seen in the current segment. */
	rail_class_t worst_cls;
	float        worst_sev;
	struct rail_features worst_feat;
};

static void emit_header(void)
{
	printk("# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,"
	       "rail_wavelength_m,fix\n");
}

static void emit_record(const struct rail_ctx *c, double lat, double lon, float speed,
                        bool fix)
{
	printk("RAIL,%.1f,%.6f,%.6f,%.1f,%s,%.2f,%.1f,%.4f,%d\n",
	       c->pos.chainage_m, lat, lon, (double)speed,
	       rail_class_name(c->worst_cls), (double)c->worst_sev,
	       (double)c->worst_feat.dom_freq_hz, (double)c->worst_feat.rail_wavelength_m,
	       fix ? 1 : 0);
}

/* Classify a feature vector via the AI model, else the deterministic fallback. */
static struct rail_verdict classify(struct rail_ctx *c, const struct rail_features *f)
{
	if (c->inf != NULL) {
		float vec[RAIL_FEATURE_DIM];
		(void)rail_feat_pack(f, vec, RAIL_FEATURE_DIM);

		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(c->inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(c->inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(c->inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= RAIL_CLASS_COUNT * sizeof(float)) {
					const float *scores = (const float *)out.data;
					int   best = 0;
					float bestv = scores[0];
					for (int k = 1; k < RAIL_CLASS_COUNT; k++) {
						if (scores[k] > bestv) {
							bestv = scores[k];
							best  = k;
						}
					}
					struct rail_verdict v = { (rail_class_t)best,
					                          1.0f - scores[RAIL_HEALTHY] };
					if (v.severity < 0.0f) {
						v.severity = 0.0f;
					}
					return v;
				}
			}
		}
	}
	return rail_classify_fallback(f);
}

static void seg_reset(struct rail_ctx *c)
{
	c->worst_cls = RAIL_HEALTHY;
	c->worst_sev = -1.0f;
}

int main(void)
{
	static struct rail_ctx c;
	struct rail_feat_state st;

	memset(&c, 0, sizeof(c));
	rail_feat_state_reset(&st);
	rail_pos_init(&c.pos, 25.0f);
	seg_reset(&c);

	/* --- I2C accelerometer (tolerate a missing chip on native_sim). --- */
	alp_i2c_t *bus = alp_i2c_open(BOARD_I2C_SENSORS);
	if (bus != NULL && icm42670_init(&c.imu, bus, IMU_I2C_ADDR) == ALP_OK &&
	    icm42670_set_accel(&c.imu, ICM42670_ODR_800_HZ, ICM42670_ACCEL_FS_2G) == ALP_OK) {
		c.imu_ok = true;
	} else {
		LOG_WRN("ICM-42670 unavailable; using synthetic vibration");
	}

	/* --- GNSS UART (tolerate absence on native_sim -> canned track). --- */
	c.gps_uart = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = E1M_UART1,
	    .baud_rate = 9600,
	});
	if (c.gps_uart != NULL) {
		(void)ublox_neo_m9n_init(&c.gps, c.gps_uart);
	} else {
		LOG_WRN("GNSS UART unavailable; replaying canned NMEA track");
	}

	/* --- AI model: NULL-tolerant; fallback classifier runs if absent. --- */
	c.inf = alp_inference_open(&(alp_inference_config_t){
	    .backend = ALP_INFERENCE_BACKEND_AUTO,
	    .format  = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = NULL,
	    .model_size = 0,
	});

	emit_header();

	double lat = 0.0, lon = 0.0;
	float  speed = 0.0f;
	bool   fix = false;
	uint8_t nmea[128];
	size_t  track_i = 0;

	for (int w = 0; w < RAIL_DEMO_WINDOWS; w++) {
		/* Fill one window. */
		rail_feat_state_reset(&st);
		for (int i = 0; i < RAIL_WINDOW_N; i++) {
			float sample;
			if (c.imu_ok) {
				icm42670_axes_t ax;
				if (icm42670_read_accel(&c.imu, &ax) == ALP_OK) {
					float g = sqrtf((float)ax.x * ax.x + (float)ax.y * ax.y +
					                (float)ax.z * ax.z) /
					          ICM_LSB_PER_G;
					sample = g;
				} else {
					sample = synth_sample(w, i);
				}
			} else {
				sample = synth_sample(w, i);
			}
			rail_feat_window_push(&st, sample);
		}

		/* Position: one GNSS sentence per window. */
		size_t len = 0;
		const char *line = NULL;
		if (c.gps_uart != NULL &&
		    ublox_neo_m9n_read_nmea_line(&c.gps, nmea, sizeof(nmea), &len, 5) == ALP_OK) {
			line = (const char *)nmea;
		} else {
			line = s_canned_track[track_i % ARRAY_SIZE(s_canned_track)];
			track_i++;
		}
		double plat = lat, plon = lon;
		float  pspd = speed;
		bool   pfix = false;
		if (rail_pos_parse_rmc(line, &plat, &plon, &pspd, &pfix) && pfix) {
			lat = plat;
			lon = plon;
			speed = pspd;
			fix = true;
		}

		/* Features + classification. */
		struct rail_features f;
		rail_feat_extract(&st, RAIL_ODR_HZ, speed, &f);
		struct rail_verdict v = classify(&c, &f);

		/* Track the worst verdict in the current segment. */
		if (v.severity > c.worst_sev) {
			c.worst_sev  = v.severity;
			c.worst_cls  = v.cls;
			c.worst_feat = f;
		}

		/* Advance position; emit a record on segment change. */
		if (rail_pos_update(&c.pos, lat, lon, fix)) {
			emit_record(&c, lat, lon, speed, fix);
			seg_reset(&c);
		}
	}

	alp_inference_close(c.inf);
	if (c.imu_ok) {
		icm42670_deinit(&c.imu);
	}
	printk("[rail] done\n");
	return 0;
}
```

> Implementer notes: `BOARD_I2C_SENSORS`, `alp_i2c_open`, `alp_uart_open`,
> `alp_uart_config_t`, `E1M_UART1`, `ALP_OK`, and `ARRAY_SIZE` come from
> `<alp/board.h>` / `<alp/peripheral.h>` / `<zephyr/sys/util.h>` (pulled via
> `<zephyr/kernel.h>`). If `alp_uart_config_t` has a different field set, match
> the sibling `drone-hud` usage (`.port_id`, `.baud_rate`). Add
> `#include <string.h>`/`<math.h>` if a symbol (`memcpy`, `sqrtf`, `sinf`) is
> unresolved. Keep `<alp/*>` peripheral usage portable — no vendor names.

- [ ] **Step 6: Write testcase.yaml**

Create `examples/ai/rail-predictive-maintenance/testcase.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: rail-predictive-maintenance
  description: |
    Train-mounted rail-condition survey: ICM-42670 vibration -> DSP
    features -> AI classifier (deterministic fallback) -> geotagged to
    GNSS lat/lon + chainage -> one CSV record per 25 m segment.
    native_sim replays a canned NMEA track + synthetic vibration.
common:
  tags: ai inference industrial predictive-maintenance gnss marketing showcase
tests:
  alp_sdk.example.rail_predictive_maintenance.e1m_evk:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
    platform_allow:
      - native_sim/native/64
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp-sdk
      - example
      - ai
      - industrial
      - aen
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[rail\\] done"

  alp_sdk.example.rail_predictive_maintenance.e1m_x_evk:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"'
    platform_allow:
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp-sdk
      - example
      - ai
      - industrial
      - v2n
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[rail\\] done"
```

> NOTE: the `e1m_evk` test is NOT `build_only` (unlike the sibling) — native_sim
> runs the canned pipeline to completion and the harness matches `[rail] done`.
> The AEN cross-build stays build-only by virtue of not being an integration
> platform.

- [ ] **Step 7: Write README.md**

Create `examples/ai/rail-predictive-maintenance/README.md`:

```markdown
# rail-predictive-maintenance

> ⚠️ **`[UNTESTED]` on hardware -- v0.6 paper-correct.** The DSP cores
> (`rail_features`, `rail_position`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> a canned NMEA track + synthetic vibration. HiL on a real bogie with a
> customer-trained model is bench-gated.

Train-mounted **rail-condition survey**: read axlebox/carbody vibration
from the on-board ICM-42670, extract DSP features, classify the rail
defect, and **geotag** each verdict to a track position (GNSS lat/lon +
along-track chainage). One CSV record per 25 m segment, ready for the
customer's GIS / MQTT gateway.

This monitors the **rail (infrastructure)**, complementing
`ai-anomaly-detection-vibration`, which monitors a **stationary asset**
(motor/pump bearing). Wheel-side defects (wheel flats) are out of scope.

## Pipeline

```
ICM-42670 (I2C) --window--> rail_features (RMS, crest, kurtosis, 8 FFT
  bands, dom-freq, rail wavelength = speed/freq) --feature vector-->
  <alp/inference.h> classifier (deterministic fallback if no model)
NEO-M9N GNSS (UART NMEA) --> rail_position (haversine chainage + segments)
  --> CSV record per segment
```

`λ = v / f` makes corrugation detection speed-invariant: corrugation is
fixed in *wavelength*, not frequency, so the speed-normalised wavelength
is the feature the classifier keys on.

## Defect taxonomy

| Class | Signature |
|-------|-----------|
| `HEALTHY` | baseline |
| `CORRUGATION` | periodic rail roughness (narrowband, stable wavelength) |
| `JOINT_WELD` | impulsive transient (high crest + kurtosis) |
| `ROUGH_RCF` | broadband roughness / rolling-contact fatigue |

## Output

```
# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,rail_wavelength_m,fix
RAIL,100.0,59.334591,18.062400,6.2,CORRUGATION,0.81,120.0,0.0517,1
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/rail-predictive-maintenance
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Drop in your own model

Place a Vela-compiled (AEN / Ethos-U) or DX-M1 (V2N) `.tflite` and point
`alp_inference_open` at it. Train a 4-class classifier
(`HEALTHY/CORRUGATION/JOINT_WELD/ROUGH_RCF`) over the
`RAIL_FEATURE_DIM`-float feature vector at the same 256-sample @ 800 Hz
window the app uses. With no model the deterministic fallback runs.

## Tests

```
# host unit tests for the DSP cores
twister -p native_sim/native/64 -T tests/unit/rail_features -T tests/unit/rail_position
```
```

- [ ] **Step 8: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md` (match the existing bullet style):

```markdown
- **Rail predictive-maintenance example** (`examples/ai/rail-predictive-maintenance/`):
  train-mounted rail-condition survey — ICM-42670 vibration → DSP feature
  extraction (`rail_features`: RMS/crest/kurtosis/FFT band energies/dominant
  frequency/rail wavelength) → AI classifier via `<alp/inference.h>` with a
  deterministic fallback → geotagged to GNSS lat/lon + haversine chainage
  (`rail_position`) → one CSV record per 25 m segment. The two pure-C DSP cores
  are host-unit-tested on `native_sim` (`tests/unit/rail_features`,
  `tests/unit/rail_position`); HiL bench-gated.
```

- [ ] **Step 9: Build + run the gate**

Run the twister command from Global Constraints with BOTH testsuite-roots (`tests/unit` AND `examples`).
Expected:
- `alp.unit.rail_features` PASS, `alp.unit.rail_position` PASS.
- `alp_sdk.example.rail_predictive_maintenance.e1m_evk` PASS on `native_sim/native/64` (console matched `[rail] done`).
- The AEN cross-build (`ensemble_e8_dk/...`) builds clean (build_only).
Read results from `/tmp/tw-rail/twister.json` (no pipe). If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (see Step 5 implementer notes) — do NOT change the portable-API contract.

- [ ] **Step 10: Format + commit**

Format all new C (`examples/**` + the cores already formatted). Then:

```bash
git add examples/ai/rail-predictive-maintenance CHANGELOG.md
git commit -m "feat(rail): rail predictive-maintenance example app (DSP+AI+geotag) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 rail_features → Task 1; fallback classifier (C2) → Task 2; C3 rail_position + RMC → Task 3; C4 main.c glue + AI dispatch + output record + scaffolding + README → Task 4. Defect taxonomy → Task 2 enum + Task 4 README. Validation (two ztest suites + native_sim run) → Tasks 1/2/3 tests + Task 4 Step 9. Platform targets (AEN primary, V2N flip, native_sim) → Task 4 board.yaml + testcase.yaml. All spec sections map to a task.

**Type consistency:** `struct rail_feat_state`, `struct rail_features`, `rail_feat_extract(st, odr_hz, speed_mps, out)`, `rail_feat_pack`, `rail_class_t`/`struct rail_verdict`/`rail_classify_fallback`/`rail_class_name`, `struct rail_pos_state`, `rail_pos_init/haversine_m/update/parse_rmc` — names + signatures identical across the header, impl, tests, and main.c. `RAIL_FEATURE_DIM = 13` used consistently. Output schema (10 columns) identical in main.c, README, spec.

**Placeholder scan:** the only intentional placeholder is the Task 1 Step 2 "bait" header (explicitly flagged, replaced in Step 5 to drive a real RED). No "TBD"/"handle edge cases"/"similar to" — every code step carries complete code.
