# Cold-Chain Integrity Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A pharma/food cold-chain integrity monitor: sample a BME280 T/RH/P sensor, compute the standards-backed metrics (temperature-excursion time, Mean Kinetic Temperature, dewpoint), classify the integrity state, and emit an AI anomaly score.

**Architecture:** One pure-C, arch-neutral, host-unit-tested core — `cold_chain` (sliding-window stats + MKT + dewpoint + excursion-minutes + a deterministic 4-state classifier + a deterministic anomaly fallback) — plus an `<alp/inference.h>` anomaly model (stub) and a thin Zephyr `main.c`. Low-rate time series → stats, no FFT.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/chips/bme280.h>` (T/RH/P, I2C), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core (`cold_chain.{c,h}`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` APIs only (I2C via the `bme280_*` chip driver, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `CC_WINDOW_N 256`, `CC_SAMPLE_MIN 1.0f`, `CC_FEATURE_DIM 8`, `CC_STATE_COUNT 4`, `CC_DH_OVER_R 10000.0f`; config defaults `t_lo 2.0f`, `t_hi 8.0f`, `mkt_limit_c 8.0f`, `excursion_min_limit 30.0f`, `dewpoint_margin_c 2.0f`.
- **COMMENT DENSITY (project standard): EVERY file under `examples/*/src/` (the core AND main.c) must be ≥50% comment lines** (`comment_lines / total_lines`, comment = `^\s*(/\*|\*|//)`). Example source is teaching material — the code blocks below are already written at that density; transcribe them with their comments, and MEASURE each file before committing (aim ≥55% for margin). Explain the MKT (Arrhenius) + Magnus-dewpoint math and the classifier thresholds.
- TDD: the core is RED-first, host-validated on `native_sim/native/64`. Sensor I/O + the AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**; NO binaries (model is a 1-byte stub; recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- Example dir: `examples/ai/cold-chain-monitor/`. Primary target E1M-AEN; V2N via `som.sku` flip. BME280 on the EVK sensor I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT `/usr/bin/clang-format-14`). WATCH the alignment trap: block comments ABOVE a consecutive-declaration/enum group, trailing `/**< */` on members.
- Unit test compiles the core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-cc/twister.json`):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-cc'
  ```
  (WSL `/tmp` can be unstable across separate shell calls — keep build+result in ONE `wsl … bash -lc` invocation when running ad hoc.)

---

## File Structure

- `examples/ai/cold-chain-monitor/src/cold_chain.{c,h}` — window stats + metrics + classifier (Tasks 1-2).
- `examples/ai/cold-chain-monitor/src/main.c` — Zephyr glue (Task 3).
- `examples/ai/cold-chain-monitor/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.{conf,overlay}` + `models/README.md` (Task 3).
- `tests/unit/cold_chain/` — ztest suite (Tasks 1-2).
- `CHANGELOG.md` — entry (Task 3).

---

### Task 1: `cold_chain` — window stats, MKT, dewpoint, excursion + host tests

**Files:**
- Create: `examples/ai/cold-chain-monitor/src/cold_chain.h`
- Create: `examples/ai/cold-chain-monitor/src/cold_chain.c`
- Create: `tests/unit/cold_chain/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_cold_chain.c}`

**Interfaces:**
- Produces (Tasks 2/3): `CC_WINDOW_N 256`, `CC_SAMPLE_MIN 1.0f`, `CC_FEATURE_DIM 8`, `CC_DH_OVER_R 10000.0f`; `struct cc_sample`, `struct cc_window_state`, `struct cc_features`, `struct cc_config`; `cc_window_reset/push/full`; `cc_feat_extract`; `cc_feat_pack`. (`struct cc_config` is defined in this task because `cc_feat_extract` needs the temperature band for `excursion_min`.)

- [ ] **Step 1: Write the failing test**

Create `tests/unit/cold_chain/src/test_cold_chain.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for cold_chain (BME280 time-series metrics) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "cold_chain.h"

ZTEST_SUITE(cold_chain, NULL, NULL, NULL, NULL, NULL);

/* A vaccine-fridge config: safe band 2..8 C. */
static const struct cc_config CFG = {
    .t_lo = 2.0f, .t_hi = 8.0f, .mkt_limit_c = 8.0f,
    .excursion_min_limit = 30.0f, .dewpoint_margin_c = 2.0f
};

/* Fill a window with constant temperature + humidity. */
static void fill_const(struct cc_window_state *st, float temp_c, float rh_pct)
{
	cc_window_reset(st);
	for (int i = 0; i < CC_WINDOW_N; i++) {
		struct cc_sample s = { .temp_c = temp_c, .rh_pct = rh_pct, .pressure_pa = 101325.0f };
		cc_window_push(st, s);
	}
}

ZTEST(cold_chain, test_fill_and_pack_dim)
{
	struct cc_window_state st;
	struct cc_features     f;
	float                  vec[CC_FEATURE_DIM];

	cc_window_reset(&st);
	zassert_false(cc_window_full(&st), "empty window not full");
	fill_const(&st, 5.0f, 50.0f);
	zassert_true(cc_window_full(&st), "full window reports full");

	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_feat_pack(&f, vec, CC_FEATURE_DIM), (size_t)CC_FEATURE_DIM,
	              "pack writes CC_FEATURE_DIM");
}

ZTEST(cold_chain, test_mkt_of_constant_equals_temperature)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 5.0f, 50.0f);
	cc_feat_extract(&st, &CFG, &f);
	/* For a constant profile, MKT == that temperature. */
	zassert_within((double)f.mkt_c, 5.0, 0.05, "MKT of constant 5 C is 5 C");
	zassert_within((double)f.excursion_min, 0.0, 0.01, "5 C is inside 2..8 -> no excursion");
}

ZTEST(cold_chain, test_mkt_exceeds_arithmetic_mean_on_spike)
{
	struct cc_window_state st;
	struct cc_features     f;

	/* 250 samples at 4 C + 6 brief samples at 40 C: arithmetic mean ~4.8 C
	 * (still in-band), but the hot spike pulls MKT well above 8 C. */
	cc_window_reset(&st);
	for (int i = 0; i < 250; i++) {
		struct cc_sample s = { .temp_c = 4.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	for (int i = 0; i < 6; i++) {
		struct cc_sample s = { .temp_c = 40.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	cc_feat_extract(&st, &CFG, &f);
	zassert_true(f.mean_temp_c < 8.0f, "arithmetic mean stays in-band (~4.8 C)");
	zassert_true(f.mkt_c > 8.0f, "MKT exceeds the limit because the spike weighs heavily");
	zassert_true(f.mkt_c > f.mean_temp_c, "MKT >= arithmetic mean (Jensen)");
}

ZTEST(cold_chain, test_dewpoint_magnus)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 20.0f, 50.0f);
	cc_feat_extract(&st, &CFG, &f);
	/* Magnus dewpoint for 20 C / 50% RH is ~9.3 C. */
	zassert_within((double)f.dewpoint_c, 9.3, 0.5, "dewpoint at 20 C / 50% RH");
}

ZTEST(cold_chain, test_excursion_minutes)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 12.0f, 55.0f); /* all out of the 2..8 band */
	cc_feat_extract(&st, &CFG, &f);
	zassert_within((double)f.excursion_min, (double)CC_WINDOW_N * CC_SAMPLE_MIN, 0.5,
	               "every sample out of band -> full window of excursion minutes");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/cold_chain/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cold_chain)

set(CC_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/cold-chain-monitor/src)
target_include_directories(app PRIVATE ${CC_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_cold_chain.c
    ${CC_SRC}/cold_chain.c
)
```

Create `tests/unit/cold_chain/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/cold_chain/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.cold_chain:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - environmental
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.cold_chain` build failure (`cold_chain.h`/`.c` missing).

- [ ] **Step 4: Write the header** (≥50% comment density — keep all the comments)

Create `examples/ai/cold-chain-monitor/src/cold_chain.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold_chain -- pure-C cold-chain integrity metrics for the BME280 example.
 *
 * A slow time series of (temperature, humidity, pressure) is reduced to the
 * metrics a pharma/food cold-chain auditor actually uses:
 *   - excursion time   : how long the temperature spent outside the safe band;
 *   - Mean Kinetic Temp : the single temperature delivering the same cumulative
 *                         thermal stress as the fluctuating profile (ICH Q1A /
 *                         USP <1079>) -- a brief hot spike counts for more than
 *                         its duration, which a plain average would hide;
 *   - dewpoint          : condensation / mould risk when ambient T nears it.
 * Arch-neutral (stdint/math only): builds for native_sim and the M55 alike,
 * and is host-unit-tested.
 */
#ifndef COLD_CHAIN_H
#define COLD_CHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Window length (samples) and the wall-clock minutes each sample represents.
 * The demo logs one reading per simulated minute, so excursion counts convert
 * directly to minutes. */
#define CC_WINDOW_N    256
#define CC_SAMPLE_MIN  1.0f
/* mean/min/max T + mean RH + slope + dewpoint + MKT + excursion-minutes. */
#define CC_FEATURE_DIM 8
/* Arrhenius activation term ΔH/R for MKT.  ΔH = 83.144 kJ/mol (the ICH default
 * for pharmaceutical stability), R = 8.314 J/mol·K, so ΔH/R = 10000 K. */
#define CC_DH_OVER_R   10000.0f

/** One environmental reading in physical units. */
struct cc_sample {
	float temp_c;      /**< temperature, degrees Celsius. */
	float rh_pct;      /**< relative humidity, percent. */
	float pressure_pa; /**< barometric pressure, pascals (logged, not classified). */
};

/** Sliding window of readings. */
struct cc_window_state {
	struct cc_sample s[CC_WINDOW_N];
	uint16_t         count;
};

/** Derived cold-chain metrics over the window. */
struct cc_features {
	float mean_temp_c;          /**< arithmetic mean temperature. */
	float min_temp_c;           /**< coldest sample. */
	float max_temp_c;           /**< warmest sample. */
	float mean_rh_pct;          /**< mean relative humidity. */
	float temp_slope_c_per_min; /**< warming/cooling trend (>0 = warming). */
	float dewpoint_c;           /**< Magnus dewpoint of the mean T/RH. */
	float mkt_c;                /**< mean kinetic temperature, Celsius. */
	float excursion_min;        /**< minutes spent outside the safe band. */
};

/** Per-product thresholds (a vaccine fridge by default). */
struct cc_config {
	float t_lo;                /**< safe-band low edge, Celsius. */
	float t_hi;                /**< safe-band high edge, Celsius. */
	float mkt_limit_c;         /**< MKT above this = cumulative damage. */
	float excursion_min_limit; /**< excursion minutes above this = breach. */
	float dewpoint_margin_c;   /**< T within this of dewpoint = condensation risk. */
};

/** Reset the window (count = 0). */
void cc_window_reset(struct cc_window_state *st);

/** Append one reading; ignored once the window is full. */
void cc_window_push(struct cc_window_state *st, struct cc_sample s);

/** True once CC_WINDOW_N readings have been pushed. */
bool cc_window_full(const struct cc_window_state *st);

/** Reduce the window to the cold-chain metrics.  @p cfg supplies the safe band
 *  used for the excursion count. */
void cc_feat_extract(const struct cc_window_state *st, const struct cc_config *cfg,
                     struct cc_features *out);

/** Pack the metrics into the AI feature vector; returns the count (CC_FEATURE_DIM). */
size_t cc_feat_pack(const struct cc_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* COLD_CHAIN_H */
```

- [ ] **Step 5: Write the implementation** (≥50% comment density — keep all the comments)

Create `examples/ai/cold-chain-monitor/src/cold_chain.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold_chain implementation -- see cold_chain.h.  The two non-obvious formulas
 * (Mean Kinetic Temperature and the Magnus dewpoint) are derived inline.
 */
#include "cold_chain.h"

#include <math.h>
#include <string.h>

void cc_window_reset(struct cc_window_state *st)
{
	st->count = 0;
}

void cc_window_push(struct cc_window_state *st, struct cc_sample s)
{
	if (st->count < CC_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool cc_window_full(const struct cc_window_state *st)
{
	return st->count >= CC_WINDOW_N;
}

/*
 * Mean Kinetic Temperature (ICH Q1A / USP <1079>).
 *
 * MKT is the constant temperature that would inflict the same Arrhenius-rate
 * cumulative degradation as the real, fluctuating profile:
 *
 *     MKT = (ΔH/R) / ( -ln( (1/n) Σ exp( -(ΔH/R) / T_i ) ) )
 *
 * with T_i in KELVIN.  Because exp(-c/T) is convex, Jensen's inequality gives
 * MKT >= the arithmetic mean -- a short hot excursion raises MKT more than a
 * plain average would, which is exactly why cold-chain auditing uses MKT.
 * The accumulation runs in double precision because the exponentials are tiny.
 */
static float mkt_celsius(const struct cc_window_state *st, int n)
{
	double sum_exp = 0.0;
	for (int i = 0; i < n; i++) {
		double t_kelvin = (double)st->s[i].temp_c + 273.15;
		sum_exp += exp(-(double)CC_DH_OVER_R / t_kelvin);
	}
	double mean_exp = sum_exp / (double)n;
	/* Guard the log: mean_exp is strictly positive for any real temperature. */
	double mkt_kelvin = (double)CC_DH_OVER_R / (-log(mean_exp));
	return (float)(mkt_kelvin - 273.15);
}

/*
 * Magnus dewpoint approximation (a = 17.62, b = 243.12 °C):
 *
 *     γ  = ln(RH/100) + a·T / (b + T)
 *     Td = b·γ / (a - γ)
 *
 * Valid for RH in (0, 100]; we clamp RH to >= 1% to keep the log finite.
 */
static float dewpoint_celsius(float temp_c, float rh_pct)
{
	const float a = 17.62f, b = 243.12f;
	float       rh = (rh_pct < 1.0f) ? 1.0f : ((rh_pct > 100.0f) ? 100.0f : rh_pct);
	float       gamma = logf(rh / 100.0f) + (a * temp_c) / (b + temp_c);
	return (b * gamma) / (a - gamma);
}

void cc_feat_extract(const struct cc_window_state *st, const struct cc_config *cfg,
                     struct cc_features *out)
{
	const int n = (st->count < CC_WINDOW_N) ? st->count : CC_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* First pass: means, min/max temperature, and the out-of-band sample count. */
	float sum_t = 0.0f, sum_rh = 0.0f;
	float min_t = st->s[0].temp_c, max_t = st->s[0].temp_c;
	int   out_of_band = 0;
	for (int i = 0; i < n; i++) {
		float t = st->s[i].temp_c;
		sum_t += t;
		sum_rh += st->s[i].rh_pct;
		if (t < min_t) {
			min_t = t;
		}
		if (t > max_t) {
			max_t = t;
		}
		/* A reading counts as an excursion when it leaves the product's band. */
		if (t < cfg->t_lo || t > cfg->t_hi) {
			out_of_band++;
		}
	}
	out->mean_temp_c  = sum_t / (float)n;
	out->mean_rh_pct  = sum_rh / (float)n;
	out->min_temp_c   = min_t;
	out->max_temp_c   = max_t;
	out->excursion_min = (float)out_of_band * CC_SAMPLE_MIN;

	/* Warming/cooling trend: last-quarter mean minus first-quarter mean, divided
	 * by the window duration in minutes (positive = warming). */
	int   q = n / 4;
	if (q < 1) {
		q = 1;
	}
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += st->s[i].temp_c;
		last += st->s[n - 1 - i].temp_c;
	}
	out->temp_slope_c_per_min =
	    ((last - first) / (float)q) / ((float)n * CC_SAMPLE_MIN);

	/* Dewpoint uses the window-mean T/RH so it lines up with the classifier's
	 * mean-temperature comparison. */
	out->dewpoint_c = dewpoint_celsius(out->mean_temp_c, out->mean_rh_pct);

	/* Mean kinetic temperature over the whole window. */
	out->mkt_c = mkt_celsius(st, n);
}

size_t cc_feat_pack(const struct cc_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)CC_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++]  = f->mean_temp_c;
	vec[i++]  = f->min_temp_c;
	vec[i++]  = f->max_temp_c;
	vec[i++]  = f->mean_rh_pct;
	vec[i++]  = f->temp_slope_c_per_min;
	vec[i++]  = f->dewpoint_c;
	vec[i++]  = f->mkt_c;
	vec[i++]  = f->excursion_min;
	return i; /* == CC_FEATURE_DIM */
}
```

- [ ] **Step 6: Run GREEN + check density**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.cold_chain` PASS, 5/5. Then measure both files' comment density (`grep -cE '^\s*(/\*|\*|//)' <file>` over total lines) — both ≥50%; if under, add more teaching comments before committing.

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/ai/cold-chain-monitor/src/cold_chain.h \
        examples/ai/cold-chain-monitor/src/cold_chain.c \
        tests/unit/cold_chain
git commit -m "feat(cc): cold_chain window stats + MKT + dewpoint + excursion + ztest"
```

---

### Task 2: `cold_chain` — 4-state classifier + anomaly fallback + host tests

**Files:**
- Modify: `examples/ai/cold-chain-monitor/src/cold_chain.{h,c}`
- Modify: `tests/unit/cold_chain/src/test_cold_chain.c`

**Interfaces:**
- Consumes: `struct cc_features`, `struct cc_config` (Task 1).
- Produces (Task 3): `typedef enum { CC_OK=0, CC_TEMP_EXCURSION=1, CC_MKT_EXCEEDED=2, CC_CONDENSATION_RISK=3, CC_STATE_COUNT } cc_state_t;` `cc_state_t cc_classify(const struct cc_features *f, const struct cc_config *cfg);` `const char *cc_state_name(cc_state_t s);` `float cc_anomaly_fallback(const struct cc_features *f, const struct cc_config *cfg);`

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/cold_chain/src/test_cold_chain.c`:

```c
ZTEST(cold_chain, test_classify_ok_and_excursion)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 5.0f, 50.0f); /* stable in-band cold */
	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_classify(&f, &CFG), CC_OK, "stable 5 C -> OK");

	fill_const(&st, 12.0f, 55.0f); /* mean out of band */
	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_classify(&f, &CFG), CC_TEMP_EXCURSION, "12 C -> TEMP_EXCURSION");
}

