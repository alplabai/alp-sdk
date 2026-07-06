# Wearable Activity + Fall Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A body-worn IMU edge node that classifies activity (idle/walk/run/stairs) with a small NPU classifier and detects falls with a reliable rule-based 3-phase detector.

**Architecture:** Two pure-C, arch-neutral, host-unit-tested cores — `motion_features` (windowed accel+gyro features + a deterministic idle/walk/run fallback) and `fall_detect` (a per-sample 3-phase free-fall→impact→stillness state machine) — plus an `<alp/inference.h>` activity classifier (stub + fallback) and a thin Zephyr `main.c`. Single 100 Hz IMU stream feeds the per-sample fall detector and the windowed feature path.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/chips/icm42670.h>` (accel+gyro, I2C), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Cores (`motion_features.{c,h}`, `fall_detect.{c,h}`) are pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55. A `#ifndef M_PI` fallback near the top of any core that uses `M_PI`.
- App peripherals via portable `<alp/*>` APIs only (I2C via the `icm42670_*` chip driver, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `MOT_WINDOW_N 256`, `MOT_SR_HZ 100.0f`, `MOT_FEATURE_DIM 12`, `ACT_CLASS_COUNT 4`; `FALL_FREEFALL_G 0.5f`, `FALL_IMPACT_G 2.5f`. Accel full-scale ±16 g = **2048 LSB/g** (`ICM42670_ACCEL_FS_16G`); gyro ±2000 dps = **16.4 LSB/dps**; ODR 100 Hz (`ICM42670_ODR_100_HZ`). IMU I2C addr `ICM42670_I2C_ADDR_HIGH` (0x69, E1M EVK strap).
- TDD: each core RED-first, host-validated on `native_sim/native/64`. Sensor I/O + the AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude`; NO binaries (model is a 1-byte stub; training recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- Example dir: `examples/ai/wearable-activity-fall/`. Primary target E1M-AEN; V2N via `som.sku` flip. Cross-EVK `icm42670` (populated on both EVKs).
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT `/usr/bin/clang-format-14`).
- Unit tests compile each core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists (for `M_PI` on the host) — same pattern as the rail/acoustic examples. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-wact/twister.json`):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-wact'
  ```

---

## File Structure

- `examples/ai/wearable-activity-fall/src/motion_features.{c,h}` — windowed features + activity fallback (Tasks 1-2).
- `examples/ai/wearable-activity-fall/src/fall_detect.{c,h}` — 3-phase fall state machine (Task 3).
- `examples/ai/wearable-activity-fall/src/main.c` — Zephyr glue (Task 4).
- `examples/ai/wearable-activity-fall/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.{conf,overlay}` + `models/README.md` (Task 4).
- `tests/unit/motion_features/`, `tests/unit/fall_detect/` — ztest suites (Tasks 1-3).
- `CHANGELOG.md` — entry (Task 4).

---

### Task 1: `motion_features` — windowed accel+gyro feature extraction + host tests

**Files:**
- Create: `examples/ai/wearable-activity-fall/src/motion_features.h`
- Create: `examples/ai/wearable-activity-fall/src/motion_features.c`
- Create: `tests/unit/motion_features/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_motion_features.c}`

**Interfaces:**
- Produces (Tasks 2/4): `MOT_WINDOW_N 256`, `MOT_SR_HZ 100.0f`, `MOT_FEATURE_DIM 12`; `struct mot_sample`; `struct mot_window_state`; `struct mot_features`; `mot_window_reset/push/full`; `mot_feat_extract`; `mot_feat_pack`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/motion_features/src/test_motion_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for motion_features (windowed IMU DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "motion_features.h"

ZTEST_SUITE(motion_features, NULL, NULL, NULL, NULL, NULL);

/* Fill a window: gravity on Z plus a vertical bounce at freq_hz with the given
 * accel amplitude (g); gyro left near zero. */
static void fill_gait(struct mot_window_state *st, float freq_hz, float amp_g)
{
	mot_window_reset(st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		float t = (float)i / MOT_SR_HZ;
		struct mot_sample s = {
		    .ax = 0.03f * sinf(2.0f * (float)M_PI * freq_hz * t),
		    .ay = 0.03f * sinf(2.0f * (float)M_PI * freq_hz * t + 1.0f),
		    .az = 1.0f + amp_g * sinf(2.0f * (float)M_PI * freq_hz * t),
		    .gx = 0.0f, .gy = 0.0f, .gz = 0.0f,
		};
		mot_window_push(st, s);
	}
}

ZTEST(motion_features, test_fill_and_pack_dim)
{
	struct mot_window_state st;
	struct mot_features     f;
	float                   vec[MOT_FEATURE_DIM];

	mot_window_reset(&st);
	zassert_false(mot_window_full(&st), "empty window not full");
	fill_gait(&st, 2.0f, 0.3f);
	zassert_true(mot_window_full(&st), "full window reports full");

	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_feat_pack(&f, vec, MOT_FEATURE_DIM), (size_t)MOT_FEATURE_DIM,
	              "pack writes MOT_FEATURE_DIM");
}

ZTEST(motion_features, test_walk_dominant_frequency)
{
	struct mot_window_state st;
	struct mot_features     f;

	fill_gait(&st, 2.0f, 0.3f); /* 2 Hz step cadence */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_within((double)f.dom_freq_hz, 2.0, 0.6, "walk cadence ~2 Hz");
	zassert_true(f.amag_rms > 0.05f, "moving window has nonzero AC magnitude");
}

ZTEST(motion_features, test_idle_is_low_energy)
{
	struct mot_window_state st;
	struct mot_features     f;

	mot_window_reset(&st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		struct mot_sample s = { .ax = 0.0f, .ay = 0.0f, .az = 1.0f,
		                        .gx = 0.0f, .gy = 0.0f, .gz = 0.0f };
		mot_window_push(&st, s);
	}
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_true(f.amag_rms < 0.02f, "idle window has near-zero AC magnitude");
	zassert_within((double)f.tilt_deg, 0.0, 5.0, "Z-up gravity -> ~0 deg tilt");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/motion_features/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_motion_features)

set(MOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/wearable-activity-fall/src)
target_include_directories(app PRIVATE ${MOT_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_motion_features.c
    ${MOT_SRC}/motion_features.c
)
```

