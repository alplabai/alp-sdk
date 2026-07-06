# Acoustic Safety-Event Classifier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An always-listening node that classifies safety/security sound events (AMBIENT / GLASS_BREAK / ALARM / SCREAM) from a MEMS mic with a small NPU classifier plus a deterministic fallback.

**Architecture:** One pure-C, arch-neutral, host-unit-tested core — `acoustic_event` (per-frame DSP: 8 FFT band energies, spectral centroid/flatness/rolloff, crest, zero-crossing rate, RMS + a deterministic 4-class event classifier) — plus an `<alp/inference.h>` event classifier (stub) and a thin Zephyr `main.c`. Single modality → one core.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/audio.h>` (PDM mic), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core (`acoustic_event.{c,h}`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55. A `#ifndef M_PI` fallback near the top.
- App peripherals via portable `<alp/*>` APIs only (audio, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `ASE_FRAME_N 512`, `ASE_SR_HZ 16000.0f`, `ASE_N_BANDS 8`, `ASE_FEATURE_DIM 14`, `ASE_EVENT_COUNT 4`.
- The app does NOT include `<alp/board.h>` (uses `E1M_PDM0` from `<alp/audio.h>` directly) → no `ALP_BOARD_*` define needed; the PDM audio layer is linked via `CONFIG_AUDIO_DMIC=y` (native_sim conf + testcase extra_args), same as `audio-wake-word` / the wind-turbine example.
- TDD: the core is RED-first, host-validated on `native_sim/native/64`. Mic I/O + the AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude` in commits; NO binaries (model is a 1-byte stub; recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links. (PR bodies also carry NO Claude/AI footer.)
- Example dir: `examples/audio/acoustic-safety-events/`. Primary target E1M-AEN; V2N via `som.sku` flip.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT `/usr/bin/clang-format-14`).
- Unit test compiles the core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists (for `M_PI` on the host) — same pattern as the rail/acoustic/wearable examples. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-ase/twister.json`):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-ase'
  ```

---

## File Structure

- `examples/audio/acoustic-safety-events/src/acoustic_event.{c,h}` — per-frame features + classifier (Tasks 1-2).
- `examples/audio/acoustic-safety-events/src/main.c` — Zephyr glue (Task 3).
- `examples/audio/acoustic-safety-events/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.conf` + `models/README.md` (Task 3).
- `tests/unit/acoustic_event/` — ztest suite (Tasks 1-2).
- `CHANGELOG.md` — entry (Task 3).

---

### Task 1: `acoustic_event` — per-frame DSP feature extraction + host tests

**Files:**
- Create: `examples/audio/acoustic-safety-events/src/acoustic_event.h`
- Create: `examples/audio/acoustic-safety-events/src/acoustic_event.c`
- Create: `tests/unit/acoustic_event/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_acoustic_event.c}`

**Interfaces:**
- Produces (Tasks 2/3): `ASE_FRAME_N 512`, `ASE_SR_HZ 16000.0f`, `ASE_N_BANDS 8`, `ASE_FEATURE_DIM 14`; `struct ase_frame_state`; `struct ase_features`; `ase_frame_reset/push/full`; `ase_feat_extract`; `ase_feat_pack`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/acoustic_event/src/test_acoustic_event.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for acoustic_event (per-frame DSP) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "acoustic_event.h"

ZTEST_SUITE(acoustic_event, NULL, NULL, NULL, NULL, NULL);

static void fill_tone(struct ase_frame_state *st, float freq_hz, float amp)
{
	ase_frame_reset(st);
	for (int i = 0; i < ASE_FRAME_N; i++) {
		ase_frame_push(st, amp * sinf(2.0f * (float)M_PI * freq_hz * (float)i / ASE_SR_HZ));
	}
}

ZTEST(acoustic_event, test_fill_and_pack_dim)
{
	struct ase_frame_state st;
	struct ase_features    f;
	float                  vec[ASE_FEATURE_DIM];

	ase_frame_reset(&st);
	zassert_false(ase_frame_full(&st), "empty frame not full");
	fill_tone(&st, 3000.0f, 0.3f);
	zassert_true(ase_frame_full(&st), "full frame reports full");

	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_equal(ase_feat_pack(&f, vec, ASE_FEATURE_DIM), (size_t)ASE_FEATURE_DIM,
	              "pack writes ASE_FEATURE_DIM");
}

ZTEST(acoustic_event, test_tone_centroid_and_flatness)
{
	struct ase_frame_state st;
	struct ase_features    f;

	fill_tone(&st, 3000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &f);

	zassert_within((double)f.centroid_hz, 3000.0, 250.0, "tone centroid near 3 kHz");
	zassert_true(f.flatness < 0.1f, "a pure tone is very non-flat");
	zassert_within((double)f.rms, 0.2121, 0.02, "0.3 amp tone RMS ~ 0.3/sqrt(2)");
}

ZTEST(acoustic_event, test_zcr_rises_with_frequency)
{
	struct ase_frame_state st;
	struct ase_features    lo, hi;

	fill_tone(&st, 1000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &lo);
	fill_tone(&st, 6000.0f, 0.3f);
	ase_feat_extract(&st, ASE_SR_HZ, &hi);
	zassert_true(hi.zcr > lo.zcr, "ZCR increases with tone frequency");
	zassert_within((double)hi.zcr, 0.75, 0.1, "6 kHz / 16 kHz -> ZCR ~0.75");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/acoustic_event/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_acoustic_event)

set(ASE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/audio/acoustic-safety-events/src)
target_include_directories(app PRIVATE ${ASE_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_acoustic_event.c
    ${ASE_SRC}/acoustic_event.c
)
```

