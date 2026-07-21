# Wind-Turbine Acoustic Anomaly Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A nacelle acoustic condition monitor: capture mic audio, extract DSP features, normalize blade-periodic energy to rotor order (RPM-invariant via BPF), and emit a per-interval anomaly score + advisory subsystem/flag for drivetrain tonals and gross blade aero-anomalies.

**Architecture:** Three pure-C, arch-neutral, host-unit-tested cores — `acoustic_features` (per-frame DSP: FFT bands, spectral flatness/centroid, kurtosis, + a healthy-baseline anomaly fallback), `rotor_speed` (tacho + tacholess RPM → BPF), `bpf_modulation` (seconds-long band-energy envelope → Goertzel at BPF harmonics, blade order domain) — plus an `<alp/inference.h>` anomaly model (stub + deterministic fallback) and a thin Zephyr `main.c`.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/audio.h>` (PDM mic), `<alp/peripheral.h>` (GPIO tacho), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Cores (`acoustic_features.{c,h}`, `rotor_speed.{c,h}`, `bpf_modulation.{c,h}`) are pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build for native_sim AND M55. Each core has a `#ifndef M_PI` fallback near the top.
- App peripherals via portable `<alp/*>` APIs only (audio, gpio, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `ACO_FRAME_N 256`, `ACO_SR_HZ 16000.0f`, `ACO_N_BANDS 12`, `ACO_FEATURE_DIM 16`; `BPF_ENV_N 256`, `BPF_N_HARMONICS 4`, `BPF_FEATURE_DIM 5`; `ANOMALY_INPUT_DIM 22`; frame rate = `ACO_SR_HZ/ACO_FRAME_N` = 62.5 fps; default `N_BLADES 3`; `rotor_rpm_valid` range 3..30 rpm.
- TDD: each core RED-first, host-validated on `native_sim/native/64`. The mic I/O, tacho GPIO, and AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude`; NO binaries (model is a 1-byte stub array; the training recipe is docs only); no confidential/OneDrive/local paths; no login-gated vendor links.
- Example dir: `examples/audio/acoustic-anomaly-wind-turbine/`. Primary target E1M-AEN (M55-HE, Ethos-U); V2N via `som.sku` flip.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT `/usr/bin/clang-format-14`).
- The unit tests compile each core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` defined in the test CMakeLists (for `M_PI` on the host) — same pattern as the rail example. `zassert_within` takes `double`; cast `float` args to `(double)` to avoid `-Werror=double-promotion`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-wtac/twister.json`):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-wtac'
  ```

---

## File Structure

- `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.{c,h}` — per-frame DSP + anomaly fallback (Tasks 1-2).
- `examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.{c,h}` — tacho + tacholess RPM + BPF (Task 3).
- `examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.{c,h}` — envelope ring + Goertzel blade-order features (Task 4).
- `examples/audio/acoustic-anomaly-wind-turbine/src/main.c` — Zephyr glue (Task 5).
- `examples/audio/acoustic-anomaly-wind-turbine/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `models/README.md` (Task 5).
- `tests/unit/acoustic_features/`, `tests/unit/rotor_speed/`, `tests/unit/bpf_modulation/` — ztest suites (Tasks 1-4).
- `CHANGELOG.md` — entry (Task 5).

---

### Task 1: `acoustic_features` — per-frame DSP extraction + host tests

**Files:**
- Create: `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.h`
- Create: `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.c`
- Create: `tests/unit/acoustic_features/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_acoustic_features.c}`

**Interfaces:**
- Produces (Tasks 2/5): `ACO_FRAME_N 256`, `ACO_N_BANDS 12`, `ACO_SR_HZ 16000.0f`, `ACO_FEATURE_DIM (ACO_N_BANDS + 4)`; `struct aco_frame_state`; `struct aco_features`; `aco_frame_reset/push/full`; `aco_feat_extract(st, sr_hz, out)`; `aco_feat_pack(f, vec, cap)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/acoustic_features/src/test_acoustic_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for acoustic_features (per-frame DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "acoustic_features.h"

ZTEST_SUITE(acoustic_features, NULL, NULL, NULL, NULL, NULL);

static void fill(struct aco_frame_state *st, float (*gen)(int))
{
	aco_frame_reset(st);
	for (int i = 0; i < ACO_FRAME_N; i++) {
		aco_frame_push(st, gen(i));
	}
}

/* Low-amplitude broadband: three incommensurate tones -> flat-ish spectrum. */
static float gen_lownoise(int i)
{
	return 0.02f * (sinf((float)i * 1.7f) + sinf((float)i * 0.37f) + sinf((float)i * 3.91f));
}

/* A 1 kHz tone at 16 kHz ODR -> single spectral peak. */
static float gen_tone_1k(int i)
{
	return sinf(2.0f * (float)M_PI * 1000.0f * (float)i / ACO_SR_HZ);
}

/* Periodic impulses -> high kurtosis. */
static float gen_impulse(int i)
{
	return (i % 64 == 0) ? 1.0f : 0.0f;
}

ZTEST(acoustic_features, test_fill_and_pack_dim)
{
	struct aco_frame_state st;
	struct aco_features    f;
	float                  vec[ACO_FEATURE_DIM];

	aco_frame_reset(&st);
	zassert_false(aco_frame_full(&st), "empty frame not full");
	fill(&st, gen_lownoise);
	zassert_true(aco_frame_full(&st), "full frame reports full");

	aco_feat_extract(&st, ACO_SR_HZ, &f);
	zassert_equal(aco_feat_pack(&f, vec, ACO_FEATURE_DIM), (size_t)ACO_FEATURE_DIM,
	              "pack writes ACO_FEATURE_DIM");
}

ZTEST(acoustic_features, test_tone_is_less_flat_than_noise)
{
	struct aco_frame_state st;
	struct aco_features    fn, ft;

	fill(&st, gen_lownoise);
	aco_feat_extract(&st, ACO_SR_HZ, &fn);
	fill(&st, gen_tone_1k);
	aco_feat_extract(&st, ACO_SR_HZ, &ft);

	zassert_true(ft.spectral_flatness < fn.spectral_flatness,
	             "a pure tone is spectrally less flat than broadband");
	zassert_within((double)ft.spectral_centroid_hz, 1000.0, 150.0,
	               "tone centroid near 1 kHz");
}

ZTEST(acoustic_features, test_impulse_has_high_kurtosis)
{
	struct aco_frame_state st;
	struct aco_features    f;

	fill(&st, gen_impulse);
	aco_feat_extract(&st, ACO_SR_HZ, &f);
	zassert_true(f.kurtosis > 5.0f, "impulse train has high kurtosis");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/acoustic_features/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_acoustic_features)

set(ACO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/audio/acoustic-anomaly-wind-turbine/src)
target_include_directories(app PRIVATE ${ACO_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_acoustic_features.c
    ${ACO_SRC}/acoustic_features.c
)
```

Create `tests/unit/acoustic_features/prj.conf`:

```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/acoustic_features/testcase.yaml`:

```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.acoustic_features:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - audio
      - dsp
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run the test to verify it fails (RED)**

Run the twister command (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_features` build failure (`acoustic_features.h`/`.c` don't exist).

- [ ] **Step 4: Write the header**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features -- pure-C per-frame DSP feature extraction for the
 * wind-turbine acoustic anomaly monitor.  Arch-neutral (stdint/math only):
 * builds for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef ACOUSTIC_FEATURES_H
#define ACOUSTIC_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACO_FRAME_N    256
#define ACO_N_BANDS    12
#define ACO_SR_HZ      16000.0f
/** 12 band energies + flatness + centroid + kurtosis + total_rms. */
#define ACO_FEATURE_DIM (ACO_N_BANDS + 4)

struct aco_frame_state {
	float    samples[ACO_FRAME_N];
	uint16_t count;
};

struct aco_features {
	float band_energy[ACO_N_BANDS]; /**< normalised log-band energy (sum 1). */
	float spectral_flatness;        /**< geo-mean/arith-mean: ~1 broadband, ~0 tonal. */
	float spectral_centroid_hz;     /**< magnitude-weighted mean frequency. */
	float kurtosis;                 /**< time-domain 4th moment (impulsiveness). */
	float total_rms;                /**< AC RMS (DC removed). */
};

void   aco_frame_reset(struct aco_frame_state *st);
void   aco_frame_push(struct aco_frame_state *st, float sample);
bool   aco_frame_full(const struct aco_frame_state *st);
void   aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out);
size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_FEATURES_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features implementation -- see acoustic_features.h.
 */
#include "acoustic_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void aco_frame_reset(struct aco_frame_state *st)
{
	st->count = 0;
}

void aco_frame_push(struct aco_frame_state *st, float sample)
{
	if (st->count < ACO_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

bool aco_frame_full(const struct aco_frame_state *st)
{
	return st->count >= ACO_FRAME_N;
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

void aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out)
{
	const int n = (st->count < ACO_FRAME_N) ? st->count : ACO_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* DC removal + time-domain moments. */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	float sum2 = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		sum4 += x * x * x * x;
	}
	float var      = sum2 / (float)n;
	out->total_rms = sqrtf(var);
	out->kurtosis  = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/* Spectrum. */
	static float re[ACO_FRAME_N];
	static float im[ACO_FRAME_N];
	for (int i = 0; i < ACO_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, ACO_FRAME_N);

	const int half = ACO_FRAME_N / 2;
	float     mag[ACO_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f;
	float     log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]  = m;
		float f = (float)k * sr_hz / (float)ACO_FRAME_N;
		mag_total += m;
		centroid_num += f * m;
		/* Spectral flatness on a small-epsilon floored magnitude. */
		float me = m + 1e-9f;
		log_sum += logf(me);
		lin_sum += me;
		active++;
	}
	out->spectral_centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	if (active > 0 && lin_sum > 1e-12f) {
		float geo  = expf(log_sum / (float)active);
		float arith = lin_sum / (float)active;
		out->spectral_flatness = geo / arith;
	} else {
		out->spectral_flatness = 0.0f;
	}

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	float mag2_total = 0.0f;
	for (int k = 1; k < half; k++) {
		mag2_total += mag[k] * mag[k];
	}
	if (mag2_total < 1e-20f) {
		return;
	}
	for (int k = 1; k < half; k++) {
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)ACO_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ACO_N_BANDS) {
			b = ACO_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	for (int b = 0; b < ACO_N_BANDS; b++) {
		out->band_energy[b] /= mag2_total;
	}
}

size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ACO_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ACO_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->spectral_flatness;
	vec[i++] = f->spectral_centroid_hz;
	vec[i++] = f->kurtosis;
	vec[i++] = f->total_rms;
	return i; /* == ACO_FEATURE_DIM */
}
```

- [ ] **Step 6: Run the test to verify it passes (GREEN)**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_features` PASS, 3/3. Read `/tmp/tw-wtac/twister.json`. If the centroid test misses, debug the bin→frequency mapping (do not loosen tolerance blindly).

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.h \
        examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.c \
        tests/unit/acoustic_features
git commit -m "feat(wtac): acoustic_features per-frame DSP (FFT bands/flatness/centroid/kurtosis) + ztest"
```

---

### Task 2: `acoustic_features` — healthy-baseline anomaly fallback + host tests

**Files:**
- Modify: `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.h` (add the anomaly API)
- Modify: `examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.c`
- Modify: `tests/unit/acoustic_features/src/test_acoustic_features.c`

**Interfaces:**
- Consumes: `ACO_FEATURE_DIM` (Task 1).
- Produces (Task 5): `struct aco_baseline { float mean[ACO_FEATURE_DIM]; float inv_var[ACO_FEATURE_DIM]; }`; `float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/acoustic_features/src/test_acoustic_features.c`:

```c
ZTEST(acoustic_features, test_anomaly_zero_on_baseline_high_off_baseline)
{
	struct aco_baseline base;
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		base.mean[i]    = 1.0f;
		base.inv_var[i] = 1.0f;
	}

	float on[ACO_FEATURE_DIM];
	float off[ACO_FEATURE_DIM];
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		on[i]  = 1.0f;       /* exactly the baseline mean */
		off[i] = 1.0f + 3.0f; /* 3 sigma off on every feature */
	}

	float s_on  = aco_anomaly_fallback(on, ACO_FEATURE_DIM, &base);
	float s_off = aco_anomaly_fallback(off, ACO_FEATURE_DIM, &base);

	zassert_within((double)s_on, 0.0, 1e-4, "score ~0 at the baseline mean");
	zassert_true(s_off > 0.9f, "score saturates high far off baseline");
	zassert_true(s_off <= 1.0f, "score is clamped to <= 1");
}
```

- [ ] **Step 2: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: build failure — `aco_baseline`/`aco_anomaly_fallback` undeclared.

- [ ] **Step 3: Add the API to the header**

Insert into `acoustic_features.h` before the closing `#ifdef __cplusplus }`:

```c
/** Healthy-baseline template: per-feature mean + inverse variance. */
struct aco_baseline {
	float mean[ACO_FEATURE_DIM];
	float inv_var[ACO_FEATURE_DIM];
};

/**
 * Deterministic anomaly score in [0,1]: Mahalanobis-style deviation of @p vec
 * from the healthy @p base, squashed by `1 - exp(-d/ACO_FEATURE_DIM)`.  Runs when
 * no AI model is loaded so the demo still produces a real score.
 */
float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base);
```

- [ ] **Step 4: Implement**

Append to `acoustic_features.c`:

```c
float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base)
{
	float d2 = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float dx = vec[i] - base->mean[i];
		d2 += dx * dx * base->inv_var[i];
	}
	/* Squash to [0,1): grows with normalised distance, saturates smoothly. */
	float score = 1.0f - expf(-d2 / (float)ACO_FEATURE_DIM);
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
```

> Check (3-sigma-on-16-features): d2 = 16·9 = 144; score = 1 − exp(−144/16) = 1 − exp(−9) ≈ 0.99988 > 0.9. At the mean, d2 = 0 → score 0.

- [ ] **Step 5: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_features` PASS, 4/4.

- [ ] **Step 6: Format + commit**

```bash
git add examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.h \
        examples/audio/acoustic-anomaly-wind-turbine/src/acoustic_features.c \
        tests/unit/acoustic_features/src/test_acoustic_features.c
git commit -m "feat(wtac): healthy-baseline Mahalanobis anomaly fallback + tests"
```

---

### Task 3: `rotor_speed` — tacho + tacholess RPM + BPF + host tests

**Files:**
- Create: `examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.{h,c}`
- Create: `tests/unit/rotor_speed/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_rotor_speed.c}`

**Interfaces:**
- Produces (Task 5): `float rotor_bpf_hz(float rpm, uint8_t n_blades)`; `bool rotor_rpm_valid(float rpm)`; `float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev)`; `float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/rotor_speed/src/test_rotor_speed.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rotor_speed (tacho + tacholess RPM, BPF) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "rotor_speed.h"

ZTEST_SUITE(rotor_speed, NULL, NULL, NULL, NULL, NULL);

ZTEST(rotor_speed, test_bpf_formula)
{
	/* 3 blades at 15 rpm -> BPF = 3 * 15 / 60 = 0.75 Hz. */
	zassert_within((double)rotor_bpf_hz(15.0f, 3), 0.75, 1e-4, "BPF = N*rpm/60");
}

ZTEST(rotor_speed, test_rpm_valid_gate)
{
	zassert_true(rotor_rpm_valid(15.0f), "15 rpm is valid");
	zassert_false(rotor_rpm_valid(0.0f), "0 rpm invalid");
	zassert_false(rotor_rpm_valid(100.0f), "100 rpm invalid");
}

ZTEST(rotor_speed, test_tacho_rpm)
{
	/* 1 pulse/rev, 4,000,000 us between pulses -> 60e6/4e6 = 15 rpm. */
	zassert_within((double)rotor_tacho_rpm(4000000u, 1), 15.0, 0.01, "1 ppr tacho");
	/* 60 ppr encoder at 15 rpm -> interval 66667 us. */
	zassert_within((double)rotor_tacho_rpm(66667u, 60), 15.0, 0.1, "60 ppr tacho");
	zassert_equal(rotor_tacho_rpm(0u, 1), 0.0f, "zero interval guarded");
}

ZTEST(rotor_speed, test_tacholess_recovers_rpm)
{
	/* Build a band-energy envelope amplitude-modulated at BPF=0.75 Hz,
	 * sampled at the 62.5 fps frame rate, N blades = 3 -> rpm 15. */
	const float fr  = ACO_FRAME_RATE_HZ; /* 62.5 */
	const float bpf = 0.75f;
	float       env[256];
	for (int i = 0; i < 256; i++) {
		env[i] = 1.0f + 0.5f * sinf(2.0f * (float)M_PI * bpf * (float)i / fr);
	}
	float rpm = rotor_tacholess_rpm(env, 256, fr, 3);
	zassert_within((double)rpm, 15.0, 1.5, "tacholess RPM within 1.5 of 15");
}
```

> Note: the test uses `ACO_FRAME_RATE_HZ` and `M_PI`; `rotor_speed.h` defines the former, and the test CMakeLists defines `_GNU_SOURCE` for the latter.

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/rotor_speed/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_rotor_speed)

set(ACO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/audio/acoustic-anomaly-wind-turbine/src)
target_include_directories(app PRIVATE ${ACO_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_rotor_speed.c
    ${ACO_SRC}/rotor_speed.c
)
```

Create `tests/unit/rotor_speed/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/rotor_speed/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.rotor_speed:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - audio
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.rotor_speed` build failure.

- [ ] **Step 4: Write the header**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rotor_speed -- pure-C rotor RPM estimation (tacho pulse-interval and
 * tacholess from the band-energy envelope) + blade-pass frequency.
 * Arch-neutral; host-unit-tested.
 */
#ifndef ROTOR_SPEED_H
#define ROTOR_SPEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Per-frame rate of the acoustic front end (ACO_SR_HZ / ACO_FRAME_N). */
#define ACO_FRAME_RATE_HZ 62.5f

/** Blade-pass frequency: BPF = n_blades * rpm / 60. */
float rotor_bpf_hz(float rpm, uint8_t n_blades);

/** Plausibility gate for a wind-turbine rotor (3..30 rpm). */
bool rotor_rpm_valid(float rpm);

/** RPM from a tacho pulse interval (us) and pulses-per-revolution. */
float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev);