Create `tests/unit/motion_features/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/motion_features/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.motion_features:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - imu
      - wearable
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.motion_features` build failure (`motion_features.h`/`.c` missing).

- [ ] **Step 4: Write the header**

Create `examples/ai/wearable-activity-fall/src/motion_features.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features -- pure-C windowed accel+gyro feature extraction for the
 * wearable activity / fall example.  Arch-neutral (stdint/math only): builds
 * for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef MOTION_FEATURES_H
#define MOTION_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOT_WINDOW_N    256
#define MOT_SR_HZ       100.0f
/** 3 accel-axis RMS + 3 gyro-axis RMS + amag_rms + gmag_rms + sma + dom_freq +
 *  jerk_rms + tilt_deg = 12. */
#define MOT_FEATURE_DIM 12

/** One IMU sample: accel in g, gyro in deg/s. */
struct mot_sample {
	float ax, ay, az;
	float gx, gy, gz;
};

struct mot_window_state {
	struct mot_sample s[MOT_WINDOW_N];
	uint16_t          count;
};

struct mot_features {
	float a_rms[3];     /**< per-axis accel AC RMS. */
	float g_rms[3];     /**< per-axis gyro AC RMS. */
	float amag_rms;     /**< RMS of |a| (DC removed). */
	float gmag_rms;     /**< RMS of |gyro| (DC removed). */
	float sma;          /**< signal-magnitude-area: mean(|ax|+|ay|+|az|). */
	float dom_freq_hz;  /**< dominant FFT bin of |a| (step cadence). */
	float jerk_rms;     /**< RMS of d|a|/dt. */
	float tilt_deg;     /**< tilt of the mean accel vector from vertical. */
};

void   mot_window_reset(struct mot_window_state *st);
void   mot_window_push(struct mot_window_state *st, struct mot_sample s);
bool   mot_window_full(const struct mot_window_state *st);
void   mot_feat_extract(const struct mot_window_state *st, float sr_hz,
                        struct mot_features *out);
size_t mot_feat_pack(const struct mot_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_FEATURES_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/ai/wearable-activity-fall/src/motion_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features implementation -- see motion_features.h.
 */
#include "motion_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mot_window_reset(struct mot_window_state *st)
{
	st->count = 0;
}

void mot_window_push(struct mot_window_state *st, struct mot_sample s)
{
	if (st->count < MOT_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool mot_window_full(const struct mot_window_state *st)
{
	return st->count >= MOT_WINDOW_N;
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

void mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out)
{
	const int n = (st->count < MOT_WINDOW_N) ? st->count : MOT_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* Per-axis means (accel + gyro) and |a| series. */
	float mean_a[3] = { 0, 0, 0 }, mean_g[3] = { 0, 0, 0 };
	static float amag[MOT_WINDOW_N];
	float        mean_amag = 0.0f, sma = 0.0f;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		mean_a[0] += s->ax; mean_a[1] += s->ay; mean_a[2] += s->az;
		mean_g[0] += s->gx; mean_g[1] += s->gy; mean_g[2] += s->gz;
		amag[i] = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
		mean_amag += amag[i];
		sma += fabsf(s->ax) + fabsf(s->ay) + fabsf(s->az);
	}
	for (int k = 0; k < 3; k++) {
		mean_a[k] /= (float)n;
		mean_g[k] /= (float)n;
	}
	mean_amag /= (float)n;
	out->sma = sma / (float)n;

	/* Per-axis AC RMS (accel + gyro), |gyro| RMS, |a| AC RMS, jerk RMS. */
	float sa[3] = { 0, 0, 0 }, sg[3] = { 0, 0, 0 };
	float s_amag = 0.0f, s_gmag = 0.0f, mean_gmag = 0.0f;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		float da[3] = { s->ax - mean_a[0], s->ay - mean_a[1], s->az - mean_a[2] };
		float dg[3] = { s->gx - mean_g[0], s->gy - mean_g[1], s->gz - mean_g[2] };
		for (int k = 0; k < 3; k++) {
			sa[k] += da[k] * da[k];
			sg[k] += dg[k] * dg[k];
		}
		float dm = amag[i] - mean_amag;
		s_amag += dm * dm;
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz);
		mean_gmag += gm;
	}
	mean_gmag /= (float)n;
	for (int i = 0; i < n; i++) {
		const struct mot_sample *s = &st->s[i];
		float gm = sqrtf(s->gx * s->gx + s->gy * s->gy + s->gz * s->gz) - mean_gmag;
		s_gmag += gm * gm;
	}
	for (int k = 0; k < 3; k++) {
		out->a_rms[k] = sqrtf(sa[k] / (float)n);
		out->g_rms[k] = sqrtf(sg[k] / (float)n);
	}
	out->amag_rms = sqrtf(s_amag / (float)n);
	out->gmag_rms = sqrtf(s_gmag / (float)n);

	float s_jerk = 0.0f;
	for (int i = 1; i < n; i++) {
		float d = (amag[i] - amag[i - 1]) * sr_hz; /* per-second jerk */
		s_jerk += d * d;
	}
	out->jerk_rms = (n > 1) ? sqrtf(s_jerk / (float)(n - 1)) : 0.0f;

	/* Tilt of the mean accel vector from vertical (Z). */
	out->tilt_deg = atan2f(sqrtf(mean_a[0] * mean_a[0] + mean_a[1] * mean_a[1]), mean_a[2]) *
	                180.0f / (float)M_PI;

	/* Dominant frequency of the |a| envelope via FFT (DC removed). */
	static float re[MOT_WINDOW_N];
	static float im[MOT_WINDOW_N];
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		re[i] = (i < n) ? (amag[i] - mean_amag) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, MOT_WINDOW_N);
	const int half = MOT_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = re[k] * re[k] + im[k] * im[k];
		if (m2 > dom_val) {
			dom_val = m2;
			dom_bin = k;
		}
	}
	out->dom_freq_hz = (float)dom_bin * sr_hz / (float)MOT_WINDOW_N;
}

size_t mot_feat_pack(const struct mot_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)MOT_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int k = 0; k < 3; k++) {
		vec[i++] = f->a_rms[k];
	}
	for (int k = 0; k < 3; k++) {
		vec[i++] = f->g_rms[k];
	}
	vec[i++] = f->amag_rms;
	vec[i++] = f->gmag_rms;
	vec[i++] = f->sma;
	vec[i++] = f->dom_freq_hz;
	vec[i++] = f->jerk_rms;
	vec[i++] = f->tilt_deg;
	return i; /* == MOT_FEATURE_DIM */
}
```

