/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-drpai-inference -- run a compiled model through the RZ/V2N's
 * on-die DRP-AI3 NPU via <alp/inference.h>, on one or more still frames
 * given on the command line, and print what a person can read at a
 * booth: per-image results plus timing.
 *
 * What this example shows
 * ========================
 *
 *   1. Load a DRP-AI-compiled model bundle (a `drpai_dir` tar -- see
 *      "Model bundle" below) into memory and open it through the
 *      portable `<alp/inference.h>` surface with
 *      `backend = ALP_INFERENCE_BACKEND_DRPAI` and
 *      `format = ALP_INFERENCE_MODEL_DRPAI`.
 *   2. For each frame path on argv: read it, copy it into the model's
 *      input tensor, run one `alp_inference_invoke()`, and print the
 *      output tensor plus wall-clock timing.
 *   3. Handle the case where DRP-AI is unavailable (no NPU-enabled
 *      alp-sdk build, or a board whose `&drpai0` devicetree node was
 *      never enabled) the way the documented NOSUPPORT contract
 *      requires: `alp_inference_open()` returns NULL, this program
 *      reports why and exits cleanly instead of crashing.
 *
 * Input: raw pre-processed frames, not JPEG/PNG
 * ==============================================
 *
 *   Decoding a real image file needs an image codec library this SDK
 *   does not carry (and adding one just for a demo is out of scope --
 *   see docs/portability.md on keeping the portable surface small).
 *   Instead this example reads RAW pre-processed frames: flat
 *   640x640x3 float32 NCHW buffers, exactly `FRAME_BYTES` bytes each --
 *   planar (the whole R plane, then the whole G plane, then the whole B
 *   plane), NOT interleaved HWC. NCHW is the target model's declared
 *   ONNX input shape (RUHMI's `yolox-S_VOC.onnx`, `1,3,640,640` per
 *   docs/bring-up-drpai-v2n.md Sec 5 and
 *   scripts/alp_model/adapters/drpai.py), and it reaches the model
 *   unchanged: the DRP-AI backend (src/yocto/inference_drpai.cpp) pushes
 *   this buffer straight into the MERA runtime's `SetInput()` and never
 *   runs the compiled bundle's own `preprocess/` step against it. The
 *   runtime cannot catch a layout mistake for you: `alp_inference_get_
 *   input()` reports only a byte count, never a shape (the MERA wrapper
 *   exposes no per-input shape, so rank stays 0 -- see
 *   `alp_inference_drpai_get_input()` in inference_drpai.cpp), so an
 *   interleaved HWC file of the identical byte count passes the size
 *   check below and would be fed to the NPU with a silently wrong
 *   channel ordering. Getting NCHW right is entirely the caller's job.
 *
 *   Channel order (RGB vs BGR), pixel normalisation (raw 0-255 vs /255
 *   vs mean/std), and letterbox padding are UNVERIFIED on this
 *   review host (no vendor sample was run here) -- the vendor's own
 *   `how-to/sample_app_v2h/app_yolox_cam` sample is the authority for
 *   all three; match it exactly, do not guess. A customer with a real
 *   camera/video pipeline (out of scope here -- see issue #1149)
 *   produces frames in this NCHW layout with whatever resize +
 *   normalise + HWC->CHW transpose their capture path already needs. A
 *   host-side sketch with Pillow + NumPy -- RGB, raw 0-255 float32,
 *   plain resize with no letterbox -- is a PLACEHOLDER only, to be
 *   checked against `app_yolox_cam` before trusting it for real
 *   detections:
 *
 *       import numpy as np
 *       from PIL import Image
 *       img = Image.open("photo.jpg").convert("RGB").resize((640, 640))
 *       # HWC (640, 640, 3) -> CHW (3, 640, 640): move the channel axis
 *       # from last to first; this is the transpose the model's NCHW
 *       # input needs and a same-size HWC file would silently skip.
 *       np.asarray(img, dtype=np.float32).transpose(2, 0, 1)[None].tofile("frame0.bin")
 *
 * Model bundle
 * ============
 *
 *   `argv[1]` is the path to a `drpai_dir` bundle tar -- the output of
 *
 *       python3 -m alp_model build --target drpai --product V2N <model.onnx>
 *
 *   (scripts/alp_model/adapters/drpai.py; see docs/bring-up-drpai-v2n.md
 *   Sec 5).  That script tars the compiler's object directory
 *   (drp_desc.bin / weight.bin / addr_map.txt / deploy.json / deploy.so
 *   / preprocess/) deterministically; this program hands the raw tar
 *   bytes to `alp_inference_open()` as `cfg.model_data` exactly as-is
 *   -- the SDK's DRP-AI backend (src/yocto/inference_drpai.cpp) is what
 *   untars it to a private staging directory before loading it into the
 *   vendor runtime. No compiled `drpai_dir` bundle exists yet, though --
 *   docs/bring-up-drpai-v2n.md Sec 5 confirms only an ONNX source
 *   (RUHMI's `yolox-S_VOC.onnx`) exists in a fresh checkout, not a
 *   compiled `drp_desc.bin`/`weight.bin`/`addr_map.txt`/`deploy.json`
 *   set; compiling one for real accuracy needs a real calibration image
 *   set, tracked in alp-sdk#1271, not done here. This program's own
 *   size/byte-for-byte check against that bundle's sample `input_0.bin`
 *   is therefore still owed, not done: that file does not exist in this
 *   checkout either.
 *
 * Output: raw scores, not decoded detections
 * ===========================================
 *
 *   The compiled bundle's `deploy.json` carries a single fused
 *   `mera_drp` op -- the whole YOLOX graph is NPU-offloaded, so
 *   `alp_inference_get_output()` hands back one flat float32 tensor: the
 *   raw, pre-decode network output for all ~8400 candidate boxes across
 *   the model's three feature-map strides (8 / 16 / 32 for a 640x640
 *   input), each carrying box regression + objectness + 20 VOC class
 *   scores.  Turning that into actual (class, confidence, box) triples
 *   needs a real YOLOX decoder: generate each stride's grid + anchor
 *   points, apply sigmoid to the objectness and class-score channels,
 *   regress (cx, cy, w, h) against the matching grid cell, and run
 *   non-maximum-suppression across the whole candidate set. That decode
 *   step is NOT implemented here -- writing one against a model this
 *   SDK has never run on silicon would be unverifiable guesswork, not a
 *   teaching example. Instead `print_top_scores()` below prints the
 *   TOP_N largest raw values in the output tensor, clearly labelled as
 *   raw and undecoded. A customer productising this demo adds the
 *   decoder once real hardware confirms the raw tensor's layout.
 *
 * What actually ran
 * ==================
 *
 *   This file builds clean as a standalone C translation unit against
 *   the real `<alp/inference.h>` header. Linking it against a real
 *   `libalp_sdk` and running it end-to-end through the documented
 *   NOSUPPORT path have NOT been re-verified by this PR: that claim was
 *   carried over from feat/1145-drpai-v2n-bringup -- the branch whose
 *   self-reported claims were found false and which is the reason this
 *   salvage PR exists -- and was never independently re-run. This PR's
 *   own review host is macOS, where the full SDK does not build at all
 *   for an unrelated reason (a `section` attribute error blocks
 *   src/backend.c / src/backends/storage/sw_fallback.c repo-wide), so
 *   the claim stays open, not confirmed, until it runs on Linux. The
 *   frame-size guard and argv parsing have been read and hand-traced
 *   for correctness, not executed. The one non-trivial algorithm here,
 *   top_scores_select() (src/top_scores.c, used by print_top_scores()),
 *   has a real host unit test against hand-built float arrays:
 *   tests/unit/top_scores/. Nothing here has run against real DRP-AI
 *   silicon: `alp_inference_open()` with a real bundle first executes
 *   on a `drpai`-enabled `alp-image-edge` bake on an E1M-X V2N board,
 *   per docs/bring-up-drpai-v2n.md.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alp/inference.h"
#include "top_scores.h"

/* Exact byte size of one 640x640x3 float32 NCHW frame -- see "Input:
 * raw pre-processed frames" above.  A frame file of any other size is
 * rejected up front rather than fed to the NPU short or truncated. */
#define FRAME_BYTES (640u * 640u * 3u * sizeof(float))

/* How many of the raw output values print_top_scores() reports -- see
 * "Output: raw scores, not decoded detections" above. */
#define TOP_N 5u

/* ---- small helpers ----------------------------------------------------- */

/* Read an entire file into a malloc'd buffer; the caller frees it.
 * Returns NULL (and prints why) on any error -- missing file, seek
 * failure, OOM, or a short read. */
static void *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fprintf(stderr, "error: cannot seek '%s': %s\n", path, strerror(errno));
		fclose(f);
		return NULL;
	}
	long len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "error: cannot determine length of '%s'\n", path);
		fclose(f);
		return NULL;
	}

	if (len == 0) {
		fprintf(stderr, "error: '%s' is empty\n", path);
		fclose(f);
		return NULL;
	}

	void *buf = malloc((size_t)len);
	if (buf == NULL) {
		fprintf(stderr, "error: out of memory reading '%s' (%ld bytes)\n", path, len);
		fclose(f);
		return NULL;
	}

	size_t got = fread(buf, 1, (size_t)len, f);
	fclose(f);
	if (got != (size_t)len) {
		fprintf(stderr, "error: short read on '%s' (got %zu of %ld bytes)\n", path, got, len);
		free(buf);
		return NULL;
	}

	*out_len = (size_t)len;
	return buf;
}

