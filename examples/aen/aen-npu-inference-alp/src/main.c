/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-npu-inference-alp -- the REAL person_detect MobileNet running on the
 * E1M-AEN801 (Ensemble E8) Ethos-U85 NPU *through the portable
 * <alp/inference.h> API* -- the SDK-backed sibling of
 * aen-npu-inference-person-mram (which drives the NPU via the raw Arm
 * InferenceProcess wrapper).
 *
 * The whole point: a customer writes only alp_inference_open() / invoke() /
 * get_output().  The AEN Ethos-U backend (src/backends/inference/ethos_u_aen.cpp)
 * supplies the NPU AXI address-remap + SRAM-port config-select strong overrides;
 * the app does NOT hand-write them (contrast the raw sibling, which does).
 *
 * DMA placement (the #1 silicon rule): the Ethos-U is a DMA master that reads
 * the model + arena and writes the output over its own AXI master.  Those
 * buffers MUST live in NPU-reachable global SRAM0 (@0x02000000), NOT the
 * M55-local ITCM/DTCM.  So the model is memcpy'd from rodata into an SRAM0
 * buffer, and the tensor arena is placed in the "SRAM0" linker region; the
 * input/output tensors live inside that arena, so they are SRAM0-resident too.
 *
 * Model: person_detect_u85, Vela-compiled for ethos-u85-256 with the Alif
 * Ethos_U85_SRAM_Only system-config (see CMakeLists.txt / gen_model.py).  ~263
 * KiB -> overflows ITCM, so this image links into MRAM slot0 and boots via
 * Flow D.  RESULT PASS = alp_inference_invoke succeeded AND the NPU actually
 * wrote a non-zero output tensor.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "alp/inference.h"
#include "alp/peripheral.h" /* alp_status_name / alp_last_error */

/* gen_model.py output: networkModelData[], NETWORK_MODEL_LEN,
 * NETWORK_MODEL_NAME, TENSOR_ARENA_SIZE, NPU_ALIGN, TENSOR_ARENA_ATTR
 * (= __aligned(16) section("SRAM0")). */
#include "model.h"

/* Model staged into DMA-reachable SRAM0.  networkModelData[] is const rodata
 * (in MRAM/flash); the NPU reads the fused command stream + weights via its own
 * AXI master, so the executed bytes MUST be in NPU-reachable SRAM0.  An
 * *initialised* array in a custom section is NOT init-copied by Zephyr -> keep
 * the source in rodata and memcpy into this uninitialised SRAM0 buffer. */
static uint8_t model_sram[NETWORK_MODEL_LEN] __aligned(NPU_ALIGN) __attribute__((section("SRAM0")));

/* Tensor arena in SRAM0 (TENSOR_ARENA_ATTR = __aligned(16) section("SRAM0")).
 * The MicroInterpreter lays the input + output tensors inside this arena, so
 * they are NPU-reachable too. */
TENSOR_ARENA_ATTR static uint8_t tensor_arena[TENSOR_ARENA_SIZE];

/* Evidence latched for a SWD symbol read, independent of the console decode. */
volatile int      g_invoke_status = -99;
volatile uint32_t g_out_bytes     = 0;
volatile uint8_t  g_out_first[16] = { 0 };
volatile int      g_argmax        = -1;

int main(void)
{
	printk("\n=== aen-npu-inference-alp (alp_inference_open -> Ethos-U85) ===\n");
	printk("model      : %s (%u bytes)\n", NETWORK_MODEL_NAME, (unsigned)NETWORK_MODEL_LEN);
	printk(
	    "arena      : %u bytes @ SRAM0 (%p)\n", (unsigned)TENSOR_ARENA_SIZE, (void *)tensor_arena);

	/* Stage the Vela model into DMA-reachable SRAM0. */
	memcpy(model_sram, networkModelData, NETWORK_MODEL_LEN);

	/* Open the model through the portable API.  On E8 the AEN Ethos-U backend
	 * (priority 100, silicon_ref alif:ensemble:e8) wins over the portable CPU
	 * TFLM path and dispatches onto the NPU. */
	alp_inference_config_t cfg = {
		.model_data  = model_sram,
		.model_size  = NETWORK_MODEL_LEN,
		.format      = ALP_INFERENCE_MODEL_VELA,
		.backend     = ALP_INFERENCE_BACKEND_AUTO,
		.arena_bytes = TENSOR_ARENA_SIZE,
		.arena       = tensor_arena,
	};
	alp_inference_t *inf = alp_inference_open(&cfg);
	if (inf == NULL) {
		printk("RESULT FAIL: alp_inference_open -> NULL (%s)\n", alp_status_name(alp_last_error()));
		return 0;
	}

	/* Fill the input tensor (zero-filled; a real deployment memcpys a captured
	 * frame here).  The tensor's data lives in the SRAM0 arena. */
	alp_inference_tensor_t in = { 0 };
	alp_status_t           s  = alp_inference_get_input(inf, 0, &in);
	if (s == ALP_OK && in.data != NULL) {
		memset(in.data, 0, in.size_bytes);
	}
	printk(
	    "input      : %u bytes @ %p (%s)\n", (unsigned)in.size_bytes, in.data, alp_status_name(s));

	/* Run inference -- the fused Ethos-U custom op dispatches onto the NPU. */
	s               = alp_inference_invoke(inf);
	g_invoke_status = (int)s;
	if (s != ALP_OK) {
		printk("RESULT FAIL: alp_inference_invoke -> %s\n", alp_status_name(s));
		alp_inference_close(inf);
		return 0;
	}

	alp_inference_tensor_t out = { 0 };
	s                          = alp_inference_get_output(inf, 0, &out);
	if (s != ALP_OK || out.data == NULL) {
		printk("RESULT FAIL: alp_inference_get_output -> %s\n", alp_status_name(s));
		alp_inference_close(inf);
		return 0;
	}
	g_out_bytes = (uint32_t)out.size_bytes;

	/* Scan the output: PASS requires a non-zero tensor (an all-zero output
	 * despite invoke OK would mean the NPU never actually wrote it). */
	const uint8_t *ob          = (const uint8_t *)out.data;
	uint32_t       n           = out.size_bytes < 16u ? (uint32_t)out.size_bytes : 16u;
	bool           any_nonzero = false;
	int            best_i      = -1;
	int            best_v      = -128; /* int8 floor */
	for (uint32_t i = 0; i < out.size_bytes; i++) {
		int8_t v = (int8_t)ob[i];
		if (i < n) {
			g_out_first[i] = (uint8_t)v;
		}
		if (v != 0) {
			any_nonzero = true;
		}
		if (v > best_v) {
			best_v = v;
			best_i = (int)i;
		}
	}
	g_argmax = best_i;

	printk("output     : %u bytes, argmax=%d (val=%d), first=",
	       (unsigned)out.size_bytes,
	       best_i,
	       best_v);
	for (uint32_t i = 0; i < n; i++) {
		printk("%02x ", g_out_first[i]);
	}
	printk("\n");

	if ((out.size_bytes > 0) && any_nonzero) {
		printk("RESULT PASS: NPU inference via alp_inference_open -- model=%s "
		       "out_bytes=%u argmax=%d\n",
		       NETWORK_MODEL_NAME,
		       (unsigned)out.size_bytes,
		       best_i);
	} else {
		printk("RESULT FAIL: output not populated (out_bytes=%u nonzero=%d)\n",
		       (unsigned)out.size_bytes,
		       (int)any_nonzero);
	}

	alp_inference_close(inf);
	return 0;
}