/**
 * Estimate RPM from the band-energy envelope's blade-pass modulation:
 * autocorrelation peak over the plausible lag range -> BPF -> RPM.
 * The mic-only fallback when no tacho is present.
 */
float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades);

#ifdef __cplusplus
}
#endif

#endif /* ROTOR_SPEED_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rotor_speed implementation -- see rotor_speed.h.
 */
#include "rotor_speed.h"

#include <math.h>

float rotor_bpf_hz(float rpm, uint8_t n_blades)
{
	return (float)n_blades * rpm / 60.0f;
}

bool rotor_rpm_valid(float rpm)
{
	return rpm >= 3.0f && rpm <= 30.0f;
}

float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev)
{
	if (pulse_interval_us == 0u || pulses_per_rev == 0u) {
		return 0.0f;
	}
	/* rev_period_us = interval * ppr; rpm = 60e6 / rev_period_us. */
	double rev_period_us = (double)pulse_interval_us * (double)pulses_per_rev;
	return (float)(60000000.0 / rev_period_us);
}

float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades)
{
	if (n < 32 || n_blades == 0u || frame_rate_hz <= 0.0f) {
		return 0.0f;
	}

	/* Mean-remove. */
	double mean = 0.0;
	for (size_t i = 0; i < n; i++) {
		mean += env[i];
	}
	mean /= (double)n;

	/* Autocorrelation over a lag range bounded to the plausible BPF band and
	 * to at least two periods inside the window. */
	const size_t lag_min = 10;
	const size_t lag_max = n / 2;
	double       best    = -1.0;
	size_t       best_lag = lag_min;
	for (size_t lag = lag_min; lag <= lag_max; lag++) {
		double s = 0.0;
		for (size_t i = 0; i + lag < n; i++) {
			s += (env[i] - mean) * (env[i + lag] - mean);
		}
		if (s > best) {
			best     = s;
			best_lag = lag;
		}
	}
	float bpf = frame_rate_hz / (float)best_lag;
	return 60.0f * bpf / (float)n_blades;
}
```

- [ ] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.rotor_speed` PASS, 4/4. If `test_tacholess_recovers_rpm` misses, inspect the autocorrelation peak (period lag should be ≈ 62.5/0.75 ≈ 83); do not widen tolerance without confirming the peak lands correctly.