- [ ] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.motion_features` PASS, 3/3. If `test_walk_dominant_frequency` misses, check the bin→Hz map (2 Hz at 100/256 → bin 5 → 1.95 Hz, within 0.6).

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/ai/wearable-activity-fall/src/motion_features.h \
        examples/ai/wearable-activity-fall/src/motion_features.c \
        tests/unit/motion_features
git commit -m "feat(wact): motion_features windowed IMU DSP (RMS/SMA/cadence/jerk/tilt) + ztest"
```

---

### Task 2: `motion_features` — deterministic activity fallback + host tests

**Files:**
- Modify: `examples/ai/wearable-activity-fall/src/motion_features.{h,c}`
- Modify: `tests/unit/motion_features/src/test_motion_features.c`

**Interfaces:**
- Consumes: `struct mot_features` (Task 1).
- Produces (Task 4): `typedef enum { ACT_IDLE=0, ACT_WALK=1, ACT_RUN=2, ACT_STAIRS=3, ACT_CLASS_COUNT } mot_activity_t;` `struct mot_verdict { mot_activity_t cls; float confidence; };` `struct mot_verdict mot_activity_fallback(const struct mot_features *f);` `const char *mot_activity_name(mot_activity_t c);`

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/motion_features/src/test_motion_features.c`:

```c
ZTEST(motion_features, test_classify_idle)
{
	struct mot_window_state st;
	struct mot_features     f;
	mot_window_reset(&st);
	for (int i = 0; i < MOT_WINDOW_N; i++) {
		struct mot_sample s = { .ax = 0.0f, .ay = 0.0f, .az = 1.0f };
		mot_window_push(&st, s);
	}
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_IDLE, "still -> IDLE");
}

ZTEST(motion_features, test_classify_walk_vs_run)
{
	struct mot_window_state st;
	struct mot_features     f;

	fill_gait(&st, 2.0f, 0.3f); /* 2 Hz, modest amplitude -> WALK */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_WALK, "2 Hz modest -> WALK");

	fill_gait(&st, 3.0f, 1.2f); /* 3 Hz, large amplitude -> RUN */
	mot_feat_extract(&st, MOT_SR_HZ, &f);
	zassert_equal(mot_activity_fallback(&f).cls, ACT_RUN, "3 Hz strong -> RUN");
}

ZTEST(motion_features, test_activity_name)
{
	zassert_true(strcmp(mot_activity_name(ACT_IDLE), "IDLE") == 0, "name");
	zassert_true(strcmp(mot_activity_name(ACT_RUN), "RUN") == 0, "name");
	zassert_true(strcmp(mot_activity_name(ACT_STAIRS), "STAIRS") == 0, "name");
}
```

Add `#include <string.h>` at the top of the test if not already present.

- [ ] **Step 2: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: build failure — `mot_activity_fallback`/`mot_verdict`/`mot_activity_name` undeclared.

- [ ] **Step 3: Add the API to the header**

Insert into `motion_features.h` before the closing `#ifdef __cplusplus }`:

```c
/** Coarse activity classes.  STAIRS is an AI-only class -- the deterministic
 *  fallback cannot separate it from WALK without a barometer. */
typedef enum {
	ACT_IDLE   = 0,
	ACT_WALK   = 1,
	ACT_RUN    = 2,
	ACT_STAIRS = 3,
	ACT_CLASS_COUNT
} mot_activity_t;

struct mot_verdict {
	mot_activity_t cls;
	float          confidence; /**< 0..1 */
};

/** Deterministic idle/walk/run classifier over the feature vector.  Runs when no
 *  AI model is loaded.  Never emits STAIRS (maps that case to WALK). */
struct mot_verdict mot_activity_fallback(const struct mot_features *f);

/** Stable upper-case class name for the record. */
const char *mot_activity_name(mot_activity_t c);
```

- [ ] **Step 4: Implement**

Append to `motion_features.c`:

```c
struct mot_verdict mot_activity_fallback(const struct mot_features *f)
{
	struct mot_verdict v = { ACT_IDLE, 0.0f };

	if (f->amag_rms < 0.05f) {
		v.cls        = ACT_IDLE;
		v.confidence = 0.8f;
	} else if (f->dom_freq_hz > 2.5f && f->amag_rms > 0.6f) {
		v.cls        = ACT_RUN;
		v.confidence = fminf(1.0f, f->amag_rms);
	} else {
		/* Moving but not running -> WALK (covers stairs too; the model splits). */
		v.cls        = ACT_WALK;
		v.confidence = 0.7f;
	}
	return v;
}

const char *mot_activity_name(mot_activity_t c)
{
	switch (c) {
	case ACT_IDLE:
		return "IDLE";
	case ACT_WALK:
		return "WALK";
	case ACT_RUN:
		return "RUN";
	case ACT_STAIRS:
		return "STAIRS";
	default:
		return "UNKNOWN";
	}
}
```

`fminf` needs `<math.h>` (already included in Task 1).

- [ ] **Step 5: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.motion_features` PASS, 6/6. If `test_classify_walk_vs_run` misses RUN, confirm the 3 Hz/1.2 g window gives `amag_rms > 0.6` and `dom_freq_hz > 2.5` (3 Hz → bin 7.68 → bin 8 → 3.125 Hz); tune the synthetic amplitude, not the thresholds, if needed.

- [ ] **Step 6: Format + commit**

```bash
git add examples/ai/wearable-activity-fall/src/motion_features.h \
        examples/ai/wearable-activity-fall/src/motion_features.c \
        tests/unit/motion_features/src/test_motion_features.c
git commit -m "feat(wact): deterministic idle/walk/run activity fallback + tests"
```

---

### Task 3: `fall_detect` — 3-phase fall state machine + host tests

**Files:**
- Create: `examples/ai/wearable-activity-fall/src/fall_detect.{h,c}`
- Create: `tests/unit/fall_detect/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_fall_detect.c}`

**Interfaces:**
- Produces (Task 4): `FALL_FREEFALL_G 0.5f`, `FALL_IMPACT_G 2.5f`; `struct fall_state`; `void fall_reset(struct fall_state *)`; `bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out)`; `bool fall_is_armed(const struct fall_state *)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/fall_detect/src/test_fall_detect.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for fall_detect (3-phase fall state machine) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "fall_detect.h"

ZTEST_SUITE(fall_detect, NULL, NULL, NULL, NULL, NULL);

#define SR 100.0f

/* Push a run of `count` samples at constant magnitude; returns true if a fall
 * fired during the run, capturing impact_g. */
static bool push_const(struct fall_state *st, float g, int count, float *impact)
{
	bool fired = false;
	for (int i = 0; i < count; i++) {
		float ig = 0.0f;
		if (fall_push(st, g, SR, &ig)) {
			fired   = true;
			*impact = ig;
		}
	}
	return fired;
}

ZTEST(fall_detect, test_canonical_fall_fires)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;

	push_const(&st, 1.0f, 30, &impact);          /* normal */
	push_const(&st, 0.2f, 15, &impact);          /* free-fall (>= 8 samples) */
	bool fired_impact = push_const(&st, 5.0f, 2, &impact); /* impact spike */
	bool fired_still  = push_const(&st, 1.0f, 120, &impact); /* >=1 s stillness */

	zassert_true(fired_impact || fired_still, "a 3-phase fall is detected");
	zassert_within((double)impact, 5.0, 0.5, "impact_g captured ~5 g");
}

ZTEST(fall_detect, test_walk_never_fires)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	bool  fired  = false;
	for (int i = 0; i < 1000; i++) {
		float g  = 1.0f + 0.6f * sinf(2.0f * (float)M_PI * 2.0f * (float)i / SR);
		float ig = 0.0f;
		if (fall_push(&st, g, SR, &ig)) {
			fired = true;
		}
		(void)impact;
	}
	zassert_false(fired, "ordinary walking never triggers a fall");
}

ZTEST(fall_detect, test_impact_without_freefall_does_not_fire)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	/* Hard sit-down: no free-fall, just a 3 g bump then settle. */
	push_const(&st, 1.0f, 30, &impact);
	bool a = push_const(&st, 3.0f, 2, &impact);
	bool b = push_const(&st, 1.0f, 120, &impact);
	zassert_false(a || b, "impact with no preceding free-fall is not a fall");
}

ZTEST(fall_detect, test_freefall_without_impact_does_not_fire)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	push_const(&st, 1.0f, 30, &impact);
	push_const(&st, 0.2f, 15, &impact);          /* free-fall ... */
	bool fired = push_const(&st, 1.0f, 200, &impact); /* ... then normal, no impact */
	zassert_false(fired, "free-fall with no impact is not a fall");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/fall_detect/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_fall_detect)

