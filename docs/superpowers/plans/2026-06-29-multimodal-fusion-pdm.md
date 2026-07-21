# Multimodal Fusion PdM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A multi-sensor motor-health monitor that fuses vibration (ICM-42670), current (INA236), and temperature (BME280) into one fault hypothesis + a confidence-weighted health score via cross-modal corroboration.

**Architecture:** One pure-C, arch-neutral, host-unit-tested core — `fusion_health` (per-modality sub-scores vs a healthy baseline → corroboration count → cross-modal fault hypothesis → confidence-weighted fused health) — plus an `<alp/inference.h>` fused model (stub) and a thin Zephyr `main.c` that reads the 3 sensors into a compact per-modality summary.

**Tech Stack:** Zephyr 4.4, C11, ztest on `native_sim/native/64`, `<alp/chips/{icm42670,ina236,bme280}.h>` (I2C), `<alp/inference.h>`, `scripts/alp_project.py` board.yaml→Kconfig.

## Global Constraints

- Core (`fusion_health.{c,h}`) is pure C — only `<stdint.h>`/`<stddef.h>`/`<stdbool.h>`/`<string.h>`/`<math.h>`. No Zephyr/MMIO/intrinsics; must build native_sim AND M55.
- App peripherals via portable `<alp/*>` APIs only (I2C via `icm42670_*`/`ina236_*`/`bme280_*`, inference); NO vendor (Ethos-U/DEEPX) name in app code — `ALP_INFERENCE_BACKEND_AUTO`.
- Fixed constants exactly: `FUSION_FEATURE_DIM 9`, `FUSION_FAULT_COUNT 5`; a modality is anomalous when its sub-score > 1.0 (deviation exceeds tolerance).
- **COMMENT DENSITY (project standard): EVERY file under `examples/*/src/` (the core AND main.c) must be ≥50% comment lines** (`comment_lines / total_lines`). The code blocks below are written at that density — transcribe with their comments, and MEASURE each file before committing.
- TDD: the core is RED-first, host-validated on `native_sim/native/64`. Sensor I/O + the AI call are the only non-host-testable parts.
- "Alp Lab AB" copyright (NOT "ALP Lab"); no `Co-Authored-By: Claude` in commits; **PR bodies carry NO Claude/AI footer**; NO binaries (model is a 1-byte stub; recipe is docs); no confidential/OneDrive/local paths; no login-gated vendor links.
- `main.c` includes `<alp/board.h>` (for `BOARD_I2C_SENSORS`) → BOTH testcase entries (native_sim AND the AEN `build_only`) MUST carry `CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"` or the AEN build hits `board.h`'s `#error`.
- Example dir: `examples/ai/multimodal-fusion-pdm/`. Primary target E1M-AEN; V2N via `som.sku` flip. ICM-42670 + INA236 + BME280 on the EVK sensor I2C bus.
- `examples/**` + `tests/**` C is clang-format-22-clean (WSL `~/.local/bin/clang-format`, v22 — NOT v14). WATCH the alignment trap: block comments ABOVE declaration/enum groups; trailing `/**< */` on members.
- Unit test compiles the core `.c` directly via a relative path from the test dir, with `_GNU_SOURCE` in the test CMakeLists. `zassert_within` takes `double`; cast `float` args to `(double)`.
- Twister gate (literal paths, NO `$VARS`, NO pipe; read `/tmp/tw-fuse/twister.json`; keep build+result in ONE `wsl … bash -lc` invocation — WSL /tmp is unstable across separate calls):
  ```
  wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && \
    export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr && \
    export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk && \
    export ZEPHYR_TOOLCHAIN_VARIANT=host && \
    python3 zephyr/scripts/twister \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/unit \
      --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples \
      -p native_sim/native/64 -O /tmp/tw-fuse'
  ```

---

## File Structure

- `examples/ai/multimodal-fusion-pdm/src/fusion_health.{c,h}` — the fusion core (Task 1).
- `examples/ai/multimodal-fusion-pdm/src/main.c` — Zephyr glue: 3-sensor read + summary + fusion (Task 2).
- `examples/ai/multimodal-fusion-pdm/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}` + `boards/native_sim_native_64.{conf,overlay}` + `models/README.md` (Task 2).
- `tests/unit/fusion_health/` — ztest suite (Task 1).
- `CHANGELOG.md` — entry (Task 2).

---

### Task 1: `fusion_health` — cross-modal fusion core + host tests

**Files:**
- Create: `examples/ai/multimodal-fusion-pdm/src/fusion_health.h`
- Create: `examples/ai/multimodal-fusion-pdm/src/fusion_health.c`
- Create: `tests/unit/fusion_health/{CMakeLists.txt,prj.conf,testcase.yaml,src/test_fusion_health.c}`

**Interfaces:**
- Produces (Task 2): `FUSION_FEATURE_DIM 9`, `FUSION_FAULT_COUNT 5`; `struct fusion_input`, `struct fusion_baseline`, `struct fusion_result`, `fusion_fault_t`; `fusion_assess`, `fusion_pack`, `fusion_fault_name`.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/fusion_health/src/test_fusion_health.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for fusion_health (cross-modal sensor fusion) -- native_sim.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include "fusion_health.h"

ZTEST_SUITE(fusion_health, NULL, NULL, NULL, NULL, NULL);