Create `tests/unit/acoustic_event/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/acoustic_event/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.acoustic_event:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - audio
      - safety
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_event` build failure (`acoustic_event.h`/`.c` missing).

- [ ] **Step 4: Write the header**

Create `examples/audio/acoustic-safety-events/src/acoustic_event.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event -- pure-C per-frame DSP feature extraction for the acoustic
 * safety-event classifier.  Arch-neutral (stdint/math only): builds for
 * native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef ACOUSTIC_EVENT_H
#define ACOUSTIC_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASE_FRAME_N     512
#define ASE_SR_HZ       16000.0f
#define ASE_N_BANDS     8
/** 8 band energies + centroid + flatness + rolloff + crest + zcr + rms. */
#define ASE_FEATURE_DIM 14

struct ase_frame_state {
	float    samples[ASE_FRAME_N];
	uint16_t count;
};

struct ase_features {
	float band_energy[ASE_N_BANDS]; /**< normalised log-band energy (sum 1). */
	float centroid_hz;              /**< magnitude-weighted mean frequency. */
	float flatness;                 /**< geo/arith mean: ~1 broadband, ~0 tonal. */
	float rolloff_hz;               /**< freq below which 85% of energy lies. */
	float crest;                    /**< peak/RMS (impulsiveness). */
	float zcr;                      /**< zero-crossing rate (0..1). */
	float rms;                      /**< AC RMS (DC removed). */
};

void   ase_frame_reset(struct ase_frame_state *st);
void   ase_frame_push(struct ase_frame_state *st, float sample);
bool   ase_frame_full(const struct ase_frame_state *st);
void   ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out);
size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_EVENT_H */
```

- [ ] **Step 5: Write the implementation**