- [ ] **Step 7: Format + commit**

```bash
git add examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.h \
        examples/audio/acoustic-anomaly-wind-turbine/src/rotor_speed.c \
        tests/unit/rotor_speed
git commit -m "feat(wtac): rotor_speed tacho + tacholess RPM + BPF + tests"
```

---

### Task 4: `bpf_modulation` — blade-order envelope analysis + host tests

**Files:**
- Create: `examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.{h,c}`
- Create: `tests/unit/bpf_modulation/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_bpf_modulation.c}`

**Interfaces:**
- Produces (Task 5): `BPF_ENV_N 256`, `BPF_N_HARMONICS 4`, `BPF_FEATURE_DIM 5`; `struct bpf_env_state`; `struct bpf_modulation`; `bpf_env_reset/push`; `bpf_modulation_extract(st, bpf_hz, frame_rate_hz, out)`; `bpf_modulation_pack(m, vec, cap)`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/bpf_modulation/src/test_bpf_modulation.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for bpf_modulation (blade-order envelope analysis) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "bpf_modulation.h"

ZTEST_SUITE(bpf_modulation, NULL, NULL, NULL, NULL, NULL);

static void fill_am(struct bpf_env_state *st, float bpf, float fr, float depth)
{
	bpf_env_reset(st);
	for (int i = 0; i < BPF_ENV_N; i++) {
		bpf_env_push(st, 1.0f + depth * sinf(2.0f * (float)M_PI * bpf * (float)i / fr));
	}
}