set(MOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/wearable-activity-fall/src)
target_include_directories(app PRIVATE ${MOT_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_fall_detect.c
    ${MOT_SRC}/fall_detect.c
)
```

Create `tests/unit/fall_detect/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/fall_detect/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.fall_detect:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - imu
      - wearable
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.fall_detect` build failure.

- [ ] **Step 4: Write the header**

Create `examples/ai/wearable-activity-fall/src/fall_detect.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect -- pure-C 3-phase fall detector (free-fall -> impact ->
 * post-impact stillness) as a per-sample state machine.  Rule-based, no model
 * (well-understood physics).  Arch-neutral; host-unit-tested.
 */
#ifndef FALL_DETECT_H
#define FALL_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FALL_FREEFALL_G 0.5f /**< |a| below this = free-fall. */
#define FALL_IMPACT_G   2.5f /**< |a| above this = impact. */

enum fall_phase {
	FALL_PHASE_NORMAL = 0,
	FALL_PHASE_FREEFALL,
	FALL_PHASE_WAIT_IMPACT,
	FALL_PHASE_POST_IMPACT,
};

struct fall_state {
	enum fall_phase phase;
	uint16_t        ff_count;     /**< consecutive free-fall samples. */
	uint16_t        wait_count;   /**< samples since free-fall ended. */
	uint16_t        post_count;   /**< samples since impact. */
	uint16_t        still_count;  /**< consecutive near-1g samples. */
	float           impact_g;     /**< peak |a| of the impact. */
};

/** Reset to the NORMAL phase. */
void fall_reset(struct fall_state *st);

/**
 * Feed one accel-magnitude sample (g).  Returns true exactly on the sample that
 * confirms a fall (free-fall >= ~80 ms -> impact > FALL_IMPACT_G within ~300 ms
 * -> post-impact stillness ~1 s), writing the peak impact to @p impact_g_out.
 * Self-resets after a confirmed fall or a phase timeout.  @p sr_hz sets the
 * sample-count windows.
 */
bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out);

/** True when a free-fall/impact sequence is in progress (telemetry/tests). */
bool fall_is_armed(const struct fall_state *st);

#ifdef __cplusplus
}
#endif

#endif /* FALL_DETECT_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/ai/wearable-activity-fall/src/fall_detect.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect implementation -- see fall_detect.h.
 */
#include "fall_detect.h"

#include <math.h>

void fall_reset(struct fall_state *st)
{
	st->phase       = FALL_PHASE_NORMAL;
	st->ff_count    = 0;
	st->wait_count  = 0;
	st->post_count  = 0;
	st->still_count = 0;
	st->impact_g    = 0.0f;
}

bool fall_is_armed(const struct fall_state *st)
{
	return st->phase != FALL_PHASE_NORMAL;
}

bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out)
{
	/* Phase windows in samples, derived from the sample rate. */
	const uint16_t ff_min      = (uint16_t)(0.08f * sr_hz); /* >= ~80 ms free-fall */
	const uint16_t impact_win  = (uint16_t)(0.30f * sr_hz); /* impact within ~300 ms */
	const uint16_t still_min   = (uint16_t)(1.00f * sr_hz); /* ~1 s stillness */
	const uint16_t post_max    = (uint16_t)(3.00f * sr_hz); /* give up after ~3 s */

	switch (st->phase) {
	case FALL_PHASE_NORMAL:
		if (amag_g < FALL_FREEFALL_G) {
			st->phase    = FALL_PHASE_FREEFALL;
			st->ff_count = 1;
		}
		break;

	case FALL_PHASE_FREEFALL:
		if (amag_g < FALL_FREEFALL_G) {
			st->ff_count++;
		} else if (st->ff_count >= ff_min) {
			/* Valid free-fall ended; look for the impact. */
			st->phase      = FALL_PHASE_WAIT_IMPACT;
			st->wait_count = 0;
			st->impact_g   = 0.0f;
			/* This same sample may already be the impact. */
			if (amag_g > FALL_IMPACT_G) {
				st->impact_g   = amag_g;
				st->phase      = FALL_PHASE_POST_IMPACT;
				st->post_count = 0;
				st->still_count = 0;
			}
		} else {
			st->phase = FALL_PHASE_NORMAL; /* free-fall too short */
		}
		break;

	case FALL_PHASE_WAIT_IMPACT:
		st->wait_count++;
		if (amag_g > FALL_IMPACT_G) {
			st->impact_g    = amag_g;
			st->phase       = FALL_PHASE_POST_IMPACT;
			st->post_count  = 0;
			st->still_count = 0;
		} else if (st->wait_count > impact_win) {
			st->phase = FALL_PHASE_NORMAL; /* no impact after free-fall */
		}
		break;

	case FALL_PHASE_POST_IMPACT:
		st->post_count++;
		if (amag_g > st->impact_g) {
			st->impact_g = amag_g; /* track the spike peak. */
		}
		if (fabsf(amag_g - 1.0f) < 0.3f) {
			st->still_count++;
		} else {
			st->still_count = 0;
		}
		if (st->still_count >= still_min) {
			float impact = st->impact_g;
			fall_reset(st);
			if (impact_g_out != NULL) {
				*impact_g_out = impact;
			}
			return true; /* confirmed fall */
		}
		if (st->post_count > post_max) {
			fall_reset(st); /* never settled -> not a fall */
		}
		break;
	}
	return false;
}
```

- [ ] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.fall_detect` PASS, 4/4. If the canonical fall doesn't fire, trace the phase transitions (free-fall 15 samples ≥ ff_min 8 → impact 5 g → 120 still samples ≥ still_min 100).