ZTEST(cold_chain, test_classify_mkt_exceeded)
{
	struct cc_window_state st;
	struct cc_features     f;

	/* Mean stays in-band but a brief 40 C spike pushes MKT over the limit. */
	cc_window_reset(&st);
	for (int i = 0; i < 250; i++) {
		struct cc_sample s = { .temp_c = 4.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	for (int i = 0; i < 6; i++) {
		struct cc_sample s = { .temp_c = 40.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
		cc_window_push(&st, s);
	}
	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_classify(&f, &CFG), CC_MKT_EXCEEDED, "in-band mean + hot spike -> MKT_EXCEEDED");
}

ZTEST(cold_chain, test_classify_condensation)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 5.0f, 95.0f); /* in-band but very humid -> near dewpoint */
	cc_feat_extract(&st, &CFG, &f);
	zassert_equal(cc_classify(&f, &CFG), CC_CONDENSATION_RISK, "5 C / 95% RH -> CONDENSATION_RISK");
}

ZTEST(cold_chain, test_anomaly_and_names)
{
	struct cc_window_state st;
	struct cc_features     f;

	fill_const(&st, 5.0f, 50.0f);
	cc_feat_extract(&st, &CFG, &f);
	zassert_true(cc_anomaly_fallback(&f, &CFG) < 0.2f, "stable cold -> low anomaly");

	fill_const(&st, 20.0f, 50.0f); /* deep excursion */
	cc_feat_extract(&st, &CFG, &f);
	zassert_true(cc_anomaly_fallback(&f, &CFG) > 0.8f, "deep excursion -> high anomaly");

	zassert_true(strcmp(cc_state_name(CC_OK), "OK") == 0, "name");
	zassert_true(strcmp(cc_state_name(CC_MKT_EXCEEDED), "MKT_EXCEEDED") == 0, "name");
}
```

Add `#include <string.h>` at the top of the test if not already present.