ZTEST(bpf_modulation, test_blade_order_peaks_at_bpf)
{
	struct bpf_env_state st;
	struct bpf_modulation m;
	const float           fr = 62.5f, bpf = 0.75f;

	fill_am(&st, bpf, fr, 0.5f);
	bpf_modulation_extract(&st, bpf, fr, &m);

	/* Fundamental blade order carries far more energy than the 2nd harmonic. */
	zassert_true(m.blade_order_energy[0] > 5.0f * m.blade_order_energy[1],
	             "fundamental dominates the 2nd harmonic for a pure AM tone");
	/* (max-min)/(max+min) of 1 +/- 0.5 = 0.5. */
	zassert_within((double)m.modulation_depth, 0.5, 0.05, "modulation depth ~0.5");
}

ZTEST(bpf_modulation, test_flat_envelope_has_no_order_energy)
{
	struct bpf_env_state st;
	struct bpf_modulation m;

	bpf_env_reset(&st);
	for (int i = 0; i < BPF_ENV_N; i++) {
		bpf_env_push(&st, 1.0f); /* flat */
	}
	bpf_modulation_extract(&st, 0.75f, 62.5f, &m);

	zassert_true(m.blade_order_energy[0] < 1e-3f, "flat envelope -> ~0 order energy");
	zassert_within((double)m.modulation_depth, 0.0, 1e-3, "flat -> zero modulation depth");
}

ZTEST(bpf_modulation, test_pack_dim)
{
	struct bpf_modulation m = { 0 };
	float                 vec[BPF_FEATURE_DIM];
	zassert_equal(bpf_modulation_pack(&m, vec, BPF_FEATURE_DIM), (size_t)BPF_FEATURE_DIM,
	              "pack writes BPF_FEATURE_DIM");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/bpf_modulation/CMakeLists.txt`:
```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_bpf_modulation)