- [ ] **Step 7: Format + commit**

```bash
git add examples/ai/wearable-activity-fall/src/fall_detect.h \
        examples/ai/wearable-activity-fall/src/fall_detect.c \
        tests/unit/fall_detect
git commit -m "feat(wact): fall_detect 3-phase state machine (freefall/impact/stillness) + tests"
```

---

### Task 4: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/ai/wearable-activity-fall/src/main.c`
- Create: `examples/ai/wearable-activity-fall/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/ai/wearable-activity-fall/boards/native_sim_native_64.{conf,overlay}`
- Create: `examples/ai/wearable-activity-fall/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `motion_features.h` + `fall_detect.h`; portable `<alp/peripheral.h>` (I2C), `<alp/inference.h>`, `<alp/board.h>`, `<alp/chips/icm42670.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `WACT,...` record per window, ending `[wact] done`.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/ai/wearable-activity-fall/CMakeLists.txt`:
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
project(wearable_activity_fall LANGUAGES C)

target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/main.c
    src/motion_features.c
    src/fall_detect.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/wearable-activity-fall/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml** (mirror the vibration-anomaly example: ICM42670 on I2C0)

Create `examples/ai/wearable-activity-fall/board.yaml`:
```yaml
# board.yaml -- wearable activity recognition + fall detection.
#
# A body-worn M55 node reads the on-board ICM-42670 6-axis IMU, classifies
# coarse activity (idle/walk/run/stairs) with a small NPU model, and detects
# falls with a rule-based 3-phase detector.  Same source targets the V2N DEEPX
# path when som.sku is flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

pins:
  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS, doc: "ICM-42670 IMU bus" }

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

chips:
  - icm42670                  # 6-axis IMU; accel + gyro.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim overlay + conf**

Create `examples/ai/wearable-activity-fall/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# native_sim has no real I2C controller; pull in the emul drivers so the
# ICM-42670 chip driver can open the sensor bus at boot.  No emul target is
# attached, so main.c falls back to synthetic motion.
CONFIG_EMUL=y
CONFIG_I2C_EMUL=y
```

Create `examples/ai/wearable-activity-fall/boards/native_sim_native_64.overlay`:
```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-build overlay -- exposes one emulated I2C controller via the alp-i2c0
 * alias so alp_i2c_open(BOARD_I2C_SENSORS) resolves and the ICM-42670 bring-up
 * runs.  No emul target is attached, so the WHO_AM_I read fails and main.c
 * falls back to synthetic motion.  On real silicon this file is NOT applied.
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

Create `examples/ai/wearable-activity-fall/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * wearable-activity-fall
 * ======================
 *
 * Body-worn IMU edge node.  Pipeline:
 *
 *   ICM-42670 accel+gyro (I2C, 100 Hz, +/-16 g) --every sample-->
 *     fall_detect (3-phase free-fall -> impact -> stillness state machine)
 *   ICM-42670 --256-sample window (2.56 s)-->
 *     motion_features (RMS/SMA/cadence/jerk/tilt) -> <alp/inference.h>
 *       activity classifier (idle/walk/run/stairs) + deterministic fallback
 *     --> one WACT record per window; falls flagged immediately.
 *
 * Honest scope: body-worn motion; coarse activity + fall detection.  NOT
 * medical-grade, not a certified fall alarm.  The model is a stub (see
 * models/README.md); with no model the deterministic fallback runs.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"

#include "fall_detect.h"
#include "motion_features.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(wact, LOG_LEVEL_INF);

#define ICM_ACCEL_LSB_PER_G 2048.0f /* +/-16 g full-scale. */
#define ICM_GYRO_LSB_PER_DPS 16.4f  /* +/-2000 dps full-scale. */
#define IMU_I2C_ADDR        ICM42670_I2C_ADDR_HIGH
#define N_WINDOWS           8

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Synthetic motion for native_sim: one window per activity, plus an injected
 * fall in window 5. */
static struct mot_sample synth_sample(int window, int i)
{
	float t = (float)i / MOT_SR_HZ;
	struct mot_sample s = { .ax = 0.0f, .ay = 0.0f, .az = 1.0f,
	                        .gx = 0.0f, .gy = 0.0f, .gz = 0.0f };
	switch (window % 4) {
	case 0: /* idle */
		break;
	case 1: /* walk ~2 Hz */
		s.az = 1.0f + 0.3f * sinf(2.0f * (float)M_PI * 2.0f * t);
		s.gx = 10.0f * sinf(2.0f * (float)M_PI * 2.0f * t);
		break;
	case 2: /* run ~3 Hz */
		s.az = 1.0f + 1.2f * sinf(2.0f * (float)M_PI * 3.0f * t);
		s.gx = 40.0f * sinf(2.0f * (float)M_PI * 3.0f * t);
		break;
	default: /* stairs-ish: walk cadence + tilt */
		s.az = 0.9f + 0.3f * sinf(2.0f * (float)M_PI * 1.8f * t);
		s.ax = 0.3f;
		break;
	}
	return s;
}

