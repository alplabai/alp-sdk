/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sim-vision -- a minimal camera -> inference -> display pipeline AND a
 * mic -> inference -> wake-word path, both running REAL TensorFlow-Lite-Micro
 * inference on the AEN M55-HP, driven by the studio hardware simulator
 * (#687) via `tan renode` (the `--sim-mode` injection harness is a pending
 * stub in tan -- tan-cli#674).
 *
 * Two independent studio-driven pipelines share one image (issue #687's
 * acceptance is a single AEN M55 image serving both):
 *
 *   Vision:  studio injects a "camera frame" into SIM_FRAME (the camera
 *            `inject.memcpy` base), then fires SIM_DOORBELL (its trigger).
 *            The app derives a feature from the frame, runs the TFLM model
 *            (no NPU -- the software path, Helium-accelerated), renders the
 *            result into the ssd1306 MONO frame buffer (SIM_DISPLAY, the
 *            descriptor framebuffers[] base), and logs an `inference:` line.
 *
 *   Audio:   studio injects an "audio clip" into SIM_AUDIO (the mic
 *            `inject.memcpy` base), then fires SIM_AUDIO_DOORBELL.  The app
 *            derives a loudness feature, runs the SAME TFLM model for a
 *            confidence score, gates a wake word on the feature, and logs a
 *            `wakeword:` line.  A real KWS net (swap model.cpp) makes the
 *            model output the classifier score; the placeholder sine model
 *            can't classify, so the loudness gate stands in as the detector.
 *
 * Studio reads the frame buffer + console back over the sim sockets.
 *
 * The buffers sit at fixed SRAM0 addresses the alif_ensemble_e8 sim model
 * maps -- they match the E1M-AEN801 profile `tan renode` resolves.
 * Swapping the (sine) model for a real net is a model.cpp
 * drop-in; the pipelines are unchanged.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include "model.hpp"

/* Sim contract -- fixed SRAM0 buffers (see the E1M-AEN801 descriptor). */
#define SIM_FRAME          ((volatile uint8_t *)0x20041000)  /* camera in      */
#define SIM_FRAME_LEN      1024
#define SIM_DOORBELL       ((volatile uint32_t *)0x20042000) /* camera trigger */
#define SIM_DISPLAY        ((volatile uint8_t *)0x20040000)  /* ssd1306 fb out */
#define SIM_DISPLAY_LEN    1024                              /* 128x64 / 8     */
#define SIM_AUDIO          ((volatile uint8_t *)0x20043000)  /* mic clip in    */
#define SIM_AUDIO_LEN      1024                              /* u8 PCM samples */
#define SIM_AUDIO_DOORBELL ((volatile uint32_t *)0x20044000) /* mic trigger    */

/* Loudness above this gates a wake word (u8 PCM centred on 0x80 silence). */
#define WAKE_ENERGY_THRESHOLD 24U

namespace {
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interpreter = nullptr;
TfLiteTensor *input = nullptr;
TfLiteTensor *output = nullptr;
constexpr int kTensorArenaSize = 2048;
alignas(16) uint8_t tensor_arena[kTensorArenaSize];

/* Quantise x, run the model, dequantise the scalar output. */
float run_model(float x)
{
	input->data.int8[0] =
		(int8_t)(x / input->params.scale + input->params.zero_point);
	(void)interpreter->Invoke();
	return ((int)output->data.int8[0] - output->params.zero_point) *
	       output->params.scale;
}

/* Camera frame -> inference -> ssd1306 frame buffer + `inference:` log. */
void run_vision(void)
{
	/* Feature: mean intensity of the injected frame. */
	uint32_t sum = 0U;
	for (int i = 0; i < SIM_FRAME_LEN; i++) {
		sum += SIM_FRAME[i];
	}
	uint8_t mean = (uint8_t)(sum / SIM_FRAME_LEN);

	/* Map mean [0,255] -> x [0,2pi], run the model. */
	float y = run_model(((float)mean / 255.0f) * 6.28318f);

	/* Render y [-1,1] as a filled bar in the MONO frame buffer. */
	int fill = (int)((y + 1.0f) * 0.5f * (float)SIM_DISPLAY_LEN);
	for (int i = 0; i < SIM_DISPLAY_LEN; i++) {
		SIM_DISPLAY[i] = (i < fill) ? 0xFFU : 0x00U;
	}

	/* Fixed-point log (avoid float printf): y in milli-units. */
	printk("inference: frame_mean=%u y_milli=%d fill=%d\n",
	       mean, (int)(y * 1000.0f), fill);
}

/* Mic clip -> inference -> `wakeword:` log (detected when loud enough). */
void run_audio(void)
{
	/* Feature: mean absolute deviation from 0x80 silence (loudness). */
	uint32_t sum = 0U;
	for (int i = 0; i < SIM_AUDIO_LEN; i++) {
		int s = (int)SIM_AUDIO[i] - 128;
		sum += (uint32_t)(s < 0 ? -s : s);
	}
	uint8_t energy = (uint8_t)(sum / SIM_AUDIO_LEN);

	/* Confidence score from the (placeholder) net; a real KWS model makes
	 * this the classifier output.  Map energy [0,127] -> x [0,2pi].
	 */
	float conf = run_model(((float)energy / 127.0f) * 6.28318f);
	bool detected = energy >= WAKE_ENERGY_THRESHOLD;

	printk("wakeword: %s energy=%u conf_milli=%d\n",
	       detected ? "detected" : "absent", energy, (int)(conf * 1000.0f));
}
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
	*SIM_AUDIO_DOORBELL = 0U;
	printk("aen-sim-vision: ready (frame=%p doorbell=%p display=%p "
	       "audio=%p audio_doorbell=%p)\n",
	       (void *)SIM_FRAME, (void *)SIM_DOORBELL, (void *)SIM_DISPLAY,
	       (void *)SIM_AUDIO, (void *)SIM_AUDIO_DOORBELL);

	while (1) {
		bool idle = true;

		if (*SIM_DOORBELL != 0U) {
			run_vision();
			*SIM_DOORBELL = 0U;
			idle = false;
		}
		if (*SIM_AUDIO_DOORBELL != 0U) {
			run_audio();
			*SIM_AUDIO_DOORBELL = 0U;
			idle = false;
		}
		if (idle) {
			k_msleep(20);
		}
	}
	return 0;
}
