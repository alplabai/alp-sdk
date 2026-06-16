/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/inference.h>.  The bench measures the
 * dispatcher overhead (cfg validation + backend lookup) without
 * actually running inference -- those numbers matter to apps that
 * sit in a tight probe-then-fallback loop around the SDK.
 *
 * v1.0 will add per-backend full invoke benches (model load + 1000x
 * invoke + close) once the HIL CI is online and DEEPX / Ethos-U
 * runtimes are linked in.
 */

#include <stddef.h>
#include <stdint.h>

#include "bench.h"

#include "alp/inference.h"

static const uint8_t k_fake_model[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };

void                 bench_inference_main(void)
{
	/* Null-cfg rejection -- earliest exit. */
	BENCH_RUN("alp_inference_open(NULL cfg)", 1000000, { (void)alp_inference_open(NULL); });

	/* Null model -- second exit. */
	BENCH_RUN("alp_inference_open(NULL model)", 1000000, {
		alp_inference_config_t cfg = { 0 };
		cfg.backend                = ALP_INFERENCE_BACKEND_AUTO;
		(void)alp_inference_open(&cfg);
	});

	/* AUTO with no backend compiled in -- exercises resolve_auto's
     * "nothing available" fall-through.  On stub backends this is
     * the same shape every NPU-targeting app pays on a host build. */
	BENCH_RUN("alp_inference_open(AUTO, no backend)", 100000, {
		alp_inference_config_t cfg = { 0 };
		cfg.backend                = ALP_INFERENCE_BACKEND_AUTO;
		cfg.model_data             = k_fake_model;
		cfg.model_size             = sizeof(k_fake_model);
		cfg.format                 = ALP_INFERENCE_MODEL_TFLITE;
		(void)alp_inference_open(&cfg);
	});

	/* num_inputs / num_outputs / get_input on a closed handle -- the
     * NULL-handle guards every wrapper function carries.  Cheap. */
	BENCH_RUN("alp_inference_num_inputs(NULL)", 1000000, { (void)alp_inference_num_inputs(NULL); });

	BENCH_RUN("alp_inference_num_outputs(NULL)", 1000000,
	          { (void)alp_inference_num_outputs(NULL); });
}