Create `examples/audio/acoustic-safety-events/src/acoustic_event.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event implementation -- see acoustic_event.h.
 */
#include "acoustic_event.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void ase_frame_reset(struct ase_frame_state *st)
{
	st->count = 0;
}

void ase_frame_push(struct ase_frame_state *st, float sample)
{
	if (st->count < ASE_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

bool ase_frame_full(const struct ase_frame_state *st)
{
	return st->count >= ASE_FRAME_N;
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

void ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out)
{
	const int n = (st->count < ASE_FRAME_N) ? st->count : ASE_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* DC removal + time-domain features (RMS, crest, ZCR). */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	float sum2 = 0.0f, peak = 0.0f;
	int   zc = 0;
	float prev = st->samples[0] - mean;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		if (fabsf(x) > peak) {
			peak = fabsf(x);
		}
		if (i > 0 && ((x < 0.0f) != (prev < 0.0f))) {
			zc++;
		}
		prev = x;
	}
	out->rms   = sqrtf(sum2 / (float)n);
	out->crest = (out->rms > 1e-6f) ? (peak / out->rms) : 0.0f;
	out->zcr   = (float)zc / (float)n;

	/* Spectrum. */
	static float re[ASE_FRAME_N];
	static float im[ASE_FRAME_N];
	for (int i = 0; i < ASE_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, ASE_FRAME_N);

	const int half = ASE_FRAME_N / 2;
	float     mag[ASE_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f, log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]  = m;
		float fr = (float)k * sr_hz / (float)ASE_FRAME_N;
		mag_total += m;
		centroid_num += fr * m;
		float me = m + 1e-9f;
		log_sum += logf(me);
		lin_sum += me;
		active++;
	}
	out->centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	out->flatness    = (active > 0 && lin_sum > 1e-12f)
	                       ? (expf(log_sum / (float)active) / (lin_sum / (float)active))
	                       : 0.0f;

	/* Spectral rolloff: lowest freq where cumulative |X|^2 >= 85% of total. */
	float total2 = 0.0f;
	for (int k = 1; k < half; k++) {
		total2 += mag[k] * mag[k];
	}
	float cum = 0.0f;
	out->rolloff_hz = 0.0f;
	for (int k = 1; k < half; k++) {
		cum += mag[k] * mag[k];
		if (total2 > 1e-20f && cum >= 0.85f * total2) {
			out->rolloff_hz = (float)k * sr_hz / (float)ASE_FRAME_N;
			break;
		}
	}

	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	if (total2 < 1e-20f) {
		return;
	}
	for (int k = 1; k < half; k++) {
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)ASE_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ASE_N_BANDS) {
			b = ASE_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	for (int b = 0; b < ASE_N_BANDS; b++) {
		out->band_energy[b] /= total2;
	}
}

size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ASE_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ASE_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->centroid_hz;
	vec[i++] = f->flatness;
	vec[i++] = f->rolloff_hz;
	vec[i++] = f->crest;
	vec[i++] = f->zcr;
	vec[i++] = f->rms;
	return i; /* == ASE_FEATURE_DIM */
}
```

- [ ] **Step 6: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_event` PASS, 3/3. (3 kHz at 16000/512 → bin 96 → 3000 Hz exactly; ZCR for 6 kHz ≈ 0.75.)

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/audio/acoustic-safety-events/src/acoustic_event.h \
        examples/audio/acoustic-safety-events/src/acoustic_event.c \
        tests/unit/acoustic_event
git commit -m "feat(ase): acoustic_event per-frame DSP (bands/centroid/flatness/rolloff/crest/zcr) + ztest"
```

---

### Task 2: `acoustic_event` — deterministic 4-class event classifier + host tests

**Files:**
- Modify: `examples/audio/acoustic-safety-events/src/acoustic_event.{h,c}`
- Modify: `tests/unit/acoustic_event/src/test_acoustic_event.c`

**Interfaces:**
- Consumes: `struct ase_features` (Task 1).
- Produces (Task 3): `typedef enum { ASE_AMBIENT=0, ASE_GLASS_BREAK=1, ASE_ALARM=2, ASE_SCREAM=3, ASE_EVENT_COUNT } ase_event_t;` `struct ase_verdict { ase_event_t ev; float confidence; };` `struct ase_verdict ase_classify_fallback(const struct ase_features *f);` `const char *ase_event_name(ase_event_t e);`

- [x] **Step 1: Write the failing test**

Append to `tests/unit/acoustic_event/src/test_acoustic_event.c`:

```c
/* A decaying 6 kHz tone -> impulsive HF (high crest + centroid + ZCR). */
static void fill_glass(struct ase_frame_state *st)
{
	ase_frame_reset(st);
	for (int i = 0; i < ASE_FRAME_N; i++) {
		float env = expf(-(float)i * 0.02f);
		ase_frame_push(st, env * sinf(2.0f * (float)M_PI * 6000.0f * (float)i / ASE_SR_HZ));
	}
}

/* Harmonic voiced: 800 Hz fundamental + 3 harmonics, high energy (scream). */
static void fill_scream(struct ase_frame_state *st)
{
	ase_frame_reset(st);
	for (int i = 0; i < ASE_FRAME_N; i++) {
		float t = (float)i / ASE_SR_HZ;
		float s = 1.0f * sinf(2.0f * (float)M_PI * 800.0f * t) +
		          0.6f * sinf(2.0f * (float)M_PI * 1600.0f * t) +
		          0.4f * sinf(2.0f * (float)M_PI * 2400.0f * t) +
		          0.3f * sinf(2.0f * (float)M_PI * 3200.0f * t);
		ase_frame_push(st, 0.25f * s);
	}
}

ZTEST(acoustic_event, test_classify_ambient)
{
	struct ase_frame_state st;
	struct ase_features    f;
	ase_frame_reset(&st);
	for (int i = 0; i < ASE_FRAME_N; i++) {
		ase_frame_push(&st, 0.002f * sinf((float)i * 0.3f));
	}
	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_equal(ase_classify_fallback(&f).ev, ASE_AMBIENT, "quiet -> AMBIENT");
}

ZTEST(acoustic_event, test_classify_glass_break)
{
	struct ase_frame_state st;
	struct ase_features    f;
	fill_glass(&st);
	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_true(f.crest > 4.0f, "glass burst is impulsive");
	zassert_equal(ase_classify_fallback(&f).ev, ASE_GLASS_BREAK, "HF impulsive -> GLASS_BREAK");
}

ZTEST(acoustic_event, test_classify_alarm)
{
	struct ase_frame_state st;
	struct ase_features    f;
	fill_tone(&st, 3000.0f, 0.3f); /* steady 3 kHz beep */
	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_equal(ase_classify_fallback(&f).ev, ASE_ALARM, "3 kHz tonal -> ALARM");
}

ZTEST(acoustic_event, test_classify_scream)
{
	struct ase_frame_state st;
	struct ase_features    f;
	fill_scream(&st);
	ase_feat_extract(&st, ASE_SR_HZ, &f);
	zassert_equal(ase_classify_fallback(&f).ev, ASE_SCREAM, "harmonic voiced -> SCREAM");
}

ZTEST(acoustic_event, test_event_name)
{
	zassert_true(strcmp(ase_event_name(ASE_AMBIENT), "AMBIENT") == 0, "name");
	zassert_true(strcmp(ase_event_name(ASE_GLASS_BREAK), "GLASS_BREAK") == 0, "name");
	zassert_true(strcmp(ase_event_name(ASE_SCREAM), "SCREAM") == 0, "name");
}
```

Add `#include <string.h>` at the top of the test if not already present.

- [x] **Step 2: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: build failure — `ase_classify_fallback`/`ase_verdict`/`ase_event_t`/`ase_event_name` undeclared.

- [x] **Step 3: Add the API to the header**

Insert into `acoustic_event.h` before the closing `#ifdef __cplusplus }`:

```c
/** Safety/security sound-event taxonomy (reference-grade). */
typedef enum {
	ASE_AMBIENT     = 0,
	ASE_GLASS_BREAK = 1,
	ASE_ALARM       = 2,
	ASE_SCREAM      = 3,
	ASE_EVENT_COUNT
} ase_event_t;

struct ase_verdict {
	ase_event_t ev;
	float       confidence; /**< 0..1 */
};

/**
 * Deterministic event classifier over the feature vector.  Runs when no AI
 * model is loaded.  Reference-grade threshold rules; customers retrain/retune.
 */
struct ase_verdict ase_classify_fallback(const struct ase_features *f);

/** Stable upper-case event name for the record. */
const char *ase_event_name(ase_event_t e);
```

