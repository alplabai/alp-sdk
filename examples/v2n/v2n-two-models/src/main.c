/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-two-models -- run two models at the same time on two different
 * NPUs of an E1M-V2M: one on the RZ/V2N's on-die DRP-AI3, one on the
 * DEEPX DX-M1 behind PCIe, each in its own thread, all through the
 * portable <alp/inference.h> surface.  Prints per-NPU latency and the
 * combined frame rate.
 *
 * What this example shows
 * ========================
 *
 *   1. Backend per HANDLE.  `alp_inference_config_t.backend` picks the
 *      accelerator for that one handle, so two handles in one process can
 *      sit on two different NPUs.  Here: ALP_INFERENCE_BACKEND_DRPAI for
 *      the first, ALP_INFERENCE_BACKEND_DEEPX_DXM1 for the second.  Do
 *      not use ALP_INFERENCE_BACKEND_AUTO for this -- AUTO is decided at
 *      build time and always resolves to the SAME backend (DEEPX if it is
 *      compiled in), so two AUTO handles would share one NPU.
 *   2. One thread per NPU.  An NPU executes one job at a time, so the way
 *      to keep both busy is one worker per NPU.  The two NPUs are
 *      independent hardware (separate drivers, IRQs and DMA engines), so
 *      their jobs can overlap; combined throughput should approach the
 *      sum of the two, not their average.  (They still share DDR
 *      bandwidth and the A55s that do pre/post-processing.)
 *   3. Why not two threads on ONE NPU?  The SDK serialises DRP-AI jobs
 *      with a process-wide lock (the driver rejects a second concurrent
 *      job), and the DX-M1 time-shares between engines on its own.  A
 *      second thread on the same NPU only queues; it adds latency, not
 *      throughput.  Several models on one NPU are fine as handles, but
 *      they take turns.
 *   4. Measuring latency with `alp_inference_last_invoke_latency_us()`:
 *      the SDK times each successful invoke(), so each worker reads its
 *      own handle's figure after every call.
 *
 * Opening order matters a little
 * ===============================
 *
 *   Both handles are opened on the MAIN thread, one after the other,
 *   before any worker starts.  Opening loads a model into the NPU (the
 *   DRP-AI backend places each model in its own range of the DRP-AI
 *   working-memory arena; the DX-M1 backend builds a dx_rt engine), and
 *   keeping that sequential gives a clear error message per NPU instead
 *   of two interleaved failures.  The SDK's handle pool defaults to 4
 *   slots (ALP_SDK_MAX_INFERENCE_HANDLES), plenty for this demo.
 *
 * Inputs
 * ======
 *
 *   usage: v2n-two-models <drpai-model.tar> <drpai-frame.bin>
 *                         <model.dxnn> <dxnn-frame.bin> [seconds] [mode]
 *
 *   No compiled model ships in this repository (alp-sdk#2236), so both
 *   models and one raw input frame each are arguments:
 *     - drpai-model.tar  a `drpai_dir` bundle tar (see
 *                        docs/bring-up-drpai-v2n.md Sec 2 and 5 and
 *                        examples/v2n/v2n-drpai-inference for how it is
 *                        produced and what the frame layout is).
 *     - model.dxnn       a DEEPX `.dxnn` model compiled with dxcom.
 *     - *-frame.bin      the raw bytes of the model's single input
 *                        tensor, exactly as many bytes as the model's
 *                        input tensor holds (checked against
 *                        alp_inference_get_input() below).
 *   `seconds` (default 10) is how long each phase runs.  `mode` is
 *   `all` (default: DRP-AI alone, DX-M1 alone, then both -- the solo
 *   lines are the baseline that shows what sharing the A55 cores and DDR
 *   costs), `solo-drpai`, `solo-dx` or `both`.  The same frame
 *   is invoked over and over: this is a throughput demo, not an
 *   accuracy one, so the outputs are not decoded or printed.
 *
 * What actually ran
 * ==================
 *
 *   NOT bench-verified.  This builds against the public header only; it
 *   has not been run on an E1M-V2M with both backends enabled.  The
 *   V2M image needs both stacks AND an alp-sdk built with the DRP-AI
 *   backend, which only compiles when RUHMI_DRPAI_TVM_DIR (an
 *   account-gated Renesas checkout) is configured; without it the DRP-AI
 *   open fails with ALP_ERR_NOSUPPORT (see docs/bring-up-drpai-v2n.md).  A failed DRP-AI invoke cannot be
 *   detected by the SDK (the vendor Run() returns void), so a model that
 *   fails on the NPU can still look like a fast, successful invoke here.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alp/inference.h"

#define DEFAULT_SECONDS 10

/* Everything one worker thread needs and reports.  `name` is only for the
 * log lines; `inf` is opened by main() and owned by it. */
struct worker {
	const char      *name;
	alp_inference_t *inf;
	double           run_s;   /* how long to keep invoking */
	uint64_t         invokes; /* out: successful invokes */
	uint64_t         lat_sum_us;
	uint64_t         lat_max_us;
	alp_status_t     error; /* out: first failure, ALP_OK if none */
};

/* ---- small helpers ----------------------------------------------------- */

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

/* Read a whole file into a malloc'd buffer (caller frees).  NULL + message
 * on any error. */
static void *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fprintf(stderr, "error: cannot seek '%s'\n", path);
		fclose(f);
		return NULL;
	}
	long len = ftell(f);
	if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "error: '%s' is empty or unreadable\n", path);
		fclose(f);
		return NULL;
	}
	void *buf = malloc((size_t)len);
	if (buf == NULL) {
		fprintf(stderr, "error: out of memory reading '%s'\n", path);
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		fprintf(stderr, "error: short read on '%s'\n", path);
		free(buf);
		return NULL;
	}
	*out_len = (size_t)len;
	return buf;
}

