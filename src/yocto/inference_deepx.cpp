/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * [vendor-ext] DEEPX DX-M1 backend hook for <alp/inference.h>.
 *
 * BENCH-UNVERIFIED: compiles + header-checks against the real DEEPX
 * dx_rt headers, but has NOT been run on silicon.  Validation needs an
 * E1M-X V2N-M1 module with the DX-M1 enumerated on PCIe plus the
 * proprietary dx_rt runtime + kernel driver on the Yocto sysroot.
 * Same posture as the recent mbox_alif_mhuv2 / alif_dave2d work.
 *
 * ----------------------------------------------------------------------
 * Real vendor API
 *   This file is written against DEEPX's *real* dx_rt C++ runtime
 *   (`#include "dxrt/dxrt_api.h"`, namespace `dxrt`), NOT the fictional
 *   C `dxnn_*` surface the v0.3 scaffold used.  The dx_rt umbrella
 *   header pulls in `dxrt::InferenceEngine`, `dxrt::Tensor`,
 *   `dxrt::InferenceOption` and the `dxrt::DataType` enum.  Surface used
 *   here (all present in dx_rt/lib/include/dxrt/):
 *     - InferenceEngine(const uint8_t* buf, size_t size,
 *                       InferenceOption& opt)            inference_engine.h
 *     - Tensors GetInputs()  / Tensors GetOutputs()      inference_engine.h
 *     - TensorPtrs Run(void* inputPtr, ...)              inference_engine.h
 *     - Tensor::data() / size_in_bytes() / type() / shape()   tensor.h
 *     - enum DataType { FLOAT, UINT8, INT8, ... }        datatype.h
 *   `Tensors` is `std::vector<Tensor>`; `Run()` blocks until the NPU
 *   returns and yields the output `TensorPtrs`.
 *
 * Vendor-artifact handling (classifying-public-vs-internal)
 *   dx_rt is PROPRIETARY (DEEPX EULA, customer-only).  Its headers + the
 *   libdxrt.so live OUTSIDE this repo (the maintainer clone at
 *   ~/npu-sdks/dx_rt; the license-gated copy belongs in alp-sdk-internal
 *   under Git LFS).  The public repo carries only THIS body, which links
 *   against the SDK located via the Yocto sysroot at build time when
 *   ALP_SDK_USE_DEEPX_DXM1=ON (default OFF).  No DEEPX source is vendored.
 *
 *   Follow-up: drop the real dx_rt headers/libs into alp-sdk-internal
 *   (Git LFS) + wire the meta-deepx-m1 dx-rt recipe into the V2N-M1
 *   MACHINE so the cross-build finds libdxrt on the sysroot.
 *
 * Blob format
 *   cfg.model_data is a `.dxnn` compiled model (magic "DXNN", 8 KiB
 *   self-describing header) produced by the host dxcom compiler
 *   (scripts/alp_model/adapters/deepx.py).  We hand the raw bytes to the
 *   in-memory InferenceEngine ctor; dx_rt parses the header and primes
 *   the device command-stream decoder.
 *
 * Dispatcher contract
 *   Mirrors the 7-symbol hook shape the Yocto dispatcher in
 *   inference_yocto.c calls (open/num_inputs/num_outputs/get_input/
 *   get_output/invoke/close).  The handle layout (struct alp_inference)
 *   is the shared definition in inference_handle_internal.h (issue #1257).
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

/* Pull the specific dx_rt headers this backend uses rather than the
 * `dxrt/dxrt_api.h` umbrella: the umbrella also drags in dxrt/cli.h (and
 * its third-party cxxopts dependency), which the inference path does not
 * need.  These four are the real DEEPX headers and declare every dxrt
 * symbol referenced below. */
#include "dxrt/datatype.h"
#include "dxrt/tensor.h"
#include "dxrt/inference_option.h"
#include "dxrt/inference_engine.h"

extern "C" {
#include "alp/inference.h"

#include "inference_handle_internal.h"
}

/* The dispatcher's `struct alp_inference` comes from the shared internal
 * header (issue #1257).  This file used to hand-mirror the layout and cast
 * to the mirror; the mirror had a DIFFERENT field order and only worked
 * because pointers are 8 bytes.  One definition, compiler-enforced. */

namespace
{

/** Per-handle DEEPX state.  Owns the dx_rt InferenceEngine + a snapshot
 *  of the input/output tensor descriptors taken at open() time.  The
 *  input data buffers are owned by the SDK here (filled by the app via
 *  get_input, handed to Run() at invoke); the output buffers are owned
 *  by the InferenceEngine and refreshed each Run(). */
struct DeepxState {
	dxrt::InferenceEngine *engine = nullptr;

	/* SDK-owned input staging buffers (one contiguous blob per input
     * tensor).  dx_rt's Run(inputPtr) takes a single pointer to the
     * concatenated inputs; for the common single-input model this is
     * just inputs[0]. */
	std::vector<std::vector<uint8_t>> input_bufs;

	/* Descriptor snapshots so get_input/get_output don't re-query the
     * engine on every call. */
	dxrt::Tensors inputs;
	dxrt::Tensors outputs;

	/* Output tensor pointers from the most recent Run(); the data()
     * pointers in `outputs` reference engine-owned device-mapped memory
     * after the first invoke. */
	dxrt::TensorPtrs last_outputs;
};

/** Map a dx_rt DataType onto the alp_inference dtype enum.  dx_rt's enum
 *  (datatype.h) carries device-only structured types (BBOX/FACE/POSE)
 *  the portable surface has no slot for; those fall back to UINT8 so the
 *  raw bytes are still reachable via the tensor's data()/size. */
alp_inference_dtype_t dxrt_dtype_to_alp(dxrt::DataType t)
{
	switch (t) {
	case dxrt::FLOAT:
		return ALP_INFERENCE_DTYPE_F32;
	case dxrt::UINT8:
		return ALP_INFERENCE_DTYPE_UINT8;
	case dxrt::INT8:
		return ALP_INFERENCE_DTYPE_INT8;
	case dxrt::UINT16:
	case dxrt::INT16:
		return ALP_INFERENCE_DTYPE_INT16;
	case dxrt::INT32:
	case dxrt::UINT32:
		return ALP_INFERENCE_DTYPE_INT32;
	default:
		/* INT64/UINT64/BBOX/FACE/POSE/NONE have no portable slot; expose
         * the raw bytes as uint8 so the caller can still reach them. */
		return ALP_INFERENCE_DTYPE_UINT8;
	}
}

/** True when every tensor in @p tensors has rank <= 4 -- the maximum
 *  alp_inference_tensor_t's fixed shape[4] descriptor can hold without
 *  truncating.  fill_tensor_descriptor() below used to silently truncate a
 *  longer shape to the first 4 dims instead of saying so; the caller read
 *  back a shape that no longer matched the model, with no signal anything
 *  was wrong (issue #1729).  Called at open() time, before any tensor
 *  descriptor is handed to a caller -- validates st->inputs/st->outputs,
 *  the DECLARED metadata dx_rt reports before any Run().  It does NOT
 *  cover the LIVE last_outputs a Run() actually hands back; get_output()
 *  re-checks that path itself right before calling fill_tensor_descriptor()
 *  on it. */
bool all_tensor_ranks_fit(dxrt::Tensors &tensors)
{
	for (auto &t : tensors) {
		if (t.shape().size() > 4) {
			return false;
		}
	}
	return true;
}

/** True if every dim of every tensor in @p tensors fits the descriptor's
 *  uint16_t shape[4] slots -- called from open(), alongside
 *  all_tensor_ranks_fit() above, so a model with an unrepresentable dim
 *  VALUE fails to load instead of lying about its shape on the first
 *  get_input()/get_output().  A separate concern from the rank check
 *  above: rank bounds how many dims fit, this bounds how big each one
 *  can be.  Mirrors inference_ort.cpp's _gather_tensor_info() gate: a
 *  dim <= 0 (symbolic/dynamic) is never rejected -- fill_tensor_descriptor()
 *  pins it to 1, same as ORT -- only a dim too large to fit is (#1645). */
bool shapes_fit_descriptor(const dxrt::Tensors &tensors)
{
	for (const dxrt::Tensor &t : tensors) {
		for (int64_t d : t.shape()) {
			if (d > UINT16_MAX) {
				return false;
			}
		}
	}
	return true;
}

/** Fill an alp tensor descriptor from a dx_rt Tensor.  `data` points at
 *  the engine/SDK-owned buffer; the app must not free it.
 *
 *  PRECONDITION: @p t's rank is <= 4 AND every dim value fits a
 *  uint16_t.  For st->inputs/st->outputs, open() refuses
 *  (ALP_ERR_NOSUPPORT) any model carrying a tensor that doesn't hold,
 *  via all_tensor_ranks_fit() / shapes_fit_descriptor() above; for a
 *  live st->last_outputs[i] tensor (get_output() after invoke()), the
 *  caller re-checks the rank itself immediately before this call, for
 *  the same reason -- either way this never truncates a live rank > 4
 *  or dim value > UINT16_MAX (issues #1729, #1645). Only the
 *  symbolic-dim pin (dim <= 0 -> 1) happens here, matching
 *  inference_ort.cpp's _gather_tensor_info(). */
void fill_tensor_descriptor(dxrt::Tensor &t, void *data, alp_inference_tensor_t *out)
{
	out->data       = data;
	out->size_bytes = static_cast<size_t>(t.size_in_bytes());
	out->dtype      = dxrt_dtype_to_alp(t.type());

	const std::vector<int64_t> &shape = t.shape();
	const size_t                n     = shape.size();
	out->rank                         = static_cast<uint8_t>((n <= 4) ? n : 4);
	for (uint8_t i = 0; i < out->rank; ++i) {
		int64_t d = shape[i];
		if (d <= 0) {
			d = 1; /* symbolic/dynamic dim -- pin to single-sample. */
		}
		out->shape[i] = static_cast<uint16_t>(d);
	}

	/* dx_rt models carry per-task quant params internally and emit
     * already-dequantized FLOAT outputs for the common case; the public
     * Tensor surface exposes no scale/zero_point accessor, so report the
     * identity transform.  Apps needing raw-quant access use the
     * <alp/ext/deepx/inference.h> escape hatch. */
	out->scale      = 1.0f;
	out->zero_point = 0;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Backend hooks (C ABI, matching inference_yocto.c's declarations).   */
/* ------------------------------------------------------------------ */

extern "C" alp_status_t alp_inference_deepx_open(struct alp_inference         *h_,
                                                 const alp_inference_config_t *cfg)
{
	struct alp_inference *h = h_;

	auto *st = new (std::nothrow) DeepxState();
	if (st == nullptr) {
		return ALP_ERR_NOMEM;
	}

	/* dx_rt reports load/device errors by throwing; a failed PCIe
     * enumeration or a bad .dxnn header surfaces as an exception, which
     * we translate to ALP_ERR_IO so the portable surface stays
     * exception-free for C callers. */
	try {
		st->engine =
		    new (std::nothrow) dxrt::InferenceEngine(static_cast<const uint8_t *>(cfg->model_data),
		                                             cfg->model_size,
		                                             dxrt::DefaultInferenceOption);
		if (st->engine == nullptr) {
			delete st;
			return ALP_ERR_NOMEM;
		}

		st->inputs  = st->engine->GetInputs();
		st->outputs = st->engine->GetOutputs();

		if (!all_tensor_ranks_fit(st->inputs) || !all_tensor_ranks_fit(st->outputs)) {
			/* alp_inference_tensor_t's shape[] has exactly 4 slots; refuse
			 * the model rather than let get_input()/get_output() hand back
			 * a shape silently truncated to the first 4 dims (issue
			 * #1729). NOSUPPORT, not IO: the model loaded fine, it is this
			 * portable descriptor that has no slot for its rank. */
			delete st->engine;
			delete st;
			return ALP_ERR_NOSUPPORT;
		}

		if (!shapes_fit_descriptor(st->inputs) || !shapes_fit_descriptor(st->outputs)) {
			/* A dim this large would silently truncate in get_input()'s /
             * get_output()'s uint16_t shape[] descriptor -- reject the
             * model here rather than lying about its shape on the first
             * call (#1645, mirrors inference_ort.cpp's
             * _gather_tensor_info() gate). */
			delete st->engine;
			delete st;
			return ALP_ERR_NOSUPPORT;
		}

		if (st->inputs.size() > 1) {
			/* invoke() hands dx_rt's Run() a SINGLE pointer --
			 * st->input_bufs[0].data() -- which dx_rt treats as the base
			 * of one contiguous blob concatenating every input tensor
			 * (see the DeepxState::input_bufs doc above). This backend
			 * stages each input in its OWN separate std::vector
			 * allocation instead, so for any model with more than one
			 * input dx_rt would read past input_bufs[0]'s real size and
			 * DMA whatever unrelated heap memory follows it over PCIe to
			 * the DX-M1 (issue #1645). Refuse rather than mis-run until a
			 * real concatenating staging buffer lands -- this path is
			 * gated behind ALP_SDK_USE_DEEPX_DXM1 (default OFF) and
			 * bench-unverified either way, so getting it wrong here would
			 * be read as a hardware/model problem on first DEEPX
			 * bring-up. */
			delete st->engine;
			delete st;
			return ALP_ERR_NOSUPPORT;
		}

		/* Stage one SDK-owned buffer per input tensor.  The app writes
         * into these via get_input(); invoke() hands inputs[0].data() to
         * Run().  (dx_rt concatenates multi-input models; the common
         * V2N-M1 vision model is single-input -- multi-input is refused
         * above until a real concatenating staging buffer lands.) */
		st->input_bufs.resize(st->inputs.size());
		for (size_t i = 0; i < st->inputs.size(); ++i) {
			st->input_bufs[i].resize(static_cast<size_t>(st->inputs[i].size_in_bytes()));
		}
	} catch (...) {
		delete st->engine;
		delete st;
		return ALP_ERR_IO;
	}

	h->be_state = st;
	return ALP_OK;
}

extern "C" std::size_t alp_inference_deepx_num_inputs(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	return (st != nullptr) ? st->inputs.size() : 0u;
}

extern "C" std::size_t alp_inference_deepx_num_outputs(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	return (st != nullptr) ? st->outputs.size() : 0u;
}

extern "C" alp_status_t alp_inference_deepx_get_input(struct alp_inference   *h_,
                                                      std::size_t             index,
                                                      alp_inference_tensor_t *out)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}
	if (index >= st->inputs.size()) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	/* Hand back the SDK-owned staging buffer, not the engine's internal
     * pointer -- the app fills this before invoke(). */
	fill_tensor_descriptor(st->inputs[index], st->input_bufs[index].data(), out);
	return ALP_OK;
}

extern "C" alp_status_t alp_inference_deepx_get_output(struct alp_inference   *h_,
                                                       std::size_t             index,
                                                       alp_inference_tensor_t *out)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}
	if (index >= st->outputs.size()) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* After the first invoke(), last_outputs[index] points at the live
     * engine-owned result buffer; before any invoke the descriptor's
     * data() is the engine's zero-initialised output area.
     *
     * all_tensor_ranks_fit() at open() time only validated st->outputs --
     * the declared metadata dx_rt reported before any Run().  It says
     * nothing about what a live Run() actually hands back in
     * last_outputs; fill_tensor_descriptor()'s PRECONDITION (rank <= 4)
     * does not hold for that path on its own, so re-check the live
     * tensor's rank here too before trusting it (issue #1729). */
	void *data = nullptr;
	if (index < st->last_outputs.size() && st->last_outputs[index] != nullptr) {
		if (st->last_outputs[index]->shape().size() > 4) {
			return ALP_ERR_NOSUPPORT;
		}
		data = st->last_outputs[index]->data();
		fill_tensor_descriptor(*st->last_outputs[index], data, out);
	} else {
		data = st->outputs[index].data();
		fill_tensor_descriptor(st->outputs[index], data, out);
	}
	return ALP_OK;
}

extern "C" alp_status_t alp_inference_deepx_invoke(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr || st->engine == nullptr) {
		return ALP_ERR_NOT_READY;
	}

	void *input_ptr = st->input_bufs.empty() ? nullptr : st->input_bufs[0].data();

	try {
		/* Synchronous run -- blocks until the NPU returns.  The returned
         * TensorPtrs reference engine-owned output memory; we stash them
         * so get_output() reflects this pass. */
		st->last_outputs = st->engine->Run(input_ptr);
	} catch (...) {
		return ALP_ERR_IO;
	}

	return st->last_outputs.empty() ? ALP_ERR_IO : ALP_OK;
}

extern "C" void alp_inference_deepx_close(struct alp_inference *h_)
{
	auto *h  = h_;
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr) {
		return;
	}
	delete st->engine;
	delete st;
	h->be_state = nullptr;
}
