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
 *   get_output/invoke/close).  The handle layout below MUST match
 *   inference_yocto.c's `struct alp_inference` exactly.
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
}

/* Mirror of the yocto dispatcher's struct alp_inference layout so we can
 * read/write be_state without exposing the type.  MUST match
 * inference_yocto.c exactly -- keep in sync if the dispatcher's fields
 * change. */
struct alp_inference_handle_layout {
	bool                    in_use;
	alp_inference_backend_t backend;
	void                   *be_state;
};

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

/** Fill an alp tensor descriptor from a dx_rt Tensor.  `data` points at
 *  the engine/SDK-owned buffer; the app must not free it. */
void fill_tensor_descriptor(dxrt::Tensor &t, void *data, alp_inference_tensor_t *out)
{
	out->data       = data;
	out->size_bytes = static_cast<size_t>(t.size_in_bytes());
	out->dtype      = dxrt_dtype_to_alp(t.type());

	const std::vector<int64_t> &shape = t.shape();
	const size_t                n     = shape.size();
	out->rank                         = static_cast<uint8_t>((n <= 4) ? n : 4);
	for (uint8_t i = 0; i < out->rank; ++i) {
		out->shape[i] = static_cast<uint16_t>(shape[i]);
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
	auto *h = reinterpret_cast<alp_inference_handle_layout *>(h_);

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

		/* Stage one SDK-owned buffer per input tensor.  The app writes
         * into these via get_input(); invoke() hands inputs[0].data() to
         * Run().  (dx_rt concatenates multi-input models; the common
         * V2N-M1 vision model is single-input.) */
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
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
	auto *st = static_cast<DeepxState *>(h->be_state);
	return (st != nullptr) ? st->inputs.size() : 0u;
}

extern "C" std::size_t alp_inference_deepx_num_outputs(struct alp_inference *h_)
{
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
	auto *st = static_cast<DeepxState *>(h->be_state);
	return (st != nullptr) ? st->outputs.size() : 0u;
}

extern "C" alp_status_t alp_inference_deepx_get_input(struct alp_inference   *h_,
                                                      std::size_t             index,
                                                      alp_inference_tensor_t *out)
{
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
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
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr) {
		return ALP_ERR_NOT_READY;
	}
	if (index >= st->outputs.size()) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* After the first invoke(), last_outputs[index] points at the live
     * engine-owned result buffer; before any invoke the descriptor's
     * data() is the engine's zero-initialised output area. */
	void *data = nullptr;
	if (index < st->last_outputs.size() && st->last_outputs[index] != nullptr) {
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
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
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
	auto *h  = reinterpret_cast<alp_inference_handle_layout *>(h_);
	auto *st = static_cast<DeepxState *>(h->be_state);
	if (st == nullptr) {
		return;
	}
	delete st->engine;
	delete st;
	h->be_state = nullptr;
}