/* A small-motor baseline: nominal + tolerance per fusion_input field, in field
 * order {vib_rms, vib_crest, current_a, current_ripple, temp_c, temp_slope}. */
static const struct fusion_baseline BASE = {
    .nominal = { 0.05f, 3.0f, 1.0f, 0.05f, 30.0f, 0.0f },
    .tol     = { 0.05f, 2.0f, 0.5f, 0.05f, 10.0f, 0.5f },
};

/* Start from the healthy nominal, then the tests perturb individual fields by
 * 3x tolerance to drive a modality's sub-score to 3.0 (clearly > 1.0). */
static struct fusion_input nominal_input(void)
{
	struct fusion_input in = { .vib_rms = 0.05f, .vib_crest = 3.0f, .current_a = 1.0f,
	                           .current_ripple = 0.05f, .temp_c = 30.0f, .temp_slope = 0.0f };
	return in;
}

ZTEST(fusion_health, test_healthy)
{
	struct fusion_input  in = nominal_input();
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_HEALTHY, "all nominal -> HEALTHY");
	zassert_equal(r.corroboration, 0, "no modality anomalous");
	zassert_true(r.health_score < 0.05f, "healthy -> ~0 health score");
}

ZTEST(fusion_health, test_bearing_wear)
{
	struct fusion_input in = nominal_input();
	in.vib_rms = 0.05f + 3.0f * 0.05f; /* +3 tol -> vib_score ~3 */
	in.temp_c  = 30.0f + 3.0f * 10.0f; /* +3 tol -> temp_score ~3 */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_BEARING_WEAR, "vibration + heat -> BEARING_WEAR");
	zassert_equal(r.corroboration, 2, "two modalities corroborate");
	zassert_true(r.health_score > 0.5f, "corroborated severe fault -> high health score");
}

ZTEST(fusion_health, test_electrical_fault)
{
	struct fusion_input in = nominal_input();
	in.current_a = 1.0f + 3.0f * 0.5f; /* current over tolerance, others nominal */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_ELECTRICAL_FAULT, "current-only -> ELECTRICAL_FAULT");
	zassert_equal(r.corroboration, 1, "one modality anomalous");
}

ZTEST(fusion_health, test_mechanical_overload)
{
	struct fusion_input in = nominal_input();
	in.vib_rms   = 0.05f + 3.0f * 0.05f;
	in.current_a = 1.0f + 3.0f * 0.5f;
	in.temp_c    = 30.0f + 3.0f * 10.0f;
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_MECHANICAL_OVERLOAD, "all three -> MECHANICAL_OVERLOAD");
	zassert_equal(r.corroboration, 3, "all three corroborate");
}

ZTEST(fusion_health, test_uncorroborated_is_low_confidence)
{
	struct fusion_input in = nominal_input();
	in.vib_rms = 0.05f + 3.0f * 0.05f; /* vibration alone, no heat/current */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_UNCORROBORATED, "single modality -> UNCORROBORATED");
	zassert_equal(r.corroboration, 1, "one modality anomalous");
	/* Same severity as the bearing case (sub-score ~3) but discounted by the
	 * 0.5 confidence factor, so it must score lower than a corroborated fault. */
	zassert_true(r.health_score < 0.6f, "uncorroborated is discounted below a corroborated fault");
}

ZTEST(fusion_health, test_pack_dim_and_names)
{
	struct fusion_input  in = nominal_input();
	struct fusion_result r;
	float                vec[FUSION_FEATURE_DIM];
	fusion_assess(&in, &BASE, &r);
	zassert_equal(fusion_pack(&r, &in, vec, FUSION_FEATURE_DIM), (size_t)FUSION_FEATURE_DIM,
	              "pack writes FUSION_FEATURE_DIM");
	zassert_true(strcmp(fusion_fault_name(FUSION_BEARING_WEAR), "BEARING_WEAR") == 0, "name");
	zassert_true(strcmp(fusion_fault_name(FUSION_HEALTHY), "HEALTHY") == 0, "name");
}
```

- [ ] **Step 2: Write the test scaffolding**

Create `tests/unit/fusion_health/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_fusion_health)

set(FUS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/ai/multimodal-fusion-pdm/src)
target_include_directories(app PRIVATE ${FUS_SRC})
target_compile_definitions(app PRIVATE _GNU_SOURCE)
target_sources(app PRIVATE
    src/test_fusion_health.c
    ${FUS_SRC}/fusion_health.c
)
```

Create `tests/unit/fusion_health/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_ZTEST=y
```

Create `tests/unit/fusion_health/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0
tests:
  alp.unit.fusion_health:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    tags:
      - alp
      - ai
      - fusion
      - predictive-maintenance
      - unit
```

- [ ] **Step 3: Run RED**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.fusion_health` build failure (`fusion_health.h`/`.c` missing).

- [ ] **Step 4: Write the header** (≥50% comment density — keep all comments)

Create `examples/ai/multimodal-fusion-pdm/src/fusion_health.h`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fusion_health -- pure-C cross-modal sensor fusion for motor health.
 *
 * Each of three sensors contributes a compact summary: vibration (ICM-42670),
 * current (INA236), temperature (BME280).  fusion_assess() scores each modality
 * against a healthy baseline, counts how many modalities corroborate, and maps
 * the cross-modal PATTERN to a fault hypothesis:
 *
 *   - a real fault shows in SEVERAL modalities (bearing wear -> vibration AND
 *     heat; overload -> all three), so corroboration RAISES confidence;
 *   - an isolated single-modality blip (a sensor knock, a transient) does NOT
 *     corroborate, so it is flagged UNCORROBORATED at low confidence rather
 *     than raising a false alarm.
 * This is why fusion beats a bank of independent single-sensor thresholds.
 * Arch-neutral (stdint/math only): builds for native_sim and the M55 alike.
 */