/* Open one model on a chosen backend and copy the frame into its input
 * tensor.  Returns the handle, or NULL after printing why.
 *
 * `backend` is the whole point: the caller names the NPU for this handle.
 * `format` has to match what that backend loads (a DRP-AI bundle tar or a
 * DEEPX .dxnn); open() refuses a mismatch with ALP_ERR_INVAL. */
static alp_inference_t *open_model(const char                  *tag,
                                   alp_inference_backend_t      backend,
                                   alp_inference_model_format_t format,
                                   const void                  *model,
                                   size_t                       model_len,
                                   const void                  *frame,
                                   size_t                       frame_len)
{
	alp_inference_config_t cfg = {
		.model_data = model,
		.model_size = model_len,
		.format     = format,
		.backend    = backend,
	};
	alp_inference_t *inf = alp_inference_open(&cfg);
	if (inf == NULL) {
		/* The reason is in the thread-local last error.  Typical here:
		 * ALP_ERR_NOSUPPORT (that backend is not built into this image),
		 * ALP_ERR_NOMEM (DRP-AI arena or handle pool full), ALP_ERR_BUSY
		 * (DX-M1 core sets exhausted), ALP_ERR_IO (device absent). */
		alp_status_t err = alp_last_error();
		fprintf(
		    stderr, "[two-npu] %s: open failed: %d (%s)\n", tag, (int)err, alp_status_name(err));
		return NULL;
	}

	alp_inference_tensor_t in = { 0 };
	if (alp_inference_get_input(inf, 0u, &in) != ALP_OK || in.data == NULL) {
		fprintf(stderr, "[two-npu] %s: cannot get input tensor 0\n", tag);
		alp_inference_close(inf);
		return NULL;
	}
	/* The backend's tensor size is authoritative.  Refuse a frame of any
	 * other size rather than feed the NPU a short or truncated tensor. */
	if (in.size_bytes != frame_len) {
		fprintf(stderr,
		        "[two-npu] %s: frame is %zu bytes but the model input is %zu bytes\n",
		        tag,
		        frame_len,
		        in.size_bytes);
		alp_inference_close(inf);
		return NULL;
	}
	/* The input buffer is SDK-owned and stays valid until close(), so the
	 * frame is copied ONCE here and every invoke reuses it. */
	memcpy(in.data, frame, frame_len);

	printf("[two-npu] %s: handle open (%zu input, %zu output)\n",
	       tag,
	       alp_inference_num_inputs(inf),
	       alp_inference_num_outputs(inf));
	return inf;
}

/* ---- worker thread ------------------------------------------------------ */

/* Invoke the worker's model back to back until the time is up.  Each
 * thread touches only its own handle and its own struct, so no locking is
 * needed here; cross-handle serialisation (if any) happens inside the SDK. */
