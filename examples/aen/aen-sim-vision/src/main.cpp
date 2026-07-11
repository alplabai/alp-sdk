/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sim-vision -- a minimal camera -> inference -> display pipeline that
 * runs REAL TensorFlow-Lite-Micro inference on the AEN M55-HP, driven by
 * the `west alp-renode --sim-mode` studio hardware simulator (#687).
 *
 * The studio gateway injects a "camera frame" into SIM_FRAME (the camera
 * `inject.memcpy` base), then fires SIM_DOORBELL (its trigger).  This app
 * derives a feature from the frame, runs the TFLM model on the M55 (no NPU
 * -- the software path, Helium-accelerated), renders the result into the
 * ssd1306 MONO frame buffer (SIM_DISPLAY, the descriptor framebuffers[]
 * base), and logs an `inference:` line to the console.  Studio reads the
 * frame buffer + console back over the sim sockets.
 *
 * The buffers sit at fixed SRAM0 addresses the alif_ensemble_e8 sim model
 * maps -- they match the E1M-AEN801 profile in scripts/west_commands/
 * alp_renode.py.  Swapping the (sine) model for a real vision net is a
 * model.cpp drop-in; the pipeline is unchanged.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "model.hpp"

/* Sim contract -- fixed SRAM0 buffers (see the E1M-AEN801 descriptor). */
#define SIM_FRAME       ((volatile uint8_t *)0x20041000)  /* camera in     */
#define SIM_FRAME_LEN   1024
#define SIM_DOORBELL    ((volatile uint32_t *)0x20042000) /* trigger       */
#define SIM_DISPLAY     ((volatile uint8_t *)0x20040000)  /* ssd1306 fb out */
#define SIM_DISPLAY_LEN 1024                              /* 128x64 / 8    */

namespace {
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
constexpr int kTensorArenaSize = 2048;
alignas(16) uint8_t tensor_arena[kTensorArenaSize];
} /* namespace */

int main(void)
{
	tflite::InitializeTarget();

	model = tflite::GetModel(g_model);
	if (model->version() != TFLITE_SCHEMA_VERSION) {
		printk("aen-sim-vision: model schema %d != %d\n",
		       (int)model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

	static tflite::MicroMutableOpResolver<1> resolver;
	resolver.AddFullyConnected();

	static tflite::MicroInterpreter static_interpreter(
		model, resolver, tensor_arena, kTensorArenaSize);
	interpreter = &static_interpreter;
	if (interpreter->AllocateTensors() != kTfLiteOk) {
		printk("aen-sim-vision: AllocateTensors failed\n");
		return -1;
	}
	input = interpreter->input(0);
	output = interpreter->output(0);

	*SIM_DOORBELL = 0U;
	printk("aen-sim-vision: ready (frame=%p doorbell=%p display=%p)\n",
	       (void *)SIM_FRAME, (void *)SIM_DOORBELL, (void *)SIM_DISPLAY);

	while (1) {
		if (*SIM_DOORBELL == 0U) {
			k_msleep(20);
			continue;
		}

		/* Feature: mean intensity of the injected frame. */
		uint32_t sum = 0U;
		for (int i = 0; i < SIM_FRAME_LEN; i++) {
			sum += SIM_FRAME[i];
		}
		uint8_t mean = (uint8_t)(sum / SIM_FRAME_LEN);

		/* Map mean [0,255] -> x [0,2pi], quantise, run the model. */
		float x = ((float)mean / 255.0f) * 6.28318f;
		input->data.int8[0] =
			(int8_t)(x / input->params.scale + input->params.zero_point);
		(void)interpreter->Invoke();
		float y = ((int)output->data.int8[0] - output->params.zero_point) *
			  output->params.scale;

		/* Render y [-1,1] as a filled bar in the MONO frame buffer. */
		int fill = (int)((y + 1.0f) * 0.5f * (float)SIM_DISPLAY_LEN);
		for (int i = 0; i < SIM_DISPLAY_LEN; i++) {
			SIM_DISPLAY[i] = (i < fill) ? 0xFFU : 0x00U;
		}

		/* Fixed-point log (avoid float printf): y in milli-units. */
		int y_milli = (int)(y * 1000.0f);
		printk("inference: frame_mean=%u y_milli=%d fill=%d\n",
		       mean, y_milli, fill);

		*SIM_DOORBELL = 0U;
	}
	return 0;
}