set(ACO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/audio/acoustic-anomaly-wind-turbine/src)
target_include_directories(app PRIVATE ${ACO_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_bpf_modulation.c
    ${ACO_SRC}/bpf_modulation.c
)
```

Create `tests/unit/bpf_modulation/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/bpf_modulation/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.bpf_modulation:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - audio
      - dsp
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.bpf_modulation` build failure.

- [ ] **Step 4: Write the header**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * bpf_modulation -- pure-C blade-order analysis of the band-energy envelope.
 * Blade faults modulate audible-band energy at the blade-pass frequency (BPF);
 * a Goertzel evaluated at the *current* BPF makes the feature RPM-invariant.
 * Drivetrain gear-mesh tones are per-frame spectral (in acoustic_features), not
 * here.  Arch-neutral; host-unit-tested.
 */
#ifndef BPF_MODULATION_H
#define BPF_MODULATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BPF_ENV_N       256  /* ~4 s at 62.5 fps */
#define BPF_N_HARMONICS 4
#define BPF_FEATURE_DIM (BPF_N_HARMONICS + 1) /* 4 blade orders + modulation_depth */

struct bpf_env_state {
	float    env[BPF_ENV_N];
	uint16_t head;
	uint16_t count;
};

struct bpf_modulation {
	float blade_order_energy[BPF_N_HARMONICS]; /**< normalised energy at k*BPF. */
	float modulation_depth;                    /**< (max-min)/(max+min) of the envelope. */
};

void   bpf_env_reset(struct bpf_env_state *st);
/** Append one per-frame energy summary (e.g. the frame's total RMS -- an
 *  ABSOLUTE energy that carries the blade-pass amplitude modulation; do not
 *  pass normalised band energies, whose sum is constant). */
void   bpf_env_push(struct bpf_env_state *st, float frame_energy);
void   bpf_modulation_extract(const struct bpf_env_state *st, float bpf_hz, float frame_rate_hz,
                              struct bpf_modulation *out);
size_t bpf_modulation_pack(const struct bpf_modulation *m, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BPF_MODULATION_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * bpf_modulation implementation -- see bpf_modulation.h.
 */
#include "bpf_modulation.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void bpf_env_reset(struct bpf_env_state *st)
{
	st->head  = 0;
	st->count = 0;
}

void bpf_env_push(struct bpf_env_state *st, float frame_energy)
{
	st->env[st->head] = frame_energy;
	st->head          = (uint16_t)((st->head + 1) % BPF_ENV_N);
	if (st->count < BPF_ENV_N) {
		st->count++;
	}
}

/* Generalised Goertzel: |X|^2 at an arbitrary frequency target_hz. */
static float goertzel_power(const float *x, int n, float target_hz, float fs)
{
	if (target_hz <= 0.0f || fs <= 0.0f) {
		return 0.0f;
	}
	float w     = 2.0f * (float)M_PI * target_hz / fs;
	float coeff = 2.0f * cosf(w);
	float s1 = 0.0f, s2 = 0.0f;
	for (int i = 0; i < n; i++) {
		float s0 = x[i] + coeff * s1 - s2;
		s2       = s1;
		s1       = s0;
	}
	return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void bpf_modulation_extract(const struct bpf_env_state *st, float bpf_hz, float frame_rate_hz,
                            struct bpf_modulation *out)
{
	memset(out, 0, sizeof(*out));
	const int n = (st->count < BPF_ENV_N) ? st->count : BPF_ENV_N;
	if (n < 8) {
		return;
	}

	/* Linearise the ring (oldest..newest) into a temp, mean-removed. */
	static float buf[BPF_ENV_N];
	float        mean = 0.0f;
	int          start = (st->count < BPF_ENV_N) ? 0 : st->head;
	for (int i = 0; i < n; i++) {
		buf[i] = st->env[(start + i) % BPF_ENV_N];
		mean += buf[i];
	}
	mean /= (float)n;

	float emin = buf[0], emax = buf[0];
	for (int i = 0; i < n; i++) {
		if (buf[i] < emin) {
			emin = buf[i];
		}
		if (buf[i] > emax) {
			emax = buf[i];
		}
		buf[i] -= mean;
	}

	/* Total envelope (AC) energy for normalisation. */
	float total = 0.0f;
	for (int i = 0; i < n; i++) {
		total += buf[i] * buf[i];
	}
	float norm = (total > 1e-12f) ? (1.0f / total) : 0.0f;

	for (int k = 1; k <= BPF_N_HARMONICS; k++) {
		float p = goertzel_power(buf, n, (float)k * bpf_hz, frame_rate_hz);
		out->blade_order_energy[k - 1] = p * norm;
	}
	out->modulation_depth = (emax + emin > 1e-9f) ? ((emax - emin) / (emax + emin)) : 0.0f;
}

size_t bpf_modulation_pack(const struct bpf_modulation *m, float *vec, size_t cap)
{
	if (cap < (size_t)BPF_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int k = 0; k < BPF_N_HARMONICS; k++) {
		vec[i++] = m->blade_order_energy[k];
	}
	vec[i++] = m->modulation_depth;
	return i; /* == BPF_FEATURE_DIM */
}
```

- [ ] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.bpf_modulation` PASS, 3/3.

- [ ] **Step 7: Format + commit**

```bash
git add examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.h \
        examples/audio/acoustic-anomaly-wind-turbine/src/bpf_modulation.c \
        tests/unit/bpf_modulation
git commit -m "feat(wtac): bpf_modulation blade-order envelope analysis (Goertzel at BPF) + tests"
```

---

### Task 5: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/audio/acoustic-anomaly-wind-turbine/src/main.c`
- Create: `examples/audio/acoustic-anomaly-wind-turbine/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/audio/acoustic-anomaly-wind-turbine/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `acoustic_features.h`, `rotor_speed.h`, `bpf_modulation.h`; portable `<alp/audio.h>`, `<alp/peripheral.h>` (GPIO), `<alp/inference.h>`, `<alp/board.h>`.
- Produces: a `native_sim/native/64` build that prints the header line + one `WTAC,...` record per report interval, ending with `[wtac] done`.

- [ ] **Step 1: Write CMakeLists.txt** (mirror audio-wake-word's generated-overlay flow; add `_GNU_SOURCE` for `M_PI` in `acoustic_features.c`/`main.c` on host)

Create `examples/audio/acoustic-anomaly-wind-turbine/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_alp_generated ${CMAKE_BINARY_DIR}/generated/alp.conf)
execute_process(
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py
            --input  ${CMAKE_CURRENT_SOURCE_DIR}/board.yaml
            --output ${_alp_generated}
            --emit zephyr-conf
    RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "alp_project.py failed (rc=${rc})")