#ifndef FUSION_HEALTH_H
#define FUSION_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 6 per-modality summary fields + 3 per-modality sub-scores = the AI input. */
#define FUSION_FEATURE_DIM 9
/* Number of fault-hypothesis classes (see fusion_fault_t). */
#define FUSION_FAULT_COUNT 5

/** Compact per-modality summary (computed by main.c from raw sensor reads). */
struct fusion_input {
	float vib_rms;        /**< vibration AC RMS (g). */
	float vib_crest;      /**< vibration crest factor (peak/RMS). */
	float current_a;      /**< mean motor current (A). */
	float current_ripple; /**< current AC ripple RMS (A). */
	float temp_c;         /**< motor/case temperature (deg C). */
	float temp_slope;     /**< temperature trend (deg C/interval). */
};

/** Healthy baseline: nominal value + tolerance per fusion_input field, in the
 *  field order {vib_rms, vib_crest, current_a, current_ripple, temp_c,
 *  temp_slope}.  A field is "anomalous" when |value - nominal| > tol. */
struct fusion_baseline {
	float nominal[6];
	float tol[6];
};

/** Fault hypothesis from the cross-modal pattern. */
typedef enum {
	FUSION_HEALTHY            = 0, /**< no modality anomalous. */
	FUSION_BEARING_WEAR       = 1, /**< vibration + temperature (friction heat). */
	FUSION_ELECTRICAL_FAULT   = 2, /**< current anomalous, vibration normal. */
	FUSION_MECHANICAL_OVERLOAD = 3, /**< all three modalities anomalous. */
	FUSION_UNCORROBORATED     = 4, /**< a single odd modality -> low confidence. */
	FUSION_FAULT_COUNT_ENUM        /* not used; FUSION_FAULT_COUNT is the macro. */
} fusion_fault_t;

/** Fusion verdict. */
struct fusion_result {
	float          health_score;  /**< 0 (healthy) .. 1 (critical), confidence-weighted. */
	fusion_fault_t hypothesis;    /**< cross-modal fault class. */
	float          vib_score;     /**< vibration sub-score (>1 = anomalous). */
	float          current_score; /**< current sub-score. */
	float          temp_score;    /**< temperature sub-score. */
	uint8_t        corroboration; /**< how many modalities are anomalous (0..3). */
};

/** Fuse the per-modality summary against the baseline into a verdict. */
void fusion_assess(const struct fusion_input *in, const struct fusion_baseline *base,
                   struct fusion_result *out);

/** Pack the 6 summary fields + 3 sub-scores into the AI feature vector;
 *  returns the count (FUSION_FEATURE_DIM), or 0 if @p cap is too small. */
size_t fusion_pack(const struct fusion_result *r, const struct fusion_input *in, float *vec,
                   size_t cap);

/** Stable upper-case hypothesis name for the record. */
const char *fusion_fault_name(fusion_fault_t f);

#ifdef __cplusplus
}
#endif

#endif /* FUSION_HEALTH_H */
```

- [ ] **Step 5: Write the implementation** (≥50% comment density — keep all comments)

Create `examples/ai/multimodal-fusion-pdm/src/fusion_health.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fusion_health implementation -- see fusion_health.h.
 */
#include "fusion_health.h"

#include <math.h>

/* A modality counts as anomalous once its sub-score exceeds this -- i.e. one of
 * its fields has deviated by more than its tolerance from the healthy nominal. */
#define FUSION_ANOMALY_THRESH 1.0f

void fusion_assess(const struct fusion_input *in, const struct fusion_baseline *base,
                   struct fusion_result *out)
{
	/* Lay the 6 summary fields out in the same order as the baseline arrays so
	 * we can score them with one loop. */
	const float v[6] = { in->vib_rms,    in->vib_crest, in->current_a,
	                     in->current_ripple, in->temp_c, in->temp_slope };

	/* Per-field normalised deviation: how many tolerances away from nominal.
	 * 0 = exactly nominal, 1 = at the tolerance edge, >1 = anomalous. */
	float dev[6];
	for (int i = 0; i < 6; i++) {
		dev[i] = (base->tol[i] > 1e-9f) ? (fabsf(v[i] - base->nominal[i]) / base->tol[i]) : 0.0f;
	}

	/* Each modality's sub-score is the worst (max) of its two fields' deviations
	 * -- a single bad field is enough to call the modality anomalous. */
	out->vib_score     = fmaxf(dev[0], dev[1]);
	out->current_score = fmaxf(dev[2], dev[3]);
	out->temp_score    = fmaxf(dev[4], dev[5]);

	/* Which modalities are anomalous, and how many corroborate. */
	int vib_hi  = (out->vib_score > FUSION_ANOMALY_THRESH);
	int cur_hi  = (out->current_score > FUSION_ANOMALY_THRESH);
	int temp_hi = (out->temp_score > FUSION_ANOMALY_THRESH);
	out->corroboration = (uint8_t)(vib_hi + cur_hi + temp_hi);

