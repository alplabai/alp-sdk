/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- TMU CORDIC dispatch.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* TMU (CORDIC math accelerator) dispatch.                            */
/* ----------------------------------------------------------------- */

/* Per-host-function -> vendor TMU mode mapping.  Indexed by the wire
 * `function` byte (gd32g553_tmu_function_t in
 * `include/alp/chips/gd32g553.h`).  `mode = 0` is the NOSUPPORT
 * sentinel for the three host functions the GD32G5 TMU doesn't
 * natively support (tan / exp / tanh) -- the host wrapper can later
 * compose them via libm or via two TMU calls (tan = sin/cos,
 * tanh = sinh/cosh, exp = cosh + sinh) without protocol changes. */
typedef struct {
	uint32_t mode;   /* TMU_MODE_* (zero = NOSUPPORT marker)             */
	uint8_t  inputs; /* 1 or 2 inputs (zero matches NOSUPPORT entries)   */
} tmu_dispatch_t;

static const tmu_dispatch_t tmu_dispatch[] = {
	[0]  = { TMU_MODE_SIN, 2 },     /* sin(theta) -- HW computes m*sin(theta) */
	[1]  = { TMU_MODE_COS, 2 },     /* cos(theta) -- ditto                    */
	[2]  = { 0, 0 },                /* tan       -- NOSUPPORT                 */
	[3]  = { TMU_MODE_ATAN, 1 },    /* atan(x)                                */
	[4]  = { TMU_MODE_ATAN2, 2 },   /* atan2(y, x)                            */
	[5]  = { TMU_MODE_SQRT, 1 },    /* sqrt(x)                                */
	[6]  = { TMU_MODE_LN, 1 },      /* log = ln(x)                            */
	[7]  = { 0, 0 },                /* exp       -- NOSUPPORT                 */
	[8]  = { TMU_MODE_SINH, 1 },    /* sinh(x)                                */
	[9]  = { TMU_MODE_COSH, 1 },    /* cosh(x)                                */
	[10] = { 0, 0 },                /* tanh      -- NOSUPPORT                 */
	[11] = { TMU_MODE_MODULUS, 2 }, /* hypot = sqrt(x^2 + y^2)                */
};
#define TMU_DISPATCH_COUNT (sizeof(tmu_dispatch) / sizeof(tmu_dispatch[0]))

/* Timeout for the TMU END flag poll.  TMU operations complete in
 * tens of cycles at 216 MHz; the bound is the abort latch for a
 * misconfigured op rather than the typical-case wait. */
#define TMU_COMPUTE_TIMEOUT 65535u

/* Strict-aliasing-safe float<->u32 bit reinterpret helpers.  GCC
 * inlines these to bitcast instructions with -O1+. */
static float bits_to_f32(uint32_t b)
{
	float f;
	__builtin_memcpy(&f, &b, sizeof f);
	return f;
}
static uint32_t f32_to_bits(float f)
{
	uint32_t b;
	__builtin_memcpy(&b, &f, sizeof b);
	return b;
}