endif()
list(APPEND EXTRA_CONF_FILE ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(acoustic_anomaly_wind_turbine LANGUAGES C)

target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/main.c
    src/acoustic_features.c
    src/rotor_speed.c
    src/bpf_modulation.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/audio/acoustic-anomaly-wind-turbine/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384
CONFIG_HEAP_MEM_POOL_SIZE=131072

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml** (mirror audio-wake-word: M55-HE always-on, PDM via DMIC, no `peripherals` enum for PDM; add `gpio` for the tacho)

Create `examples/audio/acoustic-anomaly-wind-turbine/board.yaml`:
```yaml
# board.yaml -- nacelle acoustic anomaly monitor for wind turbines.
#
# A low-power M55-HE node listens to a PDM mic, extracts DSP features,
# normalises blade-periodic energy to rotor order (BPF), and emits a
# per-interval anomaly score for drivetrain tonals + gross blade aero
# anomalies.  Same source targets the V2N DEEPX path when som.sku is
# flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
cores:
  a32_cluster:
    os: "off"
  m55_he:
    app: ./src
    libraries:
      - tflite_micro
    inference:
      default_arena_kib: 64
    peripherals:
      - gpio                 # tacho pulse input (rotor speed).

# PDM mic input is the Zephyr DMIC audio subsystem (not a peripheral
# enum), wired via CONFIG_AUDIO_DMIC=y in testcase.yaml -- same
# convention as audio-wake-word.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write main.c**

Create `examples/audio/acoustic-anomaly-wind-turbine/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic-anomaly-wind-turbine
 * =============================
 *
 * Nacelle acoustic condition monitor.  Pipeline:
 *
 *   PDM mic (<alp/audio.h>, 16 kHz) --256-sample frame-->
 *     acoustic_features (FFT bands, flatness, centroid, kurtosis)
 *       --per-frame band energy--> bpf_modulation envelope ring
 *   tacho GPIO (or tacholess) --> rotor_speed --> rpm, BPF
 *     --order features--> <alp/inference.h> anomaly model
 *                         (deterministic fallback when no model)
 *     --> one WTAC record per report interval.
 *
 * Honest scope: detects drivetrain tonals + gross blade aero-anomalies
 * (TE-crack whistle, severe erosion, icing deviation, imbalance-at-BPF).
 * Early internal cracks / delamination are Acoustic-Emission (ultrasonic,
 * contact-piezo) and are OUT OF SCOPE for an airborne mic.
 *
 * The model is a stub; see models/README.md for the training recipe.  With
 * no model the deterministic baseline-deviation fallback runs so the demo
 * still produces real anomaly scores.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/audio.h"
#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"

#include "acoustic_features.h"
#include "bpf_modulation.h"
#include "rotor_speed.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(wtac, LOG_LEVEL_INF);

#define N_BLADES         3
#define FRAMES_PER_REPORT BPF_ENV_N   /* one record per full envelope window */
#define N_REPORTS        8            /* bounded demo run */
#define ANOMALY_INPUT_DIM (ACO_FEATURE_DIM + BPF_FEATURE_DIM + 1)
/* Gear-mesh band index in band_energy[]: the log-spaced band the synthetic
 * ~600 Hz drivetrain tone lands in (bin ~10 of 128 -> band 5). */
#define GEARMESH_BAND    5

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic fallback.  Drop in a real .tflite + see
 * models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Canned variable-RPM track for native_sim (rpm per report interval). */
static const float s_canned_rpm[N_REPORTS] = { 14.0f, 15.0f, 16.0f, 17.0f,
                                               18.0f, 17.0f, 16.0f, 15.0f };

/* Healthy baseline matching the synthetic "healthy hum" (low broadband):
 * mean ~ uniform small band energy, flatness high, centroid mid, low rms.
 * Customer replaces with a baseline learned at commissioning. */
static struct aco_baseline s_baseline;

static void baseline_init(void)
{
	for (int i = 0; i < ACO_FEATURE_DIM; i++) {
		s_baseline.mean[i]    = 0.0f;
		s_baseline.inv_var[i] = 1.0f;
	}
	/* Band energies ~ 1/N each for flat broadband. */
	for (int b = 0; b < ACO_N_BANDS; b++) {
		s_baseline.mean[b] = 1.0f / (float)ACO_N_BANDS;
	}
	s_baseline.mean[ACO_N_BANDS + 0] = 0.7f;   /* spectral_flatness (broadband) */
	s_baseline.mean[ACO_N_BANDS + 1] = 4000.0f;/* centroid Hz */
	s_baseline.mean[ACO_N_BANDS + 2] = 3.0f;   /* kurtosis */
	s_baseline.mean[ACO_N_BANDS + 3] = 0.05f;  /* total_rms */
	/* Down-weight the wide-range raw features so they don't dominate distance. */
	s_baseline.inv_var[ACO_N_BANDS + 1] = 1.0f / (500.0f * 500.0f); /* centroid */
	s_baseline.inv_var[ACO_N_BANDS + 2] = 1.0f / (2.0f * 2.0f);     /* kurtosis */
}

/* Synthetic acoustic frame for native_sim: healthy hum, plus per-report
 * injected anomalies so the demo emits a mix of verdicts. */
static float synth_sample(int report, int frame, int i)
{
	float hum = 0.02f * (sinf((float)i * 1.7f) + sinf((float)i * 0.37f));
	switch (report % 3) {
	case 0: /* healthy */
		return hum;
	case 1: { /* blade imbalance: hum amplitude-modulated at BPF */
		float t   = (float)frame / ACO_FRAME_RATE_HZ;
		float am  = 1.0f + 0.6f * sinf(2.0f * (float)M_PI * 0.75f * t);
		return hum * am;
	}
	default: /* drivetrain tonal: a ~600 Hz gear-mesh tone in the mech band */
		return hum + 0.5f * sinf(2.0f * (float)M_PI * 600.0f * (float)i / ACO_SR_HZ);
	}
}

struct wtac_ctx {
	alp_audio_in_t  *mic;
	alp_gpio_t      *tacho;
	alp_inference_t *inf;
};

static const char *subsystem_name(int s)
{
	switch (s) {
	case 0:
		return "BLADE_BPF";
	case 1:
		return "DRIVETRAIN_TONAL";
	default:
		return "BROADBAND";
	}
}

int main(void)
{
	static struct wtac_ctx c;
	static struct aco_frame_state frame;
	static struct bpf_env_state   envst;

	memset(&c, 0, sizeof(c));
	baseline_init();

	/* PDM mic -- tolerate absence on native_sim (-> synthetic audio). */
	c.mic = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = E1M_PDM0,
	    .sample_rate_hz   = 16000,
	    .channels         = 1,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = ACO_FRAME_N,
	});
	if (c.mic != NULL) {
		alp_audio_in_start(c.mic);
	} else {
		LOG_WRN("PDM mic unavailable; using synthetic acoustics");
	}

	/* Tacho GPIO input -- tolerate absence (-> tacholess RPM). */
	c.tacho = alp_gpio_open(BOARD_GPIO_TACHO);
	if (c.tacho != NULL) {
		alp_gpio_configure(c.tacho, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE);
	} else {
		LOG_WRN("tacho GPIO unavailable; using tacholess RPM estimation");
	}

	/* Anomaly model -- NULL/stub-tolerant; fallback runs if absent. */
	c.inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src\n");

	int16_t pcm[ACO_FRAME_N];

	for (int r = 0; r < N_REPORTS; r++) {
		bpf_env_reset(&envst);
		struct aco_features mean_feat;
		memset(&mean_feat, 0, sizeof(mean_feat));
		float acc[ACO_FEATURE_DIM];
		memset(acc, 0, sizeof(acc));

		for (int fidx = 0; fidx < FRAMES_PER_REPORT; fidx++) {
			aco_frame_reset(&frame);
			size_t got = 0;
			bool   have_pcm =
			    (c.mic != NULL &&
			     alp_audio_in_read(c.mic, pcm, ACO_FRAME_N, &got, 50) == ALP_OK && got > 0);
			for (int i = 0; i < ACO_FRAME_N; i++) {
				float s = have_pcm ? ((float)pcm[i % (int)got] / 32768.0f)
				                   : synth_sample(r, fidx, i);
				aco_frame_push(&frame, s);
			}

			struct aco_features f;
			aco_feat_extract(&frame, ACO_SR_HZ, &f);

			/* Envelope sample = absolute frame energy (total_rms).  NOTE: do NOT
			 * use sum(band_energy) -- those are normalised to sum 1, so their sum
			 * is always 1.0 and carries no blade-pass modulation. */
			bpf_env_push(&envst, f.total_rms);

			float v[ACO_FEATURE_DIM];
			aco_feat_pack(&f, v, ACO_FEATURE_DIM);
			for (int i = 0; i < ACO_FEATURE_DIM; i++) {
				acc[i] += v[i];
			}
		}

		for (int i = 0; i < ACO_FEATURE_DIM; i++) {
			acc[i] /= (float)FRAMES_PER_REPORT;
		}

		/* Rotor speed: tacholess from the envelope (the demo has no live tacho);
		 * a real node uses rotor_tacho_rpm() from GPIO pulse intervals. */
		float rpm = rotor_tacholess_rpm(envst.env, envst.count, ACO_FRAME_RATE_HZ, N_BLADES);
		const char *rpm_src = "ESTIMATED";
		if (!rotor_rpm_valid(rpm)) {
			rpm     = s_canned_rpm[r]; /* fall back to the canned track */
			rpm_src = "TACHO";
		}
		float bpf = rotor_bpf_hz(rpm, N_BLADES);

		struct bpf_modulation mod;
		bpf_modulation_extract(&envst, bpf, ACO_FRAME_RATE_HZ, &mod);

		/* Anomaly score: AI if available, else deterministic fallback over the
		 * mean per-frame feature vector, boosted by blade-order modulation. */
		float score = aco_anomaly_fallback(acc, ACO_FEATURE_DIM, &s_baseline);
		float blade = mod.blade_order_energy[0];
		if (blade > score) {
			score = blade;
		}
		if (score > 1.0f) {
			score = 1.0f;
		}

		/* Heuristic subsystem + flags (advisory; AI gives the score). */
		int   subsystem = 2; /* BROADBAND */
		char  flags[32]  = "NONE";
		float gearmesh   = acc[GEARMESH_BAND];
		if (blade > 0.4f && mod.modulation_depth > 0.3f) {
			subsystem = 0; /* BLADE_BPF */
			strcpy(flags, "IMBALANCE");
		} else if (gearmesh > 0.3f) {
			subsystem = 1; /* DRIVETRAIN_TONAL */
			strcpy(flags, "GEARMESH");
		} else if (acc[ACO_N_BANDS + 0] < 0.3f) { /* low flatness -> tonal */
			subsystem = 0;
			strcpy(flags, "TE_WHISTLE");
		}

		float top_band_hz = (float)GEARMESH_BAND * (ACO_SR_HZ / 2.0f) / (float)ACO_N_BANDS;
		printk("WTAC,%.1f,%.1f,%.2f,%.2f,%s,%.1f,%s,%s\n", (double)(r * 4.0f), (double)rpm,
		       (double)bpf, (double)score, subsystem_name(subsystem), (double)top_band_hz,
		       flags, rpm_src);
	}

	if (c.inf != NULL) {
		alp_inference_close(c.inf);
	}
	if (c.mic != NULL) {
		alp_audio_in_stop(c.mic);
		alp_audio_in_close(c.mic);
	}
	printk("[wtac] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers exactly as the rail example did — `alp_audio_in_open`/`alp_audio_config_t` fields (`peripheral_id`/`sample_rate_hz`/`channels`/`format`/`frames_per_block`) per `include/alp/audio.h`; `alp_gpio_open`/`alp_gpio_configure(pin, ALP_GPIO_INPUT, pull)` per `include/alp/peripheral.h` (confirm the pull enum name, e.g. `ALP_GPIO_PULL_NONE`); `alp_inference_open`/`get_input`/`invoke`/`get_output`/`close` per `include/alp/inference.h` (mirror `examples/audio/audio-wake-word/src/main.c`). `BOARD_GPIO_TACHO` and `E1M_PDM0` come from `<alp/board.h>`/`<alp/audio.h>`; if `BOARD_GPIO_TACHO` has no board alias, use a concrete E1M GPIO id (e.g. `E1M_GPIO_IO0`) and note it. The AI-path classify (get_input F32 → memcpy the 22-float `[acc | mod-pack | rpm]` vector → invoke → read scalar output → `score`) mirrors audio-wake-word; with the stub it falls through to the fallback already shown — wire the AI path the same way and keep the fallback as the `#else`/NULL branch. Do NOT over-claim the model ran. Add `<string.h>`/`<math.h>` if a symbol is unresolved.

- [ ] **Step 5: Write the native_sim build conf** (DMIC link parity with wake-word; the mic opens NULL at runtime → synthetic)

Create `examples/audio/acoustic-anomaly-wind-turbine/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# Link the DMIC audio layer so alp_audio_in_open resolves; no DMIC device is
# present on native_sim, so it returns NULL and main.c uses synthetic audio.
CONFIG_AUDIO=y
CONFIG_AUDIO_DMIC=y
```

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN is a separate build_only entry — the rail-example lesson)