/* Overlay a free-fall -> impact -> stillness sequence on window 5's |a|. */
static float synth_fall_amag(int i)
{
	if (i < 20) {
		return 1.0f; /* normal */
	} else if (i < 35) {
		return 0.2f; /* free-fall (15 samples) */
	} else if (i < 37) {
		return 5.0f; /* impact */
	}
	return 1.0f; /* stillness */
}

static struct mot_verdict classify(alp_inference_t *inf, const struct mot_features *f)
{
	if (inf != NULL) {
		float vec[MOT_FEATURE_DIM];
		(void)mot_feat_pack(f, vec, MOT_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= ACT_CLASS_COUNT * sizeof(float)) {
					const float *sc = (const float *)out.data;
					int   best = 0;
					float bv   = sc[0];
					for (int k = 1; k < ACT_CLASS_COUNT; k++) {
						if (sc[k] > bv) {
							bv   = sc[k];
							best = k;
						}
					}
					return (struct mot_verdict){ (mot_activity_t)best, bv };
				}
			}
		}
	}
	return mot_activity_fallback(f);
}

int main(void)
{
	static icm42670_t   imu;
	static struct mot_window_state win;
	static struct fall_state       fall;
	bool imu_ok = false;

	fall_reset(&fall);

	alp_i2c_t *bus = alp_i2c_open(BOARD_I2C_SENSORS);
	if (bus != NULL && icm42670_init(&imu, bus, IMU_I2C_ADDR) == ALP_OK &&
	    icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_16G) == ALP_OK &&
	    icm42670_set_gyro(&imu, ICM42670_ODR_100_HZ, ICM42670_GYRO_FS_2000_DPS) == ALP_OK) {
		imu_ok = true;
	} else {
		LOG_WRN("ICM-42670 unavailable; using synthetic motion");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# WACT,t_s,activity,confidence,fall,impact_g\n");

	for (int w = 0; w < N_WINDOWS; w++) {
		mot_window_reset(&win);
		bool  fall_fired   = false;
		float impact_g     = 0.0f;

		for (int i = 0; i < MOT_WINDOW_N; i++) {
			struct mot_sample s;
			if (imu_ok) {
				icm42670_axes_t a, g;
				if (icm42670_read_accel(&imu, &a) == ALP_OK &&
				    icm42670_read_gyro(&imu, &g) == ALP_OK) {
					s.ax = (float)a.x / ICM_ACCEL_LSB_PER_G;
					s.ay = (float)a.y / ICM_ACCEL_LSB_PER_G;
					s.az = (float)a.z / ICM_ACCEL_LSB_PER_G;
					s.gx = (float)g.x / ICM_GYRO_LSB_PER_DPS;
					s.gy = (float)g.y / ICM_GYRO_LSB_PER_DPS;
					s.gz = (float)g.z / ICM_GYRO_LSB_PER_DPS;
				} else {
					s = synth_sample(w, i);
				}
			} else {
				s = synth_sample(w, i);
			}

			/* Window 5 (native_sim) overlays a fall on the |a| magnitude. */
			float amag;
			if (!imu_ok && w == 5) {
				amag = synth_fall_amag(i);
				s.az = amag; /* drive the window features too */
				s.ax = 0.0f;
				s.ay = 0.0f;
			} else {
				amag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
			}

			mot_window_push(&win, s);

			float ig = 0.0f;
			if (fall_push(&fall, amag, MOT_SR_HZ, &ig)) {
				fall_fired = true;
				impact_g   = ig;
			}
		}

		struct mot_features f;
		mot_feat_extract(&win, MOT_SR_HZ, &f);
		struct mot_verdict v = classify(inf, &f);

		printk("WACT,%.2f,%s,%.2f,%d,%.1f\n", (double)(w * 2.56f),
		       mot_activity_name(v.cls), (double)v.confidence, fall_fired ? 1 : 0,
		       (double)impact_g);
	}

	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (imu_ok) {
		icm42670_deinit(&imu);
	}
	printk("[wact] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers (as the rail/acoustic examples did) — `alp_i2c_open(BOARD_I2C_SENSORS)` per `include/alp/peripheral.h` (it may take an `alp_i2c_config_t*` — mirror `examples/ai/ai-anomaly-detection-vibration/src/main.c`); `icm42670_init/set_accel/set_gyro/read_accel/read_gyro/deinit` + `icm42670_axes_t` (int16 `.x/.y/.z`) per `include/alp/chips/icm42670.h`; the `alp_inference_*` calls per the vibration/wake-word examples. If `alp_i2c_open` needs a config struct, follow the sibling exactly. Add `<string.h>`/`<math.h>` if a symbol is unresolved. Keep `<alp/*>` portable — no vendor names.

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only` — the rail lesson)

Create `examples/ai/wearable-activity-fall/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: wearable-activity-fall
  description: |
    Body-worn IMU edge node: ICM-42670 accel+gyro -> windowed motion
    features -> activity classifier (idle/walk/run/stairs, deterministic
    fallback) + a rule-based 3-phase fall detector.  native_sim runs
    synthetic motion incl. one injected fall.
common:
  tags: ai inference wearable imu predictive-health marketing showcase
tests:
  alp_sdk.example.wearable_activity_fall.e1m_evk:
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
      - wearable
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[wact\\] done"

  alp_sdk.example.wearable_activity_fall.aen_build:
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - ai
      - wearable
```

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/ai/wearable-activity-fall/models/README.md`:
```markdown
# Activity model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic
idle/walk/run fallback runs without one. The fall detector is rule-based and
needs no model at all. To get the full taxonomy (incl. STAIRS), train a small
**HAR classifier**:

1. **Collect labelled windows** at the device's 100 Hz / 256-sample window, with
   the 12-feature `motion_features` vector as input and the activity label as
   target. Public starting datasets: **UCI-HAR**, **WISDM** (re-window to match).
2. **Train** a small dense or 1D-CNN classifier over the 12-D feature vector
   (4 classes: idle/walk/run/stairs). Keep it tiny — this is an always-on path.
3. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop the result here and point `alp_inference_open` at it.

The fall detector's thresholds (`FALL_FREEFALL_G`, `FALL_IMPACT_G`, the phase
windows) should be tuned per mounting position (wrist vs belt) on real data.

Honest scope: coarse activity + fall detection; NOT medical-grade and not a
certified fall alarm.
```

- [ ] **Step 8: Write README.md**

Create `examples/ai/wearable-activity-fall/README.md`:
```markdown
# wearable-activity-fall

> ⚠️ **`[UNTESTED]` on hardware -- v0.6 paper-correct.** The two cores
> (`motion_features`, `fall_detect`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> synthetic motion (incl. an injected fall). HiL on a real wearable + a trained
> model is bench-gated.

Body-worn IMU edge node: read a 6-axis IMU, classify coarse **activity**
(idle/walk/run/stairs) with a small NPU model, and detect **falls** with a
reliable rule-based 3-phase detector. Targets wearables / elder-care /
lone-worker safety.

## Honest scope

Body-worn motion sensing. Detects falls + coarse activity. **NOT** medical-grade,
not a certified fall alarm, no gait/health diagnostics. The fall detector is a
physics heuristic (tunable thresholds), not a guarantee.

## Pipeline

```
ICM-42670 accel+gyro (I2C, 100 Hz, +/-16 g)
  | every sample -> fall_detect (free-fall -> impact -> stillness)
  | 256-sample window -> motion_features (RMS/SMA/cadence/jerk/tilt)
  |   -> <alp/inference.h> activity classifier (deterministic fallback)
  -> WACT record per window
```

The IMU runs at **+/-16 g** so fall impacts (several g) do not clip.

## Output

```
# WACT,t_s,activity,confidence,fall,impact_g
WACT,2.56,WALK,0.91,0,0.0
WACT,12.80,IDLE,0.74,1,4.8
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/wearable-activity-fall
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the HAR training recipe. The fall detector needs no model.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/motion_features -T tests/unit/fall_detect
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Wearable activity + fall example** (`examples/ai/wearable-activity-fall/`):
  body-worn IMU edge node — ICM-42670 accel+gyro → windowed motion features
  (`motion_features`: per-axis/magnitude RMS, SMA, step cadence via FFT, jerk,
  tilt + a deterministic idle/walk/run fallback) → activity classifier via
  `<alp/inference.h>`, plus a rule-based 3-phase fall detector (`fall_detect`:
  free-fall → impact → post-impact stillness). Two pure-C cores host-unit-tested
  on `native_sim` (`tests/unit/motion_features`, `tests/unit/fall_detect`); model
  is a stub with a training recipe in `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.motion_features` (6/6), `alp.unit.fall_detect` (4/4) PASS.
- `alp_sdk.example.wearable_activity_fall.e1m_evk` PASS on `native_sim/native/64` (console `[wact] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-wact/twister.json`. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 5 notes) — do NOT change the portable-API contract or the cores' logic. The local AEN link env may hit the shared `alp_backends_*` orphan-section issue (same as sibling examples); if AEN fails ONLY with that, note it — CI is the AEN gate.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22, then:
```bash
git add examples/ai/wearable-activity-fall CHANGELOG.md
git commit -m "feat(wact): wearable activity + fall example app (IMU DSP + classifier + fall detector) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 motion_features → Task 1; activity fallback (C1) → Task 2; C2 fall_detect → Task 3; C3 AI dispatch + C4 main.c + scaffolding + models/README + README + CHANGELOG → Task 4. Output record + taxonomy → Task 4 (main.c + README). Validation (two ztest suites + native_sim run) → Tasks 1-3 tests + Task 4 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 4 board.yaml + testcase.yaml. Honest scope → Task 4 README + models/README. All spec sections map to a task. STAIRS is AI-only (fallback never emits it) — matches the corrected spec; no test asserts STAIRS from the fallback.

**Type consistency:** `MOT_WINDOW_N 256`, `MOT_SR_HZ 100.0f`, `MOT_FEATURE_DIM 12`, `ACT_CLASS_COUNT 4` consistent across header/impl/tests/main. `mot_sample/mot_window_state/mot_features/mot_window_reset/push/full/mot_feat_extract/mot_feat_pack/mot_activity_t/mot_verdict/mot_activity_fallback/mot_activity_name`, `fall_state/fall_reset/fall_push/fall_is_armed`, `FALL_FREEFALL_G/FALL_IMPACT_G` — names + signatures identical across tasks. `mot_feat_pack` writes exactly 12 in the documented order. Output schema (6 columns) identical in main.c + README + spec.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The 1-byte model stub + the synthetic generators are deliberate, documented design decisions.
```