- [x] **Step 4: Implement**

Append to `acoustic_event.c`:

```c
struct ase_verdict ase_classify_fallback(const struct ase_features *f)
{
	struct ase_verdict v = { ASE_AMBIENT, 0.6f };

	if (f->rms < 0.02f) {
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.8f;
	} else if (f->crest > 4.0f && f->centroid_hz > 4000.0f && f->zcr > 0.4f) {
		/* Broadband impulsive high-frequency burst. */
		v.ev         = ASE_GLASS_BREAK;
		v.confidence = 0.85f;
	} else if (f->flatness < 0.2f && f->centroid_hz >= 2500.0f && f->centroid_hz <= 4000.0f) {
		/* Narrowband tonal beep in the alarm band. */
		v.ev         = ASE_ALARM;
		v.confidence = 0.9f;
	} else if (f->rms > 0.1f && f->centroid_hz >= 800.0f && f->centroid_hz < 2500.0f) {
		/* High-energy harmonic voice band. */
		v.ev         = ASE_SCREAM;
		v.confidence = 0.75f;
	} else {
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.5f;
	}
	return v;
}

const char *ase_event_name(ase_event_t e)
{
	switch (e) {
	case ASE_AMBIENT:
		return "AMBIENT";
	case ASE_GLASS_BREAK:
		return "GLASS_BREAK";
	case ASE_ALARM:
		return "ALARM";
	case ASE_SCREAM:
		return "SCREAM";
	default:
		return "UNKNOWN";
	}
}
```

- [x] **Step 5: Run GREEN**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.acoustic_event` PASS, 8/8. If a classify case misses, inspect the offending feature against the branch thresholds (glass: crest>4 & centroid>4000 & zcr>0.4; alarm: flatness<0.2 & centroid 2500–4000; scream: rms>0.1 & centroid 800–2500) — debug the synthetic generator or the feature, do NOT loosen a threshold without re-checking all four classes still separate.

- [x] **Step 6: Format + commit**

```bash
git add examples/audio/acoustic-safety-events/src/acoustic_event.h \
        examples/audio/acoustic-safety-events/src/acoustic_event.c \
        tests/unit/acoustic_event/src/test_acoustic_event.c
git commit -m "feat(ase): deterministic 4-class event classifier (ambient/glass/alarm/scream) + tests"
```

---

### Task 3: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/audio/acoustic-safety-events/src/main.c`
- Create: `examples/audio/acoustic-safety-events/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/audio/acoustic-safety-events/boards/native_sim_native_64.conf`
- Create: `examples/audio/acoustic-safety-events/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `acoustic_event.h`; portable `<alp/audio.h>`, `<alp/inference.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `ASE,...` record per frame, ending `[ase] done`.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/audio/acoustic-safety-events/CMakeLists.txt`:
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
list(APPEND OVERLAY_CONFIG ${_alp_generated})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(acoustic_safety_events LANGUAGES C)

target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/main.c
    src/acoustic_event.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/audio/acoustic-safety-events/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml** (mirror audio-wake-word: M55-HE always-on, PDM via DMIC, no `peripherals` block)

Create `examples/audio/acoustic-safety-events/board.yaml`:
```yaml
# board.yaml -- acoustic safety-event classifier.
#
# An always-on low-power M55-HE node listens to a PDM mic, extracts per-frame
# DSP features, and classifies the sound event (ambient / glass-break / alarm /
# scream) with a small NPU model.  Same source targets the V2N DEEPX path when
# som.sku is flipped.

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

# PDM mic input is the Zephyr DMIC audio subsystem (not a peripheral enum),
# wired via CONFIG_AUDIO_DMIC=y in testcase.yaml -- same convention as
# audio-wake-word.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim conf**

Create `examples/audio/acoustic-safety-events/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# Link the DMIC audio layer so alp_audio_in_open resolves; no DMIC device is
# present on native_sim, so it returns NULL and main.c uses synthetic audio.
CONFIG_AUDIO=y
CONFIG_AUDIO_DMIC=y
```