Create `examples/audio/acoustic-anomaly-wind-turbine/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: acoustic-anomaly-wind-turbine
  description: |
    Nacelle acoustic condition monitor: PDM mic -> DSP features ->
    rotor-order (BPF) normalisation -> anomaly score (deterministic
    fallback) for drivetrain tonals + gross blade aero-anomalies.
    native_sim runs synthetic acoustics + a canned RPM track.
common:
  tags: audio ai inference industrial predictive-maintenance marketing showcase
tests:
  alp_sdk.example.acoustic_anomaly_wind_turbine.native_sim:
    platform_allow:
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    extra_args: CONFIG_AUDIO_DMIC=y
    tags:
      - alp-sdk
      - example
      - audio
      - ai
      - industrial
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[wtac\\] done"

  alp_sdk.example.acoustic_anomaly_wind_turbine.aen_build:
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - audio
      - ai
      - industrial
```

> Contingency: if the native_sim build cannot RUN to completion for an audio-subsystem reason (e.g. DMIC init blocks without a device), make the `native_sim` test `build_only: true` too and rely on the three unit-test suites for run-validation — report this if it happens. The three `tests/unit/*` suites are the load-bearing host validation regardless.

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/audio/acoustic-anomaly-wind-turbine/models/README.md`:
```markdown
# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic baseline
fallback runs without one. To deploy a real detector, train a small
**autoencoder** on YOUR turbine's healthy baseline — anomaly scores are
per-turbine, so a shipped model would not transfer.

## Pipeline

1. **Record a healthy baseline.** Capture nacelle audio across the normal
   operating envelope (cut-in..rated RPM, varied wind/load), tagged with rotor
   RPM from SCADA/tacho. Hours, not minutes.