/* Print the TOP_N largest values in a flat float32 tensor, largest
 * first -- the documented fallback for a YOLOX output this example does
 * not decode (see "Output: raw scores, not decoded detections" above).
 * The selection itself is top_scores_select() (src/top_scores.c), unit
 * tested standalone against hand-built float arrays in
 * tests/unit/top_scores/ -- this wrapper only owns the printing. */
static void print_top_scores(const float *values, size_t count)
{
	size_t top_idx[TOP_N];
	float  top_val[TOP_N];
	size_t n = 0;

	top_scores_select(values, count, TOP_N, top_idx, top_val, &n);

	for (size_t i = 0; i < n; ++i) {
		printf("    #%zu  raw_value=%.4f  flat_index=%zu  (raw NPU output -- not a "
		       "decoded class/box, see README.md)\n",
		       i,
		       (double)top_val[i],
		       top_idx[i]);
	}
}

/* ---- entry point --------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
		        "usage: %s <model.tar> <frame0.bin> [frame1.bin ...]\n"
		        "  model.tar  -- drpai_dir bundle tar from "
		        "`alp_model build --target drpai`\n"
		        "  frame*.bin -- raw 640x640x3 float32 NCHW frames "
		        "(%zu bytes each)\n",
		        argv[0],
		        (size_t)FRAME_BYTES);
		return 1;
	}

	const char *model_path = argv[1];
	int         num_frames = argc - 2;

	printf("[drpai] v2n-drpai-inference: %d frame(s) against '%s'\n", num_frames, model_path);

	/* ---- stage 1: load the compiled model bundle and open it ---- */

	size_t model_len = 0;
	void  *model_buf = read_file(model_path, &model_len);
	if (model_buf == NULL) {
		return 1;
	}

	/* format and backend select the DRP-AI3 bundle format and dispatcher;
	 * both are named enumerators from <alp/inference.h> -- read the
	 * header for the full set and their numeric values, don't assume
	 * these two, since new backends/formats can be added between them.
	 * arena_bytes/arena stay at the built-in default -- unlike Ethos-U,
	 * the DRP-AI backend has no caller-supplied arena; it asks the
	 * kernel driver for its reserved working-memory base itself (see
	 * "DRP-AI working-memory arena" in src/yocto/inference_drpai.cpp). */
	alp_inference_config_t cfg = {
		.model_data  = model_buf,
		.model_size  = model_len,
		.format      = ALP_INFERENCE_MODEL_DRPAI,
		.backend     = ALP_INFERENCE_BACKEND_DRPAI,
		.arena_bytes = 0u,
		.arena       = NULL,
	};

	alp_inference_t *inf = alp_inference_open(&cfg);
	if (inf == NULL) {
		/* Documented NOSUPPORT contract, same shape as the
		 * v2n-m1-deepx-inference sibling: a build without
		 * ALP_SDK_USE_DRPAI_V2N=ON, or a board whose &drpai0 DT node
		 * was never enabled, lands here instead of crashing.
		 * ALP_ERR_NOSUPPORT -- backend not compiled in; ALP_ERR_IO --
		 * commonly /dev/drpai0 absent; ALP_ERR_TIMEOUT / ALP_ERR_BUSY
		 * -- driver present but contended.  Full triage table:
		 * docs/bring-up-drpai-v2n.md Sec 7. */
		alp_status_t err = alp_last_error();
		printf("[drpai]   open returned NULL: last_err=%d (%s)\n", (int)err, alp_status_name(err));
		printf("[drpai]   (expected without a DRP-AI-enabled alp-sdk build, or on a "
		       "board\n"
		       "[drpai]    without a &drpai0 DT override -- see "
		       "docs/bring-up-drpai-v2n.md)\n");
		free(model_buf);
		return 1;
	}

	printf("[drpai] model open: %zu input(s), %zu output(s)\n",
	       alp_inference_num_inputs(inf),
	       alp_inference_num_outputs(inf));

	/* ---- stage 2: run every frame ---- */

	int exit_code = 0;

	for (int i = 0; i < num_frames; ++i) {
		const char *frame_path = argv[2 + i];

		size_t frame_len = 0;
		void  *frame_buf = read_file(frame_path, &frame_len);
		if (frame_buf == NULL) {
			exit_code = 1;
			continue;
		}
		if (frame_len != FRAME_BYTES) {
			fprintf(stderr,
			        "error: '%s' is %zu bytes, expected %zu (640x640x3 float32 "
			        "NCHW) -- skipped\n",
			        frame_path,
			        frame_len,
			        (size_t)FRAME_BYTES);
			free(frame_buf);
			exit_code = 1;
			continue;
		}

		alp_inference_tensor_t in = { 0 };
		alp_status_t           rc = alp_inference_get_input(inf, 0u, &in);
		if (rc != ALP_OK) {
			fprintf(
			    stderr, "error: get_input for '%s' failed: %s\n", frame_path, alp_status_name(rc));
			free(frame_buf);
			exit_code = 1;
			continue;
		}

		/* The backend's own input-tensor size is authoritative. Every
		 * frame is already hard-rejected above unless it is exactly
		 * FRAME_BYTES, so a mismatch here means the loaded model is
		 * not the 640x640x3 float32 NCHW one this example targets --
		 * skip the frame outright rather than copy a partial tensor
		 * and print results as if they were valid (a short copy would
		 * leave the tail of the tensor holding the previous frame's
		 * bytes, so the "output" would partly reflect frame N-1).
		 * This check is byte-count only, same as the guard above: the
		 * MERA wrapper reports no per-input shape (rank stays 0 --
		 * see "Input" above), so an interleaved HWC frame of the same
		 * size passes both checks and gets copied with the wrong
		 * channel ordering. Layout is the caller's contract, not
		 * something this program can verify. */
		if (in.size_bytes != frame_len) {
			fprintf(stderr,
			        "error: '%s' is %zu bytes but the model's input tensor is "
			        "%zu bytes -- skipped\n",
			        frame_path,
			        frame_len,
			        in.size_bytes);
			free(frame_buf);
			exit_code = 1;
			continue;
		}
		if (in.data == NULL) {
			fprintf(stderr, "error: get_input for '%s' returned a NULL buffer\n", frame_path);
			free(frame_buf);
			exit_code = 1;
			continue;
		}
		memcpy(in.data, frame_buf, frame_len);
		free(frame_buf);

		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		rc = alp_inference_invoke(inf);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double invoke_ms =
		    (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1.0e6;

		if (rc != ALP_OK) {
			fprintf(stderr, "error: invoke on '%s' failed: %s\n", frame_path, alp_status_name(rc));
			exit_code = 1;
			continue;
		}

		alp_inference_tensor_t out = { 0 };
		rc                         = alp_inference_get_output(inf, 0u, &out);
		if (rc != ALP_OK) {
			fprintf(
			    stderr, "error: get_output for '%s' failed: %s\n", frame_path, alp_status_name(rc));
			exit_code = 1;
			continue;
		}

		printf("[drpai] %s: %.2f ms, output=%zu bytes, dtype=%d\n",
		       frame_path,
		       invoke_ms,
		       out.size_bytes,
		       (int)out.dtype);

		/* alp_inference_tensor_t out = {0} makes out.dtype default to
		 * ALP_INFERENCE_DTYPE_F32 (enum value 0), so a backend that
		 * returns ALP_OK without filling dtype would otherwise be
		 * indistinguishable from a genuine float32 tensor and get
		 * walked as `const float *`. Require size_bytes to also be a
		 * sane multiple of sizeof(float) before trusting it as
		 * float32 -- report rather than guess when it isn't. */
		if (out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
		    out.size_bytes % sizeof(float) == 0 && out.size_bytes != 0) {
			print_top_scores((const float *)out.data, out.size_bytes / sizeof(float));
		} else if (out.dtype == ALP_INFERENCE_DTYPE_F32) {
			printf("    (output dtype=F32 but size_bytes=%zu is not a sane float32 "
			       "buffer -- top-score print skipped)\n",
			       out.size_bytes);
		} else {
			printf("    (output dtype=%d is not float32 -- top-score print "
			       "skipped)\n",
			       (int)out.dtype);
		}
	}

	alp_inference_close(inf);
	free(model_buf);

	printf("[drpai] done\n");
	return exit_code;
}