static void *worker_main(void *arg)
{
	struct worker *w   = arg;
	const double   end = now_s() + w->run_s;

	while (now_s() < end) {
		alp_status_t rc = alp_inference_invoke(w->inf);
		if (rc != ALP_OK) {
			w->error = rc;
			break;
		}
		/* Per-call latency measured by the SDK around this invoke. */
		uint64_t us = 0;
		if (alp_inference_last_invoke_latency_us(w->inf, &us) == ALP_OK) {
			w->lat_sum_us += us;
			if (us > w->lat_max_us) {
				w->lat_max_us = us;
			}
		}
		w->invokes++;
	}
	return NULL;
}

static void report(const struct worker *w, double wall_s)
{
	if (w->invokes == 0u) {
		printf("[two-npu] %s: 0 invokes\n", w->name);
		return;
	}
	printf("[two-npu] %s: %llu invokes, avg %.2f ms, max %.2f ms, %.1f FPS\n",
	       w->name,
	       (unsigned long long)w->invokes,
	       (double)w->lat_sum_us / (double)w->invokes / 1000.0,
	       (double)w->lat_max_us / 1000.0,
	       (double)w->invokes / wall_s);
}

/* Print every SoC thermal zone's temperature (millidegrees C, straight from
 * sysfs).  Called before and after the run so a long run shows thermal
 * drift -- the two NPUs together are the heaviest load this board sees and
 * their combined power and thermal behaviour has not been measured.  A
 * missing zone is skipped silently: this is a diagnostic, not a gate. */
static void print_temps(const char *when)
{
	for (int z = 0; z < 8; ++z) {
		char path[64];
		snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
		FILE *f = fopen(path, "r");
		if (f == NULL) {
			continue;
		}
		long mc = 0;
		if (fscanf(f, "%ld", &mc) == 1) {
			printf("[two-npu] temp %s: thermal_zone%d = %ld mC\n", when, z, mc);
		}
		fclose(f);
	}
}

/* Run `n` workers at once for `seconds`, then print each one's numbers and,
 * for n > 1, the combined FPS.  Returns 0 on success, 1 on any failure.
 * Comparing the "solo" lines with the "both" line shows what running the
 * NPUs together costs each of them. */
static int run_phase(const char *label, struct worker *ws, int n, int seconds)
{
	pthread_t tid[2];
	int       started = 0, rc = 0;

	printf("[two-npu] phase: %s\n", label);
	for (int i = 0; i < n; ++i) {
		ws[i].run_s      = (double)seconds;
		ws[i].invokes    = 0u;
		ws[i].lat_sum_us = 0u;
		ws[i].lat_max_us = 0u;
		ws[i].error      = ALP_OK;
	}
	const double t0 = now_s();
	for (; started < n; ++started) {
		if (pthread_create(&tid[started], NULL, worker_main, &ws[started]) != 0) {
			fprintf(stderr, "[two-npu] cannot start worker %d\n", started);
			rc = 1;
			break;
		}
	}
	for (int i = 0; i < started; ++i) {
		pthread_join(tid[i], NULL);
	}
	const double wall_s = now_s() - t0;

	uint64_t total = 0;
	for (int i = 0; i < started; ++i) {
		report(&ws[i], wall_s);
		total += ws[i].invokes;
		if (ws[i].error != ALP_OK) {
			fprintf(stderr,
			        "[two-npu] %s: invoke failed: %s\n",
			        ws[i].name,
			        alp_status_name(ws[i].error));
			rc = 1;
		}
	}
	if (n > 1) {
		printf("[two-npu] combined: %llu invokes in %.2f s = %.1f FPS\n",
		       (unsigned long long)total,
		       wall_s,
		       (double)total / wall_s);
	}
	return rc;
}