2. **Extract the feature vector** the device uses: per-frame `acoustic_features`
   (16 floats) averaged over each ~4 s report window, concatenated with the
   `bpf_modulation` order features (5 floats) and the RPM (1) →
   `ANOMALY_INPUT_DIM = 22`.
3. **Train an autoencoder** (e.g. 22→16→8→16→22 dense) on the healthy vectors;
   the per-window **reconstruction error** is the anomaly score. RPM is an input
   so the baseline is speed-aware.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop the result in this folder and point
   `alp_inference_open` at it.
5. **Set the alarm threshold** from the healthy reconstruction-error
   distribution (e.g. 99th percentile), not a guess.

Honest scope: this detects drivetrain tonals + gross blade aero-anomalies. Early
internal cracks / delamination need contact Acoustic-Emission sensing and are out
of scope for an airborne mic.
```

- [ ] **Step 8: Write README.md**

Create `examples/audio/acoustic-anomaly-wind-turbine/README.md`:
```markdown
# acoustic-anomaly-wind-turbine

> ⚠️ **`[UNTESTED]` on hardware -- v0.6 paper-correct.** The three DSP cores
> (`acoustic_features`, `rotor_speed`, `bpf_modulation`) are host-unit-tested on
> `native_sim/native/64`; the full app runs end-to-end on native_sim with
> synthetic acoustics + a canned RPM track. HiL on a real nacelle with a
> customer-trained model is bench-gated.

Nacelle **acoustic condition monitor** for wind turbines: a MEMS mic captures
audible-band sound, DSP extracts spectral features, blade-periodic energy is
normalised to **rotor order** (RPM-invariant via the blade-pass frequency), and a
per-interval **anomaly score** + advisory subsystem/flag is emitted for both
drivetrain tonal faults and gross blade aero-anomalies.

## Honest capability

An airborne nacelle mic credibly detects: **drivetrain/gearbox/bearing tonals**
(loudest, most reliable), **rotor imbalance** (amplitude modulation at the
blade-pass frequency), **trailing-edge-crack whistle**, **severe leading-edge
erosion**, and **icing** (spectral deviation). It does **NOT** detect early
internal cracks / delamination / fiber breakage — those are **Acoustic Emission**
(ultrasonic, structure-borne, requiring a contact piezo bonded to the blade) and
are out of scope for an airborne mic.

## Key invariant — blade-pass frequency

`BPF = N_blades × RPM / 60` (≈0.75 Hz for a 3-blade turbine at 15 rpm). Blade
faults modulate audible-band energy at BPF and its harmonics; evaluating the
modulation in **rotor orders** (at the current BPF) makes the signature
RPM-invariant under variable-speed operation.

## Pipeline

```
PDM mic (<alp/audio.h>) --frame--> acoustic_features (FFT bands, flatness,
  centroid, kurtosis) --band energy--> bpf_modulation (Goertzel at BPF orders)
tacho GPIO / tacholess --> rotor_speed --> rpm, BPF
  --> <alp/inference.h> anomaly score (deterministic fallback) --> WTAC record
```

## Output

```
# WTAC,t_s,rpm,bpf_hz,anomaly_score,dominant_subsystem,top_band_hz,flags,rpm_src
WTAC,12.0,17.4,0.87,0.62,BLADE_BPF,5833.3,IMBALANCE,ESTIMATED
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/audio/acoustic-anomaly-wind-turbine
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the training recipe (record healthy baseline → train an autoencoder → Vela/DX-M1
compile → drop it in).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/acoustic_features \
  -T tests/unit/rotor_speed -T tests/unit/bpf_modulation
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Wind-turbine acoustic anomaly example** (`examples/audio/acoustic-anomaly-wind-turbine/`):
  nacelle acoustic condition monitor — PDM mic → DSP features (`acoustic_features`:
  FFT band energies / spectral flatness / centroid / kurtosis + healthy-baseline
  anomaly fallback) → rotor-order normalisation (`rotor_speed` tacho + tacholess RPM
  → BPF; `bpf_modulation` Goertzel at blade-pass harmonics) → per-interval anomaly
  score for drivetrain tonals + gross blade aero-anomalies. Three pure-C DSP cores
  host-unit-tested on `native_sim` (`tests/unit/acoustic_features`,
  `tests/unit/rotor_speed`, `tests/unit/bpf_modulation`); model is a stub with a
  training recipe in `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.acoustic_features` (4/4), `alp.unit.rotor_speed` (4/4), `alp.unit.bpf_modulation` (3/3) PASS.
- `alp_sdk.example.acoustic_anomaly_wind_turbine.native_sim` PASS (console `[wtac] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-wtac/twister.json`. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 4 notes) — do NOT change the portable-API contract or the cores' logic. Apply the native_sim contingency in Step 6 if the run can't complete for an audio-subsystem reason.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22, then:
```bash
git add examples/audio/acoustic-anomaly-wind-turbine CHANGELOG.md
git commit -m "feat(wtac): wind-turbine acoustic anomaly example app (DSP+order+anomaly) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 acoustic_features → Task 1; anomaly fallback (C1) → Task 2; C2 rotor_speed (tacho+tacholess+BPF) → Task 3; C3 bpf_modulation (envelope + Goertzel blade orders) → Task 4; C4 AI dispatch + C5 main.c + scaffolding + models/README + README + CHANGELOG → Task 5. Output record + taxonomy → Task 5 (main.c + README). Validation (three ztest suites + native_sim run) → Tasks 1-4 tests + Task 5 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 5 board.yaml + testcase.yaml. Honest scope statement → Task 5 README + models/README. All spec sections map to a task. Gear-mesh = per-frame band (`acc[GEARMESH_BAND]` in main.c), NOT in bpf_modulation — matches the corrected spec.

**Type consistency:** `ACO_FEATURE_DIM 16`, `BPF_FEATURE_DIM 5`, `ANOMALY_INPUT_DIM 22` consistent across header/impl/tests/main. `aco_frame_state/aco_features/aco_feat_extract/aco_feat_pack/aco_baseline/aco_anomaly_fallback`, `rotor_bpf_hz/rotor_rpm_valid/rotor_tacho_rpm/rotor_tacholess_rpm`, `bpf_env_state/bpf_modulation/bpf_env_reset/bpf_env_push/bpf_modulation_extract/bpf_modulation_pack` — names + signatures identical across tasks. `ACO_FRAME_RATE_HZ` defined once in `rotor_speed.h` and used by tests + main. Output schema (9 columns) identical in main.c + README + spec.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The model stub and the hard-coded baseline are deliberate, documented design decisions, not placeholders.
```