(No native_sim DT overlay is needed — the audio layer is linked via the conf
above + the testcase `extra_args`, and main.c tolerates a NULL mic.)

- [ ] **Step 5: Write main.c**

Create `examples/audio/acoustic-safety-events/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic-safety-events
 * ======================
 *
 * Always-listening safety/security node.  Pipeline:
 *
 *   PDM mic (<alp/audio.h>, 16 kHz) --512-sample frame-->
 *     acoustic_event (bands/centroid/flatness/rolloff/crest/zcr/rms)
 *       -> <alp/inference.h> 4-class event classifier (deterministic fallback)
 *     --> one ASE record per frame.
 *
 * Honest scope: detects loud, acoustically-distinct events (glass-break /
 * alarm / scream / ambient).  NOT a certified security or life-safety sensor;
 * confounders (music, TV, clattering) cause false positives.  The model is a
 * stub (see models/README.md); with no model the deterministic fallback runs.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/audio.h"
#include "alp/inference.h"

#include "acoustic_event.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(ase, LOG_LEVEL_INF);

#define N_FRAMES 8

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Synthetic audio per frame: cycle ambient / glass / alarm / scream. */
static float synth_sample(int frame, int i)
{
	float t = (float)i / ASE_SR_HZ;
	switch (frame % 4) {
	case 0: /* ambient */
		return 0.002f * sinf((float)i * 0.3f);
	case 1: { /* glass: decaying 6 kHz */
		float env = expf(-(float)i * 0.02f);
		return env * sinf(2.0f * (float)M_PI * 6000.0f * t);
	}
	case 2: /* alarm: steady 3 kHz */
		return 0.3f * sinf(2.0f * (float)M_PI * 3000.0f * t);
	default: /* scream: harmonic voiced */
		return 0.25f * (1.0f * sinf(2.0f * (float)M_PI * 800.0f * t) +
		                0.6f * sinf(2.0f * (float)M_PI * 1600.0f * t) +
		                0.4f * sinf(2.0f * (float)M_PI * 2400.0f * t) +
		                0.3f * sinf(2.0f * (float)M_PI * 3200.0f * t));
	}
}

static struct ase_verdict classify(alp_inference_t *inf, const struct ase_features *f)
{
	if (inf != NULL) {
		float vec[ASE_FEATURE_DIM];
		(void)ase_feat_pack(f, vec, ASE_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= ASE_EVENT_COUNT * sizeof(float)) {
					const float *sc = (const float *)out.data;
					int   best = 0;
					float bv   = sc[0];
					for (int k = 1; k < ASE_EVENT_COUNT; k++) {
						if (sc[k] > bv) {
							bv   = sc[k];
							best = k;
						}
					}
					return (struct ase_verdict){ (ase_event_t)best, bv };
				}
			}
		}
	}
	return ase_classify_fallback(f);
}

int main(void)
{
	static struct ase_frame_state frame;
	int16_t pcm[ASE_FRAME_N];

	alp_audio_in_t *mic = alp_audio_in_open(&(alp_audio_config_t){
	    .peripheral_id    = E1M_PDM0,
	    .sample_rate_hz   = 16000,
	    .channels         = 1,
	    .format           = ALP_AUDIO_FMT_S16_LE,
	    .frames_per_block = ASE_FRAME_N,
	});
	if (mic != NULL) {
		alp_audio_in_start(mic);
	} else {
		LOG_WRN("PDM mic unavailable; using synthetic event audio");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# ASE,t_s,event,confidence,centroid_hz,rms\n");

	for (int w = 0; w < N_FRAMES; w++) {
		ase_frame_reset(&frame);
		size_t got = 0;
		bool   have_pcm =
		    (mic != NULL && alp_audio_in_read(mic, pcm, ASE_FRAME_N, &got, 50) == ALP_OK && got > 0);
		for (int i = 0; i < ASE_FRAME_N; i++) {
			float s = have_pcm ? ((float)pcm[i % (int)got] / 32768.0f) : synth_sample(w, i);
			ase_frame_push(&frame, s);
		}

		struct ase_features f;
		ase_feat_extract(&frame, ASE_SR_HZ, &f);
		struct ase_verdict v = classify(inf, &f);

		printk("ASE,%.2f,%s,%.2f,%.1f,%.2f\n", (double)(w * 0.032f), ase_event_name(v.ev),
		       (double)v.confidence, (double)f.centroid_hz, (double)f.rms);
	}

	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (mic != NULL) {
		alp_audio_in_stop(mic);
		alp_audio_in_close(mic);
	}
	printk("[ase] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers + the sibling `examples/audio/audio-wake-word/src/main.c` (it opens the PDM mic the same way: `alp_audio_in_open(&alp_audio_config_t{.peripheral_id=E1M_PDM0, .sample_rate_hz, .channels, .format=ALP_AUDIO_FMT_S16_LE, .frames_per_block})` + `alp_audio_in_start/read/stop/close`). The `alp_inference_*` calls per the wake-word/vibration examples. This app does NOT include `<alp/board.h>`. Add `<string.h>`/`<math.h>` if a symbol is unresolved. Keep `<alp/*>` portable — no vendor names.

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only` — no ALP_BOARD define needed since no board.h)

Create `examples/audio/acoustic-safety-events/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: acoustic-safety-events
  description: |
    Always-listening safety/security node: PDM mic -> per-frame DSP features
    -> 4-class sound-event classifier (ambient/glass-break/alarm/scream) with
    a deterministic fallback.  native_sim runs synthetic audio cycling the
    four event types.
common:
  tags: ai inference audio safety security marketing showcase
tests:
  alp_sdk.example.acoustic_safety_events.native_sim:
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
      - safety
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[ase\\] done"

  alp_sdk.example.acoustic_safety_events.aen_build:
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - audio
      - ai
      - safety
```

> Contingency: if the native_sim build cannot RUN to completion for an audio-subsystem reason, make the `native_sim` test `build_only: true` too and report it — the unit suite is the load-bearing host validation regardless.

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/audio/acoustic-safety-events/models/README.md`:
```markdown
# Event model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic threshold
classifier runs without one. For robust field detection, train a small
**4-class classifier** (ambient / glass-break / alarm / scream):

1. **Collect labelled clips** and window them to the device's 512-sample @ 16 kHz
   frame. Public starting datasets: **UrbanSound8K**, **ESC-50**, and AudioSet
   glass-break / scream subsets. Augment with your deployment's background noise.
2. **Extract the `ASE_FEATURE_DIM` feature vector** per frame (8 bands + centroid,
   flatness, rolloff, crest, ZCR, RMS) — or train directly on a log-mel
   spectrogram if you prefer a CNN.
3. **Train** a small dense/CNN classifier; calibrate the decision threshold on a
   held-out set to control the false-alarm rate.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Honest scope: detects loud, acoustically-distinct events; NOT a certified
security or life-safety sensor. Confounders (music, TV, clattering dishes) drive
false positives — real deployment needs the trained model + noise augmentation.
```

- [ ] **Step 8: Write README.md**

Create `examples/audio/acoustic-safety-events/README.md`:
```markdown
# acoustic-safety-events

> ⚠️ **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `acoustic_event`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic audio cycling the four event types.
> HiL with a real mic + a trained model is bench-gated.

An always-listening safety/security node: a MEMS mic captures audible-band
sound, DSP extracts per-frame features, and a small NPU model classifies the
**sound event** -- `AMBIENT`, `GLASS_BREAK`, `ALARM` (smoke/CO beep), `SCREAM`.

## Honest scope

Detects loud, acoustically-distinct events. **NOT** a certified security /
life-safety sensor. Real-world confounders (music, TV, clattering dishes, door
slams) cause false positives; robust deployment needs a model trained on real
data -- the deterministic fallback is coarse threshold rules.

## Discriminators

- `GLASS_BREAK` -- broadband impulsive HF burst (high crest + centroid + ZCR).
- `ALARM` -- narrowband ~3 kHz tonal beep (very low spectral flatness).
- `SCREAM` -- voiced harmonic, high energy 1-4 kHz.
- `AMBIENT` -- low RMS baseline.

## Pipeline

```
PDM mic (<alp/audio.h>, 16 kHz) --frame--> acoustic_event (bands/centroid/
  flatness/rolloff/crest/zcr/rms) -> <alp/inference.h> 4-class classifier
  (deterministic fallback) -> ASE record per frame
```

## Output

```
# ASE,t_s,event,confidence,centroid_hz,rms
ASE,0.06,GLASS_BREAK,0.85,5400.0,0.16
ASE,0.10,ALARM,0.90,3000.0,0.21
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/audio/acoustic-safety-events
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fallback). See `models/README.md` for
the training recipe (UrbanSound8K / ESC-50 / AudioSet subsets).

## Tests

```
twister -p native_sim/native/64 -T tests/unit/acoustic_event
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Acoustic safety-event example** (`examples/audio/acoustic-safety-events/`):
  always-listening security/safety node — PDM mic → per-frame `acoustic_event`
  DSP (8 FFT band energies, spectral centroid/flatness/rolloff, crest factor,
  zero-crossing rate, RMS) → a deterministic 4-class event classifier
  (AMBIENT/GLASS_BREAK/ALARM/SCREAM) + an `<alp/inference.h>` classifier with a
  deterministic fallback. The core is host-unit-tested on `native_sim`
  (`tests/unit/acoustic_event`); model is a stub with a training recipe in
  `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.acoustic_event` (8/8) PASS.
- `alp_sdk.example.acoustic_safety_events.native_sim` PASS on `native_sim/native/64` (console `[ase] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-ase/twister.json`. Verify the emitted ASE records show a sensible event spread (AMBIENT/GLASS_BREAK/ALARM/SCREAM across the 8 frames) — paste a few into the report. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 5 notes) — do NOT change the portable-API contract or the core's logic. The local AEN link env may hit the shared `alp_backends_*` orphan-section issue (same as sibling examples); if AEN fails ONLY with that, note it — CI is the AEN gate.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22, then:
```bash
git add examples/audio/acoustic-safety-events CHANGELOG.md
git commit -m "feat(ase): acoustic safety-event example app (mic DSP + 4-class classifier) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 acoustic_event → Task 1; 4-class classifier (C1) → Task 2; C2 AI dispatch + C3 main.c + scaffolding + models/README + README + CHANGELOG → Task 3. Output record + taxonomy → Task 3 (main.c + README). Validation (one ztest suite + native_sim run) → Tasks 1-2 tests + Task 3 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 3 board.yaml + testcase.yaml. Honest scope → Task 3 README + models/README. No-board.h / CONFIG_AUDIO_DMIC → Task 3 testcase + native_sim conf. All spec sections map to a task.

**Type consistency:** `ASE_FRAME_N 512`, `ASE_SR_HZ 16000.0f`, `ASE_N_BANDS 8`, `ASE_FEATURE_DIM 14`, `ASE_EVENT_COUNT 4` consistent across header/impl/tests/main. `ase_frame_state/ase_features/ase_frame_reset/push/full/ase_feat_extract/ase_feat_pack/ase_event_t/ase_verdict/ase_classify_fallback/ase_event_name` — names + signatures identical across tasks. `ase_feat_pack` writes exactly 14 in the documented order (band[0..7], centroid, flatness, rolloff, crest, zcr, rms). Output schema (6 columns) identical in main.c + README + spec. Classifier thresholds in Task 2 match the synthetic generators in Task 2's tests + main.c's synth_sample (glass 6 kHz decaying → crest>4/centroid>4000/zcr>0.4; alarm 3 kHz → flatness<0.2/centroid 2500–4000; scream 800+harmonics → rms>0.1/centroid 800–2500; ambient → rms<0.02). The classify branch ORDER (ambient → glass → alarm → scream) prevents a loud HF impulsive event from being mislabeled.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The 1-byte model stub + the synthetic generators are deliberate, documented design decisions.
```