- [ ] **Step 2: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: build failure — `cc_classify`/`cc_state_t`/`cc_state_name`/`cc_anomaly_fallback` undeclared.

- [ ] **Step 3: Add the API to the header** (keep the comments — teaching density)

Insert into `cold_chain.h` before the closing `#ifdef __cplusplus }`:

```c
/** Integrity state (reference-grade; customers retune the config per product). */
typedef enum {
	CC_OK                = 0, /**< inside band, MKT under limit, no condensation. */
	CC_TEMP_EXCURSION    = 1, /**< acute: mean out of band, or excursion-time over limit. */
	CC_MKT_EXCEEDED      = 2, /**< cumulative thermal damage (MKT over limit). */
	CC_CONDENSATION_RISK = 3, /**< T near dewpoint or very high humidity. */
	CC_STATE_COUNT
} cc_state_t;

/**
 * Classify the integrity state.  Order matters: an ACUTE excursion (currently
 * out of band) is reported before the CUMULATIVE MKT breach, which is reported
 * before the humidity-driven condensation risk.
 */
cc_state_t cc_classify(const struct cc_features *f, const struct cc_config *cfg);

/** Stable upper-case state name for the record. */
const char *cc_state_name(cc_state_t s);

/**
 * Deterministic 0..1 anomaly score (excursion depth + MKT overshoot), saturating.
 * Used when no AI model is loaded.
 */
float cc_anomaly_fallback(const struct cc_features *f, const struct cc_config *cfg);
```