int bridge_hw_tmu_compute(uint8_t function, uint8_t format, uint32_t in_a, uint32_t in_b,
                          uint32_t *result_out)
{
	if (result_out == 0) return BRIDGE_HW_ERR_INVAL;
	*result_out = 0u;
	if (function >= TMU_DISPATCH_COUNT) return BRIDGE_HW_ERR_INVAL;
	if (format > 1u) return BRIDGE_HW_ERR_INVAL;

	const tmu_dispatch_t *d = &tmu_dispatch[function];
	if (d->inputs == 0u) {
		/* tan / exp / tanh: GD32G5 TMU has no native mode.  Host
         * wrapper sees STATUS_NOSUPPORT and (eventually) falls back
         * to libm or composes via two TMU calls. */
		return BRIDGE_HW_ERR_NOTIMPL;
	}

	/* Configure TMU for this op.  Per-call config keeps the dispatch
     * stateless -- a previous SIN call doesn't leave the TMU in a
     * state that surprises a subsequent SQRT call. */
	tmu_parameter_struct cfg;
	tmu_struct_para_init(&cfg);
	tmu_deinit();
	cfg.mode            = d->mode;
	cfg.scale           = TMU_SCALING_FACTOR_1;
	cfg.dma_read        = TMU_READ_DMA_DISABLE;
	cfg.dma_write       = TMU_WRITE_DMA_DISABLE;
	cfg.input_width     = TMU_INPUT_WIDTH_32;
	cfg.output_width    = TMU_OUTPUT_WIDTH_32;
	cfg.input_floating  = (format == 1u) ? TMU_INPUT_FLOAT_ENABLE : TMU_INPUT_FLOAT_DISABLE;
	cfg.output_floating = (format == 1u) ? TMU_OUTPUT_FLOAT_ENABLE : TMU_OUTPUT_FLOAT_DISABLE;
	cfg.write_times     = (d->inputs == 2u) ? TMU_WRITE_TIMES_2 : TMU_WRITE_TIMES_1;
	/* SIN/COS emit two output words (the scaled result + an auxiliary
     * scaling factor); every other mode emits one.  We always return
     * the first word; reads are sized accordingly so the TMU END flag
     * clears cleanly. */
	const bool sin_or_cos = (d->mode == TMU_MODE_SIN) || (d->mode == TMU_MODE_COS);
	cfg.read_times        = sin_or_cos ? TMU_READ_TIMES_2 : TMU_READ_TIMES_1;
	tmu_init(&cfg);

	/* Issue the op.  For SIN/COS the host's `in_b` is unused (host
     * wrapper passes 0); the TMU's `m*sin(theta)` form needs a
     * non-zero modulus, so substitute unit-scale (1.0 in the active
     * format).  For native 2-input modes (ATAN2, MODULUS) the host's
     * in_b is the actual second operand.
     *
     * ANGLE UNITS: the TMU's SIN/COS angle input is in units of PI
     * (the vendor example writes `theta / DEMO_PI`), while the wire
     * contract is RADIANS -- normalize the F32 angle here.  Q31
     * angles are theta/pi by definition of the fixed-point format
     * and pass through unchanged.  (Caught building the functional
     * tier 2026-06-04: sin(0) == 0 in both conventions, so the soak's
     * zero-angle probe could never see the unit mismatch.) */
	uint32_t a = in_a;
	uint32_t b = in_b;
	if (sin_or_cos) {
		if (format == 1u) {
			a = f32_to_bits(bits_to_f32(in_a) / 3.14159265358979f);
			b = f32_to_bits(1.0f); /* IEEE-754 unit modulus */
		} else {
			b = 0x7FFFFFFFu; /* Q31 ~1.0 */
		}
	}

	/* PACED input writes -- do NOT use the vendor tmu_two_*_write():
     * it issues the two IDATA stores back-to-back, and with a warm
     * i-cache they land ~1 AHB cycle apart and the TMU SWALLOWS the
     * second word; the engine then waits forever for input word 2
     * and ENDF never sets.  Silicon 2026-06-04: SIN computed exactly
     * once per power-up (the cache-cold first call) and timed out on
     * every subsequent call, while hand-driven SWD register writes --
     * milliseconds apart -- always computed.  A TMU_CS read-back
     * between the stores forces an AHB round-trip so the input latch
     * takes word 1 before word 2 arrives.  Raw IDATA stores also skip
     * the bits->float->bits soft-float churn (the wire carries the
     * IEEE-754 pattern already). */
	TMU_IDATA = a;
	if (d->inputs == 2u) {
		(void)TMU_CS;
		TMU_IDATA = b;
	}

	/* Wait for END.  Bounded so a misconfigured op doesn't hang. */
	uint32_t to = TMU_COMPUTE_TIMEOUT;
	while ((RESET == tmu_flag_get(TMU_FLAG_END)) && --to) {
		/* spin */
	}
	if (to == 0u) return BRIDGE_HW_ERR_IO;
	if (SET == tmu_flag_get(TMU_FLAG_OVRF)) {
		tmu_flag_clear(TMU_FLAG_OVRF);
		return BRIDGE_HW_ERR_RANGE;
	}

	/* Drain output(s).  For SIN/COS the second word is the aux
     * scaling factor; we discard it.  Single-output modes need
     * only one read.
     *
     * ANGLE UNITS, output side: ATAN/ATAN2 results come out of the
     * CORDIC in units of PI (the mirror image of the SIN/COS input
     * convention above) -- scale back to the wire's RADIANS for F32.
     * Caught by the functional tier 2026-06-04: atan(1) answered
     * 0.25 (pi/4 in pi-units) with STATUS_OK.  Q31 angle results stay
     * native (theta/pi is the documented Q31 convention). */
	const bool angle_out = (d->mode == TMU_MODE_ATAN) || (d->mode == TMU_MODE_ATAN2);

	if (sin_or_cos) {
		if (format == 1u) {
			float fr;
			float faux;
			tmu_two_f32_read(&fr, &faux);
			*result_out = f32_to_bits(fr);
		} else {
			uint32_t aux;
			tmu_two_q31_read(result_out, &aux);
		}
	} else {
		if (format == 1u) {
			float fr;
			tmu_one_f32_read(&fr);
			if (angle_out) {
				fr *= 3.14159265358979f;
			}
			*result_out = f32_to_bits(fr);
		} else {
			tmu_one_q31_read(result_out);
		}
	}
	return BRIDGE_HW_OK;
}