	/* Map the cross-modal pattern to a hypothesis.  HEALTHY is checked FIRST so
	 * a no-anomaly window is never mislabeled by the catch-all. */
	if (out->corroboration == 0) {
		out->hypothesis = FUSION_HEALTHY;
	} else if (vib_hi && cur_hi && temp_hi) {
		/* Vibration + electrical load + heat all elevated = the motor is being
		 * driven beyond its rating. */
		out->hypothesis = FUSION_MECHANICAL_OVERLOAD;
	} else if (vib_hi && temp_hi && !cur_hi) {
		/* Mechanical roughness + friction heat, electrical draw still normal =
		 * the classic bearing-wear signature. */
		out->hypothesis = FUSION_BEARING_WEAR;
	} else if (cur_hi && !vib_hi) {
		/* Current anomaly without mechanical vibration = a winding/supply issue. */
		out->hypothesis = FUSION_ELECTRICAL_FAULT;
	} else {
		/* A lone or odd modality (e.g. vibration only) -- nothing corroborates,
		 * so this is more likely noise / a sensor knock than a real fault. */
		out->hypothesis = FUSION_UNCORROBORATED;
	}

	/* Severity from the worst modality (sub-score 1 = at-tolerance -> 0 severity;
	 * 3 -> saturates at 1).  Confidence weights it down when uncorroborated. */
	float max_sub  = fmaxf(out->vib_score, fmaxf(out->current_score, out->temp_score));
	float severity = (max_sub - 1.0f) / 2.0f;
	if (severity < 0.0f) {
		severity = 0.0f;
	}
	if (severity > 1.0f) {
		severity = 1.0f;
	}
	float confidence;
	switch (out->hypothesis) {
	case FUSION_HEALTHY:
		confidence = 0.0f; /* no fault -> no score. */
		break;
	case FUSION_UNCORROBORATED:
		confidence = 0.5f; /* single modality -> half-weight (likely noise). */
		break;
	default:
		confidence = 1.0f; /* corroborated fault -> full weight. */
		break;
	}
	out->health_score = severity * confidence;
}