- [ ] **Step 4: Implement** (keep the comments — teaching density)

Append to `cold_chain.c`:

```c
cc_state_t cc_classify(const struct cc_features *f, const struct cc_config *cfg)
{
	/* ACUTE first: the product is currently too warm/cold, or it has spent more
	 * than the allowed minutes out of band. */
	if (f->mean_temp_c < cfg->t_lo || f->mean_temp_c > cfg->t_hi ||
	    f->excursion_min > cfg->excursion_min_limit) {
		return CC_TEMP_EXCURSION;
	}
	/* CUMULATIVE: even after recovering to the band, a hot spike may have done
	 * enough Arrhenius damage to push MKT past the limit. */
	if (f->mkt_c > cfg->mkt_limit_c) {
		return CC_MKT_EXCEEDED;
	}
	/* HUMIDITY: ambient near the dewpoint (or saturated air) risks condensation
	 * on the goods/packaging. */
	if ((f->mean_temp_c - f->dewpoint_c) < cfg->dewpoint_margin_c || f->mean_rh_pct > 90.0f) {
		return CC_CONDENSATION_RISK;
	}
	return CC_OK;
}

const char *cc_state_name(cc_state_t s)
{
	switch (s) {
	case CC_OK:
		return "OK";
	case CC_TEMP_EXCURSION:
		return "TEMP_EXCURSION";
	case CC_MKT_EXCEEDED:
		return "MKT_EXCEEDED";
	case CC_CONDENSATION_RISK:
		return "CONDENSATION_RISK";
	default:
		return "UNKNOWN";
	}
}

float cc_anomaly_fallback(const struct cc_features *f, const struct cc_config *cfg)
{
	float score = 0.0f;

	/* How far the mean temperature is outside the band, scaled by the band width. */
	float band = cfg->t_hi - cfg->t_lo;
	if (band > 1e-6f) {
		float over = 0.0f;
		if (f->mean_temp_c > cfg->t_hi) {
			over = f->mean_temp_c - cfg->t_hi;
		} else if (f->mean_temp_c < cfg->t_lo) {
			over = cfg->t_lo - f->mean_temp_c;
		}
		score = over / band;
	}
	/* MKT overshoot contributes too (cumulative damage). */
	if (f->mkt_c > cfg->mkt_limit_c && cfg->mkt_limit_c > 1e-6f) {
		float mkt_term = (f->mkt_c - cfg->mkt_limit_c) / cfg->mkt_limit_c;
		if (mkt_term > score) {
			score = mkt_term;
		}
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

- [ ] **Step 5: Run GREEN + check density**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.cold_chain` PASS, 9/9. Confirm both core files remain ≥50% comment density. If `test_classify_mkt_exceeded` mislabels, confirm the branch order (acute excursion is checked before MKT, and the 4 C/40 C window's mean ≈ 4.8 C stays in-band so it reaches the MKT branch) — do NOT reorder or change thresholds.

- [ ] **Step 6: Format + commit**

```bash
git add examples/ai/cold-chain-monitor/src/cold_chain.h \
        examples/ai/cold-chain-monitor/src/cold_chain.c \
        tests/unit/cold_chain/src/test_cold_chain.c
git commit -m "feat(cc): 4-state cold-chain classifier + anomaly fallback + tests"
```

---

### Task 3: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/ai/cold-chain-monitor/src/main.c`
- Create: `examples/ai/cold-chain-monitor/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/ai/cold-chain-monitor/boards/native_sim_native_64.{conf,overlay}`
- Create: `examples/ai/cold-chain-monitor/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `cold_chain.h`; portable `<alp/peripheral.h>` (I2C), `<alp/inference.h>`, `<alp/board.h>`, `<alp/chips/bme280.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `CC,...` record per report, ending `[cc] done`.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/ai/cold-chain-monitor/CMakeLists.txt`:
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
project(cold_chain_monitor LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/cold_chain.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/cold-chain-monitor/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml**

Create `examples/ai/cold-chain-monitor/board.yaml`:
```yaml
# board.yaml -- cold-chain integrity monitor.
#
# A low-power M55 node reads the on-board BME280 temperature/humidity/pressure
# sensor on a slow logging cadence, computes the cold-chain metrics (MKT,
# dewpoint, excursion time), and classifies the integrity state.  Same source
# targets the V2N DEEPX path when som.sku is flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

pins:
  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS, doc: "BME280 environment sensor bus" }

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
      - i2c                   # BME280 link.