/* ---- entry point --------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc < 5 || argc > 7) {
		fprintf(stderr,
		        "usage: %s <drpai-model.tar> <drpai-frame.bin> <model.dxnn> "
		        "<dxnn-frame.bin> [seconds] [all|solo-drpai|solo-dx|both]\n"
		        "  Runs the DRP-AI model and the DX-M1 model at the same time\n"
		        "  (mode 'all', the default: each alone first, then both).\n"
		        "  No models ship in-tree; see README.md.\n",
		        argv[0]);
		return 2;
	}
	int seconds = (argc >= 6) ? atoi(argv[5]) : DEFAULT_SECONDS;
	if (seconds <= 0) {
		fprintf(stderr, "error: seconds must be a positive integer\n");
		return 2;
	}

	const char *mode = (argc == 7) ? argv[6] : "all";
	if (strcmp(mode, "all") != 0 && strcmp(mode, "solo-drpai") != 0 &&
	    strcmp(mode, "solo-dx") != 0 && strcmp(mode, "both") != 0) {
		fprintf(stderr, "error: mode must be all, solo-drpai, solo-dx or both\n");
		return 2;
	}

	/* CPU budget.  The DRP-AI TVM runtime runs the model's CPU-side ops on
	 * its own thread pool and, by upstream TVM's default, pins one worker to
	 * each core (TVM_BIND_THREADS).  On the 4 A55 cores that pool would
	 * compete with the DX-M1 runtime's worker threads, dxrtd, this program's
	 * two inference threads and both models' pre/post-processing.  Cap the
	 * pool and let the scheduler place the threads.  overwrite=0: a value
	 * the user exported wins.  Must be set before the first DRP-AI open().
	 * The numbers are a starting point, not a measurement: tune by running
	 * with different values and comparing the "both" phase. */
	setenv("TVM_NUM_THREADS", "2", 0);
	setenv("TVM_BIND_THREADS", "0", 0);

	size_t drpai_model_len = 0, drpai_frame_len = 0, dx_model_len = 0, dx_frame_len = 0;
	void  *drpai_model = read_file(argv[1], &drpai_model_len);
	void  *drpai_frame = read_file(argv[2], &drpai_frame_len);
	void  *dx_model    = read_file(argv[3], &dx_model_len);
	void  *dx_frame    = read_file(argv[4], &dx_frame_len);

	int              exit_code = 1;
	alp_inference_t *drpai     = NULL;
	alp_inference_t *dx        = NULL;
	if (drpai_model == NULL || drpai_frame == NULL || dx_model == NULL || dx_frame == NULL) {
		goto out;
	}

	printf("[two-npu] v2n-two-models: DRP-AI + DX-M1, %d s\n", seconds);

	/* ---- stage 1: open both models, sequentially, one per NPU ---- */

	drpai = open_model("drpai",
	                   ALP_INFERENCE_BACKEND_DRPAI,
	                   ALP_INFERENCE_MODEL_DRPAI,
	                   drpai_model,
	                   drpai_model_len,
	                   drpai_frame,
	                   drpai_frame_len);
	if (drpai == NULL) {
		goto out;
	}
	dx = open_model("deepx",
	                ALP_INFERENCE_BACKEND_DEEPX_DXM1,
	                ALP_INFERENCE_MODEL_DXNN,
	                dx_model,
	                dx_model_len,
	                dx_frame,
	                dx_frame_len);
	if (dx == NULL) {
		goto out;
	}

	/* ---- stage 2: run the phases ---- */

	/* Three phases, so the cost of sharing is printed directly: each NPU
	 * alone, then both together.  `mode` picks a subset. */
	struct worker w[2] = {
		{ .name = "drpai", .inf = drpai },
		{ .name = "deepx", .inf = dx },
	};
	exit_code = 0;
	print_temps("start");
	if (strcmp(mode, "all") == 0 || strcmp(mode, "solo-drpai") == 0) {
		exit_code |= run_phase("solo drpai", &w[0], 1, seconds);
	}
	if (strcmp(mode, "all") == 0 || strcmp(mode, "solo-dx") == 0) {
		exit_code |= run_phase("solo deepx", &w[1], 1, seconds);
	}
	if (strcmp(mode, "all") == 0 || strcmp(mode, "both") == 0) {
		exit_code |= run_phase("both", w, 2, seconds);
	}
	print_temps("end");
	printf("[two-npu] done\n");

out:
	/* Close handles before freeing the model bytes: the DEEPX backend keeps
	 * a pointer to them for the lifetime of the handle. */
	if (dx != NULL) {
		alp_inference_close(dx);
	}
	if (drpai != NULL) {
		alp_inference_close(drpai);
	}
	free(drpai_model);
	free(drpai_frame);
	free(dx_model);
	free(dx_frame);
	return exit_code;
}