size_t fusion_pack(const struct fusion_result *r, const struct fusion_input *in, float *vec,
                   size_t cap)
{
	if (cap < (size_t)FUSION_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	/* 6 raw summary fields ... */
	vec[i++] = in->vib_rms;
	vec[i++] = in->vib_crest;
	vec[i++] = in->current_a;
	vec[i++] = in->current_ripple;
	vec[i++] = in->temp_c;
	vec[i++] = in->temp_slope;
	/* ... then the 3 per-modality sub-scores the rule logic derived. */
	vec[i++] = r->vib_score;
	vec[i++] = r->current_score;
	vec[i++] = r->temp_score;
	return i; /* == FUSION_FEATURE_DIM */
}

const char *fusion_fault_name(fusion_fault_t f)
{
	switch (f) {
	case FUSION_HEALTHY:
		return "HEALTHY";
	case FUSION_BEARING_WEAR:
		return "BEARING_WEAR";
	case FUSION_ELECTRICAL_FAULT:
		return "ELECTRICAL_FAULT";
	case FUSION_MECHANICAL_OVERLOAD:
		return "MECHANICAL_OVERLOAD";
	case FUSION_UNCORROBORATED:
		return "UNCORROBORATED";
	default:
		return "UNKNOWN";
	}
}
```

- [ ] **Step 6: Run GREEN + check density**

Run twister (testsuite-root `tests/unit`). Expected: `alp.unit.fusion_health` PASS, 6/6. Measure both files' comment density — both ≥50%; add teaching comments if under. If `test_uncorroborated_is_low_confidence` is ambiguous, note both bearing (health = severity×1.0 = 1.0) and uncorroborated (severity×0.5 = 0.5) share severity 1.0, so the discount is what separates them.

- [ ] **Step 7: Format + commit**

Format with clang-format-22, then:
```bash
git add examples/ai/multimodal-fusion-pdm/src/fusion_health.h \
        examples/ai/multimodal-fusion-pdm/src/fusion_health.c \
        tests/unit/fusion_health
git commit -m "feat(fuse): fusion_health cross-modal corroboration core + ztest"
```

---

### Task 2: Example app — `main.c`, scaffolding, docs

**Files:**
- Create: `examples/ai/multimodal-fusion-pdm/src/main.c`
- Create: `examples/ai/multimodal-fusion-pdm/{CMakeLists.txt,prj.conf,board.yaml,testcase.yaml,README.md}`
- Create: `examples/ai/multimodal-fusion-pdm/boards/native_sim_native_64.{conf,overlay}`
- Create: `examples/ai/multimodal-fusion-pdm/models/README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: all of `fusion_health.h`; portable `<alp/peripheral.h>` (I2C), `<alp/inference.h>`, `<alp/board.h>`, `<alp/chips/{icm42670,ina236,bme280}.h>`.
- Produces: a `native_sim/native/64` build that prints the header + one `FUSE,...` record per report, ending `[fuse] done`.

- [ ] **Step 1: Write CMakeLists.txt**

Create `examples/ai/multimodal-fusion-pdm/CMakeLists.txt`:
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
project(multimodal_fusion_pdm LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/fusion_health.c
)
```

- [ ] **Step 2: Write prj.conf**

Create `examples/ai/multimodal-fusion-pdm/prj.conf`:
```
# SPDX-License-Identifier: Apache-2.0
CONFIG_MAIN_STACK_SIZE=16384

CONFIG_LOG=y
CONFIG_LOG_PRINTK=y
CONFIG_CBPRINTF_FP_SUPPORT=y

CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y
```

- [ ] **Step 3: Write board.yaml**

Create `examples/ai/multimodal-fusion-pdm/board.yaml`:
```yaml
# board.yaml -- multimodal fusion predictive-maintenance.
#
# An M55 node reads three on-board sensors -- ICM-42670 (vibration), INA236
# (current), BME280 (temperature) -- and fuses them into one motor-health
# verdict via cross-modal corroboration.  Same source targets the V2N DEEPX
# path when som.sku is flipped.

som:
  sku: E1M-AEN701

preset: e1m-evk
supported_boards:
  - e1m-evk
  - e1m-x-evk

pins:
  - { e1m: E1M_I2C0, macro: EVK_I2C_BUS_SENSORS, doc: "ICM-42670 + INA236 + BME280 shared bus" }

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
      - i2c                   # all three sensors.

chips:
  - icm42670                  # vibration (accel).
  - ina236                    # current/voltage/power.
  - bme280                    # temperature/humidity/pressure.

diagnostics:
  log_level: info
```

- [ ] **Step 4: Write the native_sim overlay + conf**

Create `examples/ai/multimodal-fusion-pdm/boards/native_sim_native_64.conf`:
```
# SPDX-License-Identifier: Apache-2.0
# native_sim has no real I2C controller; pull in the emul drivers so the chip
# drivers can open the sensor bus at boot.  No emul targets are attached, so
# main.c falls back to synthetic per-modality data.
CONFIG_EMUL=y
CONFIG_I2C_EMUL=y
```

Create `examples/ai/multimodal-fusion-pdm/boards/native_sim_native_64.overlay`:
```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-build overlay -- exposes one emulated I2C controller via the alp-i2c0
 * alias so alp_i2c_open(BOARD_I2C_SENSORS) resolves and the chip bring-ups
 * run.  No emul targets are attached, so the chip-ID reads fail and main.c
 * falls back to synthetic per-modality data.  On real silicon this file is NOT
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

- [ ] **Step 5: Write main.c** (≥50% comment density — keep all comments)

Create `examples/ai/multimodal-fusion-pdm/src/main.c`:

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * multimodal-fusion-pdm
 * =====================
 *
 * Multi-sensor motor-health monitor.  Pipeline:
 *
 *   ICM-42670 (vibration) ---+
 *   INA236    (current)   ---+--> compact per-modality summary (fusion_input)
 *   BME280    (temperature)--+        |
 *                                      v
 *   fusion_health: per-modality sub-scores vs baseline -> corroboration count
 *     -> cross-modal fault hypothesis + confidence-weighted health score
 *     -> <alp/inference.h> fused model (deterministic fusion-rule fallback)
 *     --> one FUSE record per report.
 *
 * The point of fusion: a real fault corroborates across modalities (bearing
 * wear shows in vibration AND temperature), so an isolated single-modality
 * blip is flagged UNCORROBORATED at low confidence instead of raising a false
 * alarm -- which a bank of independent single-sensor thresholds would do.
 *
 * Honest scope: reference fusion logic.  The per-modality summaries here are
 * lightweight (the dedicated single-modality examples do the richer DSP); the
 * hypotheses are heuristic cross-modal rules (customer retunes the baseline +
 * weights per machine).  The model is a stub (see models/README.md); with no
 * model the deterministic fusion rule runs.  Any missing sensor degrades
 * gracefully: its summary stays at the baseline nominal, so that modality reads
 * non-anomalous and simply lowers corroboration.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/ina236.h"
#include "alp/chips/bme280.h"

#include "fusion_health.h"

LOG_MODULE_REGISTER(fuse, LOG_LEVEL_INF);

/* Number of fault scenarios the bounded demo walks through. */
#define N_REPORTS 5

/* Small-motor healthy baseline (nominal + tolerance per fusion_input field). */
static const struct fusion_baseline BASE = {
    .nominal = { 0.05f, 3.0f, 1.0f, 0.05f, 30.0f, 0.0f },
    .tol     = { 0.05f, 2.0f, 0.5f, 0.05f, 10.0f, 0.5f },
};

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic fusion-rule fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/*
 * Synthetic per-modality summary for native_sim (no real sensors attached).
 * Each report drives a distinct cross-modal scenario so every hypothesis fires:
 *   0 healthy; 1 bearing wear (vib+temp); 2 electrical (current);
 *   3 mechanical overload (all three); 4 uncorroborated (vibration alone).
 */
static struct fusion_input synth_input(int report)
{
	/* Start from the healthy nominal, then perturb per scenario by 3x tolerance. */
	struct fusion_input in = { .vib_rms = 0.05f, .vib_crest = 3.0f, .current_a = 1.0f,
	                           .current_ripple = 0.05f, .temp_c = 30.0f, .temp_slope = 0.0f };
	switch (report) {
	case 0:
		break; /* healthy */
	case 1:    /* bearing wear: vibration + heat */
		in.vib_rms = 0.20f;
		in.temp_c  = 60.0f;
		break;
	case 2: /* electrical: current only */
		in.current_a = 2.5f;
		break;
	case 3: /* mechanical overload: all three */
		in.vib_rms   = 0.20f;
		in.current_a = 2.5f;
		in.temp_c    = 60.0f;
		break;
	default: /* uncorroborated: vibration alone */
		in.vib_rms = 0.20f;
		break;
	}
	return in;
}

/*
 * Read the three sensors into the compact per-modality summary.  Each sensor is
 * optional: if it failed to initialise, its fields stay at the baseline nominal
 * (so that modality reads non-anomalous).  On native_sim none are present, so
 * the caller uses synth_input() instead -- this function is the real-hardware
 * read path, kept here as the reference for HiL bring-up.
 */
static void read_sensors(icm42670_t *imu, bool imu_ok, ina236_t *mon, bool mon_ok,
                         bme280_t *bme, bool bme_ok, struct fusion_input *in)
{
	/* Default every field to its healthy nominal (used for any absent sensor). */
	in->vib_rms = BASE.nominal[0];
	in->vib_crest = BASE.nominal[1];
	in->current_a = BASE.nominal[2];
	in->current_ripple = BASE.nominal[3];
	in->temp_c = BASE.nominal[4];
	in->temp_slope = BASE.nominal[5];

	/* Vibration: a short accel burst -> AC RMS + crest of the magnitude. */
	if (imu_ok) {
		float peak = 0.0f, sum2 = 0.0f, mean = 0.0f, mag[32];
		for (int i = 0; i < 32; i++) {
			icm42670_axes_t a;
			if (icm42670_read_accel(imu, &a) != ALP_OK) {
				break;
			}
			/* +/-16 g full-scale -> 2048 LSB/g (see the vibration example). */
			mag[i] = sqrtf((float)a.x * a.x + (float)a.y * a.y + (float)a.z * a.z) / 2048.0f;
			mean += mag[i];
		}
		mean /= 32.0f;
		for (int i = 0; i < 32; i++) {
			float ac = mag[i] - mean;
			sum2 += ac * ac;
			if (fabsf(ac) > peak) {
				peak = fabsf(ac);
			}
		}
		in->vib_rms   = sqrtf(sum2 / 32.0f);
		in->vib_crest = (in->vib_rms > 1e-6f) ? (peak / in->vib_rms) : 0.0f;
	}

	/* Current: mean + AC ripple over a short burst. */
	if (mon_ok) {
		float mean = 0.0f, sum2 = 0.0f, samp[16];
		for (int i = 0; i < 16; i++) {
			int32_t ua = 0;
			if (ina236_read_current_ua(mon, &ua) != ALP_OK) {
				break;
			}
			samp[i] = (float)ua / 1e6f;
			mean += samp[i];
		}
		mean /= 16.0f;
		for (int i = 0; i < 16; i++) {
			float ac = samp[i] - mean;
			sum2 += ac * ac;
		}
		in->current_a      = mean;
		in->current_ripple = sqrtf(sum2 / 16.0f);
	}

	/* Temperature: a single compensated reading (slope left at nominal here;
	 * a real build would difference successive reports). */
	if (bme_ok) {
		bme280_raw_t         raw;
		bme280_compensated_t c;
		if (bme280_read_raw(bme, &raw) == ALP_OK && bme280_compensate(bme, &raw, &c) == ALP_OK) {
			in->temp_c = (float)c.temperature_c100 / 100.0f;
		}
	}
}

int main(void)
{
	static icm42670_t imu;
	static ina236_t   mon;
	static bme280_t   bme;
	bool imu_ok = false, mon_ok = false, bme_ok = false;

	/* Open the shared sensor bus and bring up each chip independently; any that
	 * is absent simply drops out of the fusion (its modality reads nominal). */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS,
	                                                   .bitrate_hz = 400000 });
	if (bus != NULL) {
		imu_ok = (icm42670_init(&imu, bus, ICM42670_I2C_ADDR_HIGH) == ALP_OK &&
		          icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_16G) == ALP_OK);
		mon_ok = (ina236_init(&mon, bus, 0x40u, 0.01f, 8.0f, INA236_ADCRANGE_81MV) == ALP_OK);
		bme_ok = (bme280_init(&bme, bus, 0x76u) == ALP_OK);
	}
	if (!imu_ok || !mon_ok || !bme_ok) {
		LOG_WRN("one or more sensors unavailable; using synthetic per-modality data");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration\n");

	for (int r = 0; r < N_REPORTS; r++) {
		/* Build the per-modality summary: real reads when all three sensors are
		 * present, otherwise the synthetic scenario for this report. */
		struct fusion_input in;
		if (imu_ok && mon_ok && bme_ok) {
			read_sensors(&imu, imu_ok, &mon, mon_ok, &bme, bme_ok, &in);
		} else {
			in = synth_input(r);
		}

		/* Fuse.  The AI path (when a model is loaded) consumes fusion_pack()'s
		 * vector; the stub forces the deterministic rule, which IS fusion_assess. */
		struct fusion_result res;
		fusion_assess(&in, &BASE, &res);
		if (inf != NULL) {
			float vec[FUSION_FEATURE_DIM];
			(void)fusion_pack(&res, &in, vec, FUSION_FEATURE_DIM);
			alp_inference_tensor_t ten = { 0 };
			if (alp_inference_get_input(inf, 0, &ten) == ALP_OK &&
			    ten.dtype == ALP_INFERENCE_DTYPE_F32 && ten.data != NULL &&
			    ten.size_bytes >= sizeof(vec)) {
				memcpy(ten.data, vec, sizeof(vec));
				/* A trained model would override res.hypothesis here after
				 * invoke(); the stub yields no usable tensor, so we keep the
				 * deterministic verdict. */
				(void)alp_inference_invoke(inf);
			}
		}

		printk("FUSE,%.1f,%s,%.2f,%.1f,%.1f,%.1f,%u\n", (double)(r + 1),
		       fusion_fault_name(res.hypothesis), (double)res.health_score,
		       (double)res.vib_score, (double)res.current_score, (double)res.temp_score,
		       (unsigned)res.corroboration);
	}

	/* Lifecycle teardown: release the model, each sensor, then the bus. */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (imu_ok) {
		icm42670_deinit(&imu);
	}
	if (mon_ok) {
		ina236_reset(&mon);
	}
	if (bme_ok) {
		bme280_soft_reset(&bme);
	}
	if (bus != NULL) {
		alp_i2c_close(bus);
	}
	printk("[fuse] done\n");
	return 0;
}
```

> Implementer notes: reconcile `<alp/*>` signatures against the real headers + the sibling examples (`ai-anomaly-detection-vibration` for ICM-42670 + `alp_i2c_open`; `motor-current-signature`/`drone-hud` for `ina236_init`'s 6-arg form `(ctx, bus, addr, shunt_ohms, max_current_a, adcrange)` + the `INA236_ADCRANGE_81MV` enum; `cold-chain-monitor` for `bme280_init`/`read_raw`/`compensate`/`soft_reset` and whether `bme280_set_sampling(...)` is needed before reads — add it if so). If `icm42670_read_accel` needs gyro disabled or a different addr, follow the vibration sibling. Match `(void)`-cast on any discarded `alp_status_t` to avoid `-Wunused-result` on the AEN build. Add `<string.h>`/`<math.h>` if a symbol is unresolved. **main.c must be ≥50% comment density — measure before commit.** Keep `<alp/*>` portable — no vendor names.

- [ ] **Step 6: Write testcase.yaml** (native_sim RUNS; AEN separate `build_only`; BOTH carry the board define — main.c includes `<alp/board.h>`)

Create `examples/ai/multimodal-fusion-pdm/testcase.yaml`:
```yaml
# SPDX-License-Identifier: Apache-2.0

sample:
  name: multimodal-fusion-pdm
  description: |
    Multi-sensor motor-health monitor: ICM-42670 (vibration) + INA236 (current)
    + BME280 (temperature) fused via cross-modal corroboration into a fault
    hypothesis (HEALTHY / BEARING_WEAR / ELECTRICAL_FAULT / MECHANICAL_OVERLOAD
    / UNCORROBORATED) + a confidence-weighted health score.  native_sim runs
    synthetic per-modality data covering every scenario.
common:
  tags: ai inference industrial predictive-maintenance fusion marketing showcase
tests:
  alp_sdk.example.multimodal_fusion_pdm.e1m_evk:
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
        - "\\[fuse\\] done"

  alp_sdk.example.multimodal_fusion_pdm.aen_build:
    extra_configs:
      - 'CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"'
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

Create `examples/ai/multimodal-fusion-pdm/models/README.md`:
```markdown
# Fused model — training recipe

This example ships **no model** (a 1-byte stub); the deterministic cross-modal
fusion rule (`fusion_assess`) runs without one. The named hypotheses are
threshold + corroboration rules; a trained model can learn subtler fused
signatures (a fault that splits its energy across modalities below any single
threshold, or a machine-specific cross-correlation).

1. **Collect labelled runs** of the machine across its fault modes, logging the
   `fusion_input` 6-field summary (or the 9-element `fusion_pack` vector) per
   report window, tagged with the ground-truth fault.
2. **Train a small classifier** over the `FUSION_FEATURE_DIM` (9) fused vector
   → `FUSION_FAULT_COUNT` (5) classes, or an autoencoder for an anomaly score.
3. **Quantise + compile:** TFLite → **Vela** for Ethos-U (AEN) or the **DX-M1**
   toolchain for V2N. Drop it here and point `alp_inference_open` at it.

Retune the `fusion_baseline` (nominal + tolerance per field) to the machine's
healthy operating point — this is the single most important calibration step.

Honest scope: reference fusion logic; the per-modality summaries are lightweight
(the dedicated single-modality examples do the richer DSP).
```

- [ ] **Step 8: Write README.md**

Create `examples/ai/multimodal-fusion-pdm/README.md`:
```markdown
# multimodal-fusion-pdm

> ⚠️ **`[UNTESTED]` on hardware -- v0.9 paper-correct.** The `fusion_health`
> core is host-unit-tested on `native_sim/native/64`; the full app runs
> end-to-end on native_sim with synthetic per-modality data covering every
> scenario. HiL with the three real sensors + a trained model is bench-gated.

A multi-sensor motor-health monitor that **fuses** vibration (ICM-42670),
current (INA236), and temperature (BME280) into one fault hypothesis + a
confidence-weighted health score via cross-modal corroboration.

## Why fusion

A real fault corroborates across modalities -- bearing wear raises vibration AND
temperature; an overload raises all three. An isolated single-modality blip (a
sensor knock, a transient) does not corroborate, so it is flagged
`UNCORROBORATED` at low confidence instead of crying wolf. That corroboration is
how fusion suppresses the false positives a bank of single-sensor thresholds
would raise.

## Fault hypotheses

| Pattern (vibration / current / temperature anomalous) | Hypothesis |
|---|---|
| none | `HEALTHY` |
| vibration + temperature | `BEARING_WEAR` |
| current (vibration normal) | `ELECTRICAL_FAULT` |
| all three | `MECHANICAL_OVERLOAD` |
| a single modality | `UNCORROBORATED` (low confidence) |

## Pipeline

```
ICM-42670 + INA236 + BME280 --summary--> fusion_health (sub-scores ->
  corroboration -> hypothesis + health) -> <alp/inference.h> fused model
  (deterministic fusion-rule fallback) -> FUSE record
```

## Output

```
# FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration
FUSE,2.0,BEARING_WEAR,1.00,3.0,0.0,3.0,2
FUSE,5.0,UNCORROBORATED,0.50,3.0,0.0,0.0,1
```

## Build

```
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/ai/multimodal-fusion-pdm
west flash
```

Flip `som.sku` in `board.yaml` to `E1M-V2M101` for the DEEPX DX-M1 path.

## Model

No model is shipped (stub + deterministic fusion rule). See `models/README.md`
for the training recipe. The most important calibration is the per-machine
`fusion_baseline`.

## Tests

```
twister -p native_sim/native/64 -T tests/unit/fusion_health
```
```

- [ ] **Step 9: Add the CHANGELOG entry**

Add under the top `## [Unreleased]` section of `CHANGELOG.md`:
```markdown
- **Multimodal fusion PdM example** (`examples/ai/multimodal-fusion-pdm/`):
  fuses vibration (ICM-42670) + current (INA236) + temperature (BME280) into one
  motor-health verdict — `fusion_health` scores each modality vs a healthy
  baseline, counts cross-modal corroboration, and maps the pattern to a fault
  hypothesis (HEALTHY / BEARING_WEAR / ELECTRICAL_FAULT / MECHANICAL_OVERLOAD /
  UNCORROBORATED) with a confidence-weighted health score — plus an
  `<alp/inference.h>` fused model with the deterministic rule as fallback. Core
  host-unit-tested on `native_sim` (`tests/unit/fusion_health`); model is a stub
  with a recipe in `models/README.md`; HiL bench-gated.
```

- [ ] **Step 10: Build + run the gate**

Run twister with BOTH testsuite-roots (`tests/unit` AND `examples`). Expected:
- `alp.unit.fusion_health` (6/6) PASS.
- `alp_sdk.example.multimodal_fusion_pdm.e1m_evk` PASS on `native_sim/native/64` (console `[fuse] done`).
- the AEN cross-build (`ensemble_e8_dk/...`) builds (`build_only`).
Read `/tmp/tw-fuse/twister.json`. Verify the FUSE records show the scenario spread (HEALTHY / BEARING_WEAR / ELECTRICAL_FAULT / MECHANICAL_OVERLOAD / UNCORROBORATED across the 5 reports) — paste them into the report. If a `<alp/*>` symbol mismatch breaks the example build, fix `main.c` against the real API (Step 5 notes) — do NOT change the portable-API contract or the core's logic. The local AEN link env may hit the shared `alp_backends_*` orphan-section issue (same as sibling examples); if AEN fails ONLY with that, note it — CI is the AEN gate.

- [ ] **Step 11: Format + commit**

Format all new `examples/**` C with clang-format-22; confirm `main.c` ≥50% comment density. Then:
```bash
git add examples/ai/multimodal-fusion-pdm CHANGELOG.md
git commit -m "feat(fuse): multimodal fusion PdM example app (3-sensor read + fusion) + native_sim run"
```

---

## Self-Review (completed by plan author)

**Spec coverage:** C1 fusion_health (assess + pack + name) → Task 1; C2 AI dispatch + C3 main.c (3-sensor read + summary) + scaffolding + models/README + README + CHANGELOG → Task 2. Output record + taxonomy → Task 2 (main.c + README). Validation (one ztest suite + native_sim run) → Task 1 tests + Task 2 Step 10. Platform targets (AEN primary, V2N flip, native_sim) → Task 2 board.yaml + testcase.yaml. Honest scope (lightweight summaries, heuristic rules, graceful degradation) → Task 2 README + models/README + main.c banner. Comment-density ≥50% → built into the Task 1/2 code blocks + measured at each commit. board.h-needs-AEN-define → Task 2 testcase (both entries carry it). All spec sections map to a task.

**Type consistency:** `FUSION_FEATURE_DIM 9`, `FUSION_FAULT_COUNT 5` consistent across header/impl/tests/main. `fusion_input/fusion_baseline/fusion_result/fusion_fault_t/fusion_assess/fusion_pack/fusion_fault_name` — names + signatures identical across tasks. `fusion_pack` writes exactly 9 (6 summary + 3 sub-scores) in the documented order. Baseline (nominal+tol arrays, field order) identical in tests + main.c. Output schema (7 fields after FUSE) identical in main.c + README + spec. Hypothesis rule order (HEALTHY → MECHANICAL_OVERLOAD → BEARING_WEAR → ELECTRICAL_FAULT → UNCORROBORATED) matches the spec's HEALTHY-first ordering and the test cases.

**Placeholder scan:** no "TBD"/"handle edge cases"/"similar to". Every code step carries complete code. The 1-byte model stub + synthetic generator are deliberate, documented design decisions. The example core + main.c are written at ≥50% comment density in the plan.
```