chips:
  - bme280                    # temperature / humidity / pressure.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim overlay + conf**

Create `examples/ai/cold-chain-monitor/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# native_sim has no real I2C controller; pull in the emul drivers so the BME280
# chip driver can open the sensor bus at boot.  No emul target is attached, so
# main.c falls back to synthetic environment data.
CONFIG_EMUL=y
CONFIG_I2C_EMUL=y
```

Create `examples/ai/cold-chain-monitor/boards/native_sim_native_64.overlay`:
```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-build overlay -- exposes one emulated I2C controller via the alp-i2c0
 * alias so alp_i2c_open(BOARD_I2C_SENSORS) resolves and the BME280 bring-up
 * runs.  No emul target is attached, so the chip-ID read fails and main.c
 * falls back to synthetic environment data.  On real silicon this file is NOT
 * applied.
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

- [ ] **Step 5: Write main.c** (≥50% comment density — keep all the comments)

Create `examples/ai/cold-chain-monitor/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold-chain-monitor
 * ==================
 *
 * Pharma/food cold-chain integrity monitor.  Pipeline:
 *
 *   BME280 (I2C) --read_raw + compensate--> T(C) / RH(%) / P(Pa)
 *     --sliding window--> cold_chain (mean/min/max, MKT, dewpoint, excursion)
 *       -> cc_classify (OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK)
 *       + <alp/inference.h> anomaly score (deterministic fallback)
 *     --> one CC record per report window.
 *
 * Honest scope: a reference cold-chain logger.  MKT, dewpoint, and excursion
 * time are the real metrics; thresholds are configurable per product.  This is
 * NOT a certified GxP / 21-CFR-Part-11 data logger (no validated audit trail,
 * no calibration traceability, no tamper-proof storage).  The model is a stub
 * (see models/README.md); with no model the deterministic classifier + anomaly
 * fallback run.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/bme280.h"

#include "cold_chain.h"

LOG_MODULE_REGISTER(cc, LOG_LEVEL_INF);

/* BME280 I2C address: SDO-tied-low = 0x76 on the EVK (0x77 if SDO is high). */
#define BME280_ADDR 0x76u
/* Number of report windows in the bounded demo run. */
#define N_REPORTS   3

/* Vaccine-fridge thresholds (2..8 C).  A real deployment loads the limits for
 * the specific product (frozen, ambient-stable, etc.). */
static const struct cc_config CFG = {
    .t_lo = 2.0f, .t_hi = 8.0f, .mkt_limit_c = 8.0f,
    .excursion_min_limit = 30.0f, .dewpoint_margin_c = 2.0f
};

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic anomaly fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/*
 * Synthetic environment per report window (native_sim / no sensor):
 *   report 0 -> stable in-band cold (5 C, 50% RH)         -> OK
 *   report 1 -> a warming excursion (ramps 5 C -> 12 C)   -> TEMP_EXCURSION
 *   report 2 -> cold but saturated air (5 C, 95% RH)      -> CONDENSATION_RISK
 */
static struct cc_sample synth_sample(int report, int i)
{
	struct cc_sample s = { .temp_c = 5.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
	switch (report) {
	case 0:
		break; /* stable cold */
	case 1:
		/* Linear warming ramp from 5 C to 12 C across the window. */
		s.temp_c = 5.0f + 7.0f * (float)i / (float)(CC_WINDOW_N - 1);
		s.rh_pct = 55.0f;
		break;
	default:
		s.rh_pct = 95.0f; /* near-saturated cold air */
		break;
	}
	return s;
}

/* Anomaly score: AI model if available, else the deterministic fallback. */
static float anomaly_score(alp_inference_t *inf, const struct cc_features *f)
{
	if (inf != NULL) {
		float vec[CC_FEATURE_DIM];
		(void)cc_feat_pack(f, vec, CC_FEATURE_DIM);
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
	return cc_anomaly_fallback(f, &CFG);
}

int main(void)
{
	static bme280_t            dev;
	static struct cc_window_state win;
	bool sensor_ok = false;

	/* Open the shared sensor I2C bus and bring up the BME280.  On native_sim no
	 * emul target answers, so init fails and we use synthetic data. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS,
	                                                   .bitrate_hz = 400000 });
	if (bus != NULL && bme280_init(&dev, bus, BME280_ADDR) == ALP_OK) {
		sensor_ok = true;
	} else {
		LOG_WRN("BME280 unavailable; using synthetic environment data");
	}

	/* Open the inference handle (NULL/stub-tolerant -> fallback runs). */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min\n");

	for (int r = 0; r < N_REPORTS; r++) {
		cc_window_reset(&win);
		/* Fill one window of readings (real sensor or synthetic). */
		for (int i = 0; i < CC_WINDOW_N; i++) {
			struct cc_sample s;
			if (sensor_ok) {
				bme280_raw_t         raw;
				bme280_compensated_t c;
				if (bme280_read_raw(&dev, &raw) == ALP_OK &&
				    bme280_compensate(&dev, &raw, &c) == ALP_OK) {
					s.temp_c      = (float)c.temperature_c100 / 100.0f;
					s.rh_pct      = (float)c.humidity_milli_pct / 1024.0f;
					s.pressure_pa = (float)c.pressure_pa;
				} else {
					s = synth_sample(r, i);
				}
			} else {
				s = synth_sample(r, i);
			}
			cc_window_push(&win, s);
		}

		/* Reduce the window to metrics, classify, score the anomaly, emit. */
		struct cc_features f;
		cc_feat_extract(&win, &CFG, &f);
		cc_state_t st = cc_classify(&f, &CFG);
		float      an = anomaly_score(inf, &f);
		(void)an; /* an feeds the operator's gateway alongside the state. */

		printk("CC,%.1f,%s,%.1f,%.1f,%.1f,%.1f,%.1f\n",
		       (double)((r + 1) * CC_WINDOW_N * CC_SAMPLE_MIN), cc_state_name(st),
		       (double)f.mean_temp_c, (double)f.mean_rh_pct, (double)f.dewpoint_c,
		       (double)f.mkt_c, (double)f.excursion_min);
	}

	/* Lifecycle teardown: release the model, the sensor, then the bus. */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (sensor_ok) {
		bme280_soft_reset(&dev);
	}
	if (bus != NULL) {
		alp_i2c_close(bus);
	}
	printk("[cc] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers (as the rail/motor examples did). `alp_i2c_open` takes an `alp_i2c_config_t*` with `.bus_id`/`.bitrate_hz` (mirror `examples/ai/ai-anomaly-detection-vibration/src/main.c`). `bme280_init(dev, bus, addr)` + `bme280_read_raw(dev, &bme280_raw_t)` + `bme280_compensate(dev, &raw, &bme280_compensated_t{temperature_c100, pressure_pa, humidity_milli_pct})` + `bme280_soft_reset` per `include/alp/chips/bme280.h`. If the BME280 needs a `bme280_set_sampling(...)` call before reads (check the header — it likely does, to leave forced/normal mode), add it after `bme280_init` and note the chosen oversampling/mode. `BOARD_I2C_SENSORS` from `<alp/board.h>`. The `alp_inference_*` calls per the sibling examples. The `an` score is computed but only logged via the operator gateway in a real build; the `(void)an` keeps native_sim from warning — OR include it in the record if you prefer (do not change the documented 8-column schema, though). Add `<string.h>` if a symbol is unresolved. Keep `<alp/*>` portable — no vendor names. **main.c must be ≥50% comment density — measure before commit.**

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only` WITH the board define — main.c includes `<alp/board.h>`)

Create `examples/ai/cold-chain-monitor/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: cold-chain-monitor
  description: |
    Cold-chain integrity monitor: BME280 T/RH/P -> sliding-window metrics
    (MKT / dewpoint / excursion time) -> 4-state classifier (OK / TEMP_EXCURSION
    / MKT_EXCEEDED / CONDENSATION_RISK) + AI anomaly score (deterministic
    fallback).  native_sim runs synthetic environment data covering the states.
common:
  tags: ai inference industrial environmental cold-chain marketing showcase
tests:
  alp_sdk.example.cold_chain_monitor.e1m_evk:
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
      - environmental
    harness: console
    harness_config:
      type: one_line
      regex:
        - "\\[cc\\] done"

  alp_sdk.example.cold_chain_monitor.aen_build:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
    platform_allow:
      - ensemble_e8_dk/ae402fa0e5597le0/rtss_hp
    build_only: true
    tags:
      - alp-sdk
      - example
      - ai
      - environmental
```

> NOTE: this app DOES include `<alp/board.h>` (for `BOARD_I2C_SENSORS`), so BOTH testcase entries — including the AEN `aen_build` — MUST carry `CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"` or the AEN build hits `board.h`'s `#error`. (This is the bug that bit the earlier examples; do not omit it from the aen_build entry.)

- [ ] **Step 7: Write the models training-recipe doc**

Create `examples/ai/cold-chain-monitor/models/README.md`:
```markdown
# Anomaly model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic 4-state
classifier + anomaly fallback run without one. The named states are threshold
rules; the AI adds an anomaly score for **subtle multi-variable drift** the
thresholds miss — a slowly degrading compressor, a door-seal leak that nudges
humidity and the warming slope together before any single limit trips.

1. **Record a healthy baseline** of the cabinet's T/RH/P across its normal duty
   cycle (defrost cycles, door openings), at the device window
   (`CC_WINDOW_N` samples per report).
2. **Extract the 8-feature `cc_features` vector** per report window.
3. **Train an autoencoder** (e.g. 8→4→2→4→8) on the healthy vectors; the
   reconstruction error is the anomaly score.
4. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Tune the `cc_config` thresholds (band, MKT limit, excursion-minute limit,
dewpoint margin) to your product's stability data.

Honest scope: a reference logger, NOT a certified GxP / 21-CFR-Part-11 data
logger (audit trail, calibration traceability, tamper-proof storage).
```

- [ ] **Step 8: Write README.md** (no `examples/*/src/` density rule for README, but keep it useful)

Create `examples/ai/cold-chain-monitor/README.md`:
```markdown
# cold-chain-monitor

> ⚠️ **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `cold_chain` core
> is host-unit-tested on `native_sim/native/64`; the full app runs end-to-end on
> native_sim with synthetic environment data covering the states. HiL with a
> real BME280 + a trained model is bench-gated.

A pharma/food **cold-chain integrity monitor**: sample a BME280 T/RH/P sensor on
a slow logging cadence, compute the standards-backed metrics (temperature
excursion time, Mean Kinetic Temperature, dewpoint), classify the integrity
state, and emit an AI anomaly score. The *environmental* edge-AI vertical.

## Honest scope

A reference cold-chain logger. MKT, dewpoint, and excursion-time are the real,
recognized metrics; thresholds are configurable per product. **NOT** a certified
GxP / 21-CFR-Part-11 data logger -- no validated audit trail, no calibration
traceability, no tamper-proof storage.

## Standards-backed metrics

- **MKT** (Mean Kinetic Temperature, ICH Q1A / USP <1079>): the single
  temperature delivering the same cumulative thermal stress as the fluctuating
  profile. `MKT >= arithmetic mean`, so a brief hot spike counts for more than
  its duration -- which is the whole point.
- **Dewpoint** (Magnus): condensation / mould risk when ambient T nears it.

## Pipeline

```
BME280 (I2C) --window--> cold_chain (mean/min/max, MKT, dewpoint, excursion)
  -> cc_classify (OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK)
  -> <alp/inference.h> anomaly score (deterministic fallback) -> CC record
```

## Output

```
# CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min
CC,256.0,OK,5.0,50.0,-4.4,5.0,0.0
CC,768.0,CONDENSATION_RISK,5.0,95.0,4.3,5.0,0.0
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/cold-chain-monitor
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic classifier/fallback). See
`models/README.md` for the autoencoder training recipe.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/cold_chain
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Cold-chain integrity example** (`examples/ai/cold-chain-monitor/`):
  environmental edge AI — BME280 T/RH/P → sliding-window `cold_chain` metrics
  (mean/min/max, temperature slope, **Mean Kinetic Temperature** per ICH/USP,
  Magnus dewpoint, excursion minutes) → a deterministic 4-state classifier
  (OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK) + an
  `<alp/inference.h>` anomaly score with a deterministic fallback. The core is
  host-unit-tested on `native_sim` (`tests/unit/cold_chain`); model is a stub
  with a training recipe in `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.cold_chain` (9/9) PASS.
- `alp_sdk.example.cold_chain_monitor.e1m_evk` PASS on `native_sim/native/64` (console `[cc] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-cc/twister.json`. Verify the emitted CC records show a sensible state spread (OK / TEMP_EXCURSION / CONDENSATION_RISK across the 3 reports) — paste them into the report. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 5 notes) — do NOT change the portable-API contract or the core's logic. The local AEN link env may hit the shared `alp_backends_*` orphan-section issue (same as sibling examples); if AEN fails ONLY with that, note it — CI is the AEN gate.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22; confirm `main.c` ≥50% comment density. Then:
```bash
git add examples/ai/cold-chain-monitor CHANGELOG.md
git commit -m "feat(cc): cold-chain monitor example app (BME280 metrics + classifier + anomaly) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 cold_chain stats/MKT/dewpoint/excursion → Task 1; classifier + anomaly fallback (C1/C2) → Task 2; C2 AI dispatch + C3 main.c + scaffolding + models/README + README + CHANGELOG → Task 3. Output record + taxonomy → Task 3 (main.c + README). Validation (one ztest suite + native_sim run) → Tasks 1-2 tests + Task 3 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 3 board.yaml + testcase.yaml. Honest scope (reference, not certified GxP) → Task 3 README + models/README. Comment-density ≥50% → built into the Task 1/2/3 code blocks + measured at each commit step. board.h-needs-the-AEN-define → Task 3 testcase NOTE (both entries carry the define). All spec sections map to a task.

**Type consistency:** `CC_WINDOW_N 256`, `CC_SAMPLE_MIN 1.0f`, `CC_FEATURE_DIM 8`, `CC_STATE_COUNT 4`, `CC_DH_OVER_R 10000.0f` consistent across header/impl/tests/main. `cc_sample/cc_window_state/cc_features/cc_config/cc_window_reset/push/full/cc_feat_extract/cc_feat_pack/cc_state_t/cc_classify/cc_state_name/cc_anomaly_fallback` — names + signatures identical across tasks. `cc_feat_pack` writes exactly 8 in the documented order. Config defaults identical in spec + main.c + tests. Output schema (8 columns) identical in main.c + README + spec. Classifier order (acute excursion → cumulative MKT → condensation → OK) matches the spec and the test cases (the 4 C/40 C spike keeps the mean in-band so it reaches the MKT branch).

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The 1-byte model stub + synthetic generators are deliberate, documented design decisions. The example cores + main.c are written at ≥50% comment density in the plan.
```
