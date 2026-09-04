/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ONNX Runtime CPU backend hook for <alp/inference.h>, A55 / Linux / Yocto
 * side.
 *
 * Fills the ALP_INFERENCE_BACKEND_CPU slot whose header comment in
 * inference_yocto.c has read "Wiring deferred to v0.4" since v0.4.  It is
 * lowest priority in that file's resolve_auto() (task #14 sibling wiring),
 * so an NPU-bearing SoM still selects its Ethos-U / DRP-AI / DEEPX NPU
 * backend under AUTO -- this is the portable CPU floor, not a replacement
 * for the NPU paths.  Same posture as the DRP-AI (inference_drpai.cpp) and
 * DEEPX (inference_deepx.cpp) hooks: compiled only when its CMake option
 * (ALP_SDK_USE_ORT_CPU, default OFF) is on, and BENCH-UNVERIFIED -- ONNX
 * Runtime is not installed on this dev host, so this file has been
 * header-checked (see the CMake probe block's comment) but has not run on
 * silicon.
 *
 * ----------------------------------------------------------------------
 * Real vendor API
 *   Written against the *real* ONNX Runtime C API (onnxruntime_c_api.h,
 *   `microsoft/onnxruntime`, own-recipe pin v1.28.0 -- see
 *   metadata/libraries/onnxruntime.yaml).  This is a plain C API (a struct
 *   of function pointers reached via OrtGetApiBase()->GetApi()), so this
 *   file calls it directly rather than pulling in the header-only C++
 *   wrapper (onnxruntime_cxx_api.h).  That does NOT make this TU
 *   exception-free, though: <string>/<vector> are used throughout for the
 *   SDK-owned staging buffers and tensor metadata, and every one of
 *   std::vector::resize/assign, std::make_unique and std::string
 *   assignment can throw std::bad_alloc / std::length_error.  Every
 *   extern "C" hook below is wrapped in a try/catch so nothing throws
 *   across the C ABI boundary into inference_yocto.c (see the "Exception
 *   boundary" note further down). Surface used here:
 *     - OrtGetApiBase()->GetApi(ORT_API_VERSION)  resolve the ABI-pinned vtable
 *     - OrtApi::CreateEnv / ReleaseEnv
 *     - OrtApi::CreateSessionOptions / ReleaseSessionOptions
 *     - OrtApi::CreateSessionFromArray / ReleaseSession   (loads from a byte
 *       buffer -- cfg->model_data is the raw `.onnx` protobuf bytes, no
 *       separate container format the way DRP-AI's `drpai_dir` tar is)
 *     - OrtApi::SessionGetInputCount / SessionGetOutputCount
 *     - OrtApi::SessionGetInputTypeInfo / SessionGetOutputTypeInfo
 *     - OrtApi::CastTypeInfoToTensorInfo / ReleaseTypeInfo
 *     - OrtApi::GetTensorElementType / GetDimensionsCount / GetDimensions
 *     - OrtApi::SessionGetInputName / SessionGetOutputName (allocator-owned;
 *       copied into this file's own std::string, then freed)
 *     - OrtApi::GetAllocatorWithDefaultOptions
 *     - OrtApi::CreateCpuMemoryInfo / ReleaseMemoryInfo
 *     - OrtApi::CreateTensorWithDataAsOrtValue / ReleaseValue  (wraps this
 *       file's own SDK-owned buffers -- ORT reads/writes them directly, no
 *       extra copy either direction)
 *     - OrtApi::Run
 *     - OrtApi::GetErrorCode / GetErrorMessage / ReleaseStatus  (both are
 *       actually called, in _ort_status_to_alp() below: GetErrorMessage's
 *       text -- the failing op + the mismatched shape -- is written to
 *       stderr on every non-OK status, because _ort_errorcode_to_status()'s
 *       coarse enum can't carry it and this TU has no other logging
 *       facility)
 *
 * Blob format
 *   cfg->model_data / cfg->model_size carry the raw `.onnx` protobuf bytes
 *   by value, exactly the way the DEEPX path passes its raw .dxnn buffer --
 *   CreateSessionFromArray takes them as-is, so the generic loader
 *   (src/common/alp_model_loader.c) stays format-agnostic.  See the
 *   VALID_BLOB_FORMATS "onnx" entry in scripts/alp_model/manifest.py.
 *
 * Tensor shape / dynamic dims
 *   ORT reports a symbolic/dynamic dimension (e.g. a batch axis) as <= 0.
 *   The portable get_input()/get_output() contract hands back one
 *   fixed-size SDK-owned buffer per tensor with no batching story, so a
 *   dynamic dim is pinned to 1 (single-sample) at open() time rather than
 *   failing outright -- see _gather_tensor_info() below.
 *
 * Trust boundary: model_data is untrusted
 *   cfg->model_data is a customer-supplied `.onnx` blob -- nothing about
 *   its contents, including every tensor's rank and per-axis extents, is
 *   validated before it reaches this file.  ORT itself does not check the
 *   declared shape against how much real tensor data follows, so
 *   _gather_tensor_info() is the trust boundary for that data: it bounds
 *   ndim -- against TWO SEPARATE ceilings, not one (kHostileTensorRankCeiling
 *   vs. kMaxTensorRank below): a corrupt/hostile ndim (ALP_ERR_INVAL) is a
 *   different condition from a well-formed model whose rank this 4-slot
 *   descriptor simply cannot represent (ALP_ERR_NOSUPPORT) -- checks every
 *   accumulation for overflow, and rejects a shape whose element count
 *   would overflow size_t or blow a policy cap on total tensor bytes,
 *   rather than handing back a wrapped/undersized size_bytes alongside a
 *   shape[]/rank that still claims the original (huge) dimensions.  It also
 *   refuses (ALP_ERR_NOSUPPORT) any tensor whose real rank exceeds 4:
 *   alp_inference_tensor_t's shape[] has exactly 4 slots, and this backend
 *   used to silently truncate a longer shape to the first 4 dims in
 *   get_input()/get_output() instead of saying so --
 *   the caller read back a shape that no longer matched the model, with no
 *   signal anything was wrong (issue #1729).
 *
 * OrtErrorCode -> alp_status_t mapping
 *   No ORT precedent exists in this tree to reuse: inference_drpai.cpp's
 *   only table, _drpai_errno_to_status() (src/yocto/inference_drpai.cpp:154),
 *   maps POSIX driver errno from /dev/drpai0 ioctls, which shares no
 *   vocabulary with ORT's OrtErrorCode enum (onnxruntime_c_api.h).  The
 *   table below (_ort_errorcode_to_status()) is therefore authored fresh
 *   for this file -- see its own doc comment for the full mapping and the
 *   two OrtErrorCode names (ORT_NOT_FOUND, ORT_MODEL_REQUIRES_COMPILATION)
 *   that do NOT exist in the OrtErrorCode this repo's pinned API version
 *   actually declares.
 *
 * Dispatcher contract
 *   Mirrors the 7-symbol hook shape the Yocto dispatcher in
 *   inference_yocto.c calls (same shape as inference_deepx.cpp /
 *   inference_drpai.cpp).  The handle layout (struct alp_inference) is
 *   the shared definition in inference_handle_internal.h -- see that
 *   header's static_assert for the one invariant this file actually
 *   depends on (issue #1257).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "onnxruntime_c_api.h"

extern "C" {
#include "alp/inference.h"
}

#include "inference_handle_internal.h"

namespace
{

/** Resolve the ABI-pinned ORT function-pointer table once.  NULL means the
 *  linked libonnxruntime.so is older than ORT_API_VERSION (an ABI-version
 *  mismatch, not a bad model) -- callers surface that as ALP_ERR_VERSION. */
const OrtApi *_ort_api()
{
	static const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
	return api;
}

/** Map an OrtErrorCode onto the portable status enum.  Authored fresh --
 *  see the file header's "OrtErrorCode -> alp_status_t mapping" note; there
 *  is no in-tree ORT precedent to reuse.
 *
 *    - ORT_INVALID_ARGUMENT / ORT_NO_MODEL / ORT_INVALID_PROTOBUF /
 *      ORT_INVALID_GRAPH  bad input to open() (empty/corrupt/malformed
 *                         model bytes, or a bad call argument) -> INVAL.
 *    - ORT_NO_SUCHFILE  has an exact in-tree counterpart, ALP_ERR_NOT_FOUND
 *                       (used at src/backends/inference/alp_model_select.c:160)
 *                       -> NOT_FOUND.  (ORT_NOT_FOUND, which an earlier
 *                       review pass also named for this arm, does not
 *                       exist in this repo's pinned OrtErrorCode -- see
 *                       onnxruntime_c_api.h's `enum OrtErrorCode`.)
 *    - ORT_NOT_IMPLEMENTED / ORT_EP_FAIL  the loaded model needs an op or
 *                       an execution-provider capability this ORT build
 *                       doesn't carry -> NOSUPPORT.
 *                       (ORT_MODEL_REQUIRES_COMPILATION, also named by that
 *                       review pass, likewise does not exist in this
 *                       OrtErrorCode -- same note.)
 *    - ORT_RUNTIME_EXCEPTION  the EXPECTED failure for any model with a
 *                       data-dependent output shape (see this file's
 *                       "output buffers ... reused every invoke()" note) --
 *                       a real shape/argument mismatch at Run() time, not a
 *                       device error, so it gets its own arm (INVAL, already
 *                       part of invoke()'s documented return set) instead of
 *                       collapsing into the IO catch-all below, where it
 *                       would be indistinguishable from a device read error.
 *    - ORT_FAIL / ORT_ENGINE_ERROR / ORT_MODEL_LOADED / everything else
 *                       -> IO, the catch-all for "the runtime tried and
 *                       failed" the same way _drpai_errno_to_status's
 *                       default arm does for its driver.
 */
alp_status_t _ort_errorcode_to_status(OrtErrorCode code)
{
	switch (code) {
	case ORT_OK:
		return ALP_OK;
	case ORT_INVALID_ARGUMENT:
	case ORT_NO_MODEL:
	case ORT_INVALID_PROTOBUF:
	case ORT_INVALID_GRAPH:
		return ALP_ERR_INVAL;
	case ORT_NO_SUCHFILE:
		return ALP_ERR_NOT_FOUND;
	case ORT_NOT_IMPLEMENTED:
	case ORT_EP_FAIL:
		return ALP_ERR_NOSUPPORT;
	case ORT_RUNTIME_EXCEPTION:
		return ALP_ERR_INVAL;
	default:
		return ALP_ERR_IO;
	}
}

/** Consume an OrtStatus* the way every OrtApi call returns one: NULL means
 *  success, non-NULL carries an OrtErrorCode + message and MUST be freed
 *  with ReleaseStatus exactly once.  Maps + releases in one place so no
 *  call site can forget either half.  Also surfaces ORT's own message on
 *  stderr for every non-OK status -- see the file header's "Surface used
 *  here" note on GetErrorMessage. */
alp_status_t _ort_status_to_alp(OrtStatus *status)
{
	if (status == nullptr) {
		return ALP_OK;
	}
	const OrtApi      *api  = _ort_api();
	const OrtErrorCode code = api->GetErrorCode(status);
	std::fprintf(stderr, "[alp_sdk][inference_ort] %s\n", api->GetErrorMessage(status));
	const alp_status_t rc = _ort_errorcode_to_status(code);
	api->ReleaseStatus(status);
	return rc;
}

/** Map an ONNX tensor element type onto the portable dtype enum.  ORT
 *  carries more element types than the portable enum has slots for;
 *  INT64 has no 64-bit slot (same fallback DRP-AI's mera_dtype_to_alp
 *  uses) and BOOL / DOUBLE / UINT32 / UINT64 / BFLOAT16 / the FP8 variants
 *  / COMPLEX64 / COMPLEX128 have no portable counterpart at all -- all
 *  fall back to UINT8 (the raw bytes stay reachable via size_bytes, which
 *  is always sized from the REAL ORT element width, not this fallback). */
alp_inference_dtype_t _ort_dtype_to_alp(ONNXTensorElementDataType t)
{
	switch (t) {
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
		return ALP_INFERENCE_DTYPE_F32;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
		return ALP_INFERENCE_DTYPE_F16;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
		return ALP_INFERENCE_DTYPE_INT8;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
		return ALP_INFERENCE_DTYPE_UINT8;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
		return ALP_INFERENCE_DTYPE_INT16;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
		return ALP_INFERENCE_DTYPE_INT32;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
		/* No 64-bit slot in the portable enum; report as int32 (callers
         * needing true int64 read raw bytes via size_bytes). */
		return ALP_INFERENCE_DTYPE_INT32;
	default:
		return ALP_INFERENCE_DTYPE_UINT8;
	}
}

/** Byte width of one element of ORT type @p t, or 0 for a type this
 *  fixed-width-buffer contract can't size (notably STRING, whose elements
 *  are variable-length, and UNDEFINED).  This is the REAL ORT element
 *  size used to size SDK-owned buffers -- independent of (and often wider
 *  than) whatever _ort_dtype_to_alp() falls back to for the descriptor's
 *  `dtype` field. */
size_t _ort_dtype_size_bytes(ONNXTensorElementDataType t)
{
	switch (t) {
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
		return 1;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16:
		return 2;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32:
		return 4;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX64:
		return 8;
	case ONNX_TENSOR_ELEMENT_DATA_TYPE_COMPLEX128:
		return 16;
	default:
		/* STRING / UNDEFINED -- not representable as a fixed-width SDK
         * buffer; the caller rejects the tensor rather than guess. */
		return 0;
	}
}

/** Snapshot of one input or output tensor's static metadata, taken once at
 *  open() time. */
struct OrtTensorInfo {
	std::string               name;
	ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	/* Dynamic dims (<= 0, e.g. a symbolic batch axis) are pinned to 1 --
     * see the file header's "Tensor shape / dynamic dims" note. */
	std::vector<int64_t> shape;
	size_t               elem_size  = 0; /* real ORT element width, bytes */
	size_t               size_bytes = 0; /* elem_size * product(shape); overflow-
                                          * and policy-cap-checked, see
                                          * _gather_tensor_info() */
};

/* Hostile/corrupt-input ceiling -- NOT the same concern as kMaxTensorRank
 * below.  A real ONNX model's tensors run to a handful of dims; this exists
 * only to reject a corrupt/hostile ndim before it drives an oversized
 * std::vector<int64_t> allocation a few lines down in _gather_tensor_info(),
 * not to constrain any legitimate model -- INVAL, the same "malformed input"
 * verdict _ort_errorcode_to_status() gives ORT_INVALID_GRAPH.  A rank this
 * large is implausible for ANY real graph, representable or not, so it is
 * screened out before the representable-rank question below is even asked. */
constexpr size_t kHostileTensorRankCeiling = 32;

/* Hard ceiling on a tensor's rank: the portable alp_inference_tensor_t
 * descriptor (include/alp/inference.h) carries a FIXED shape[4] / uint8_t
 * rank pair -- there is no wire format here to widen, unlike kMaxTensorBytes
 * below, which is this backend's own policy choice. A tensor whose real rank
 * exceeds 4 -- but is still under kHostileTensorRankCeiling above, i.e. a
 * well-formed model this API just has no slot for -- is refused outright at
 * open() (see the kMaxTensorRank check in _gather_tensor_info() below)
 * rather than silently reported as a truncated 4-dim shape that no longer
 * matches the model (issue #1729: a caller trusting that truncated shape
 * gets confidently wrong tensor geometry, not a truncated-but-honest one --
 * worse than a refusal). */
constexpr size_t kMaxTensorRank = 4;

/* Policy ceiling on one tensor's SDK-owned buffer.  This backend targets
 * the CPU floor on an A55/Yocto SoM, not a datacenter accelerator -- a
 * single tensor past this size is refused outright rather than attempted. */
constexpr size_t kMaxTensorBytes = static_cast<size_t>(1) << 30; /* 1 GiB */

/** Fill @p out with input (@p is_input) or output tensor @p index's static
 *  metadata: name, dtype, shape (dynamic dims pinned to 1) and the byte
 *  size of the fixed buffer this backend allocates for it.
 *
 *  TRUST BOUNDARY: @p session was loaded from cfg->model_data, a
 *  customer-supplied `.onnx` blob whose declared tensor shapes are
 *  entirely untrusted.  ORT does not itself bound a tensor's declared
 *  element count against how much data actually backs it, so every
 *  accumulation below is overflow-checked and the resulting size is
 *  capped against kMaxTensorBytes -- a wrapped or undersized size_bytes
 *  here would otherwise pair with a shape[]/rank in the public descriptor
 *  that still claims the original (huge) dimensions, and a caller sizing
 *  its own copy from that descriptor would read/write past the real
 *  buffer. */
alp_status_t _gather_tensor_info(const OrtApi  *api,
                                 OrtSession    *session,
                                 size_t         index,
                                 bool           is_input,
                                 OrtAllocator  *alloc,
                                 OrtTensorInfo &out)
{
	OrtTypeInfo *type_info = nullptr;
	alp_status_t rc =
	    _ort_status_to_alp(is_input ? api->SessionGetInputTypeInfo(session, index, &type_info)
	                                : api->SessionGetOutputTypeInfo(session, index, &type_info));
	if (rc != ALP_OK) {
		return rc;
	}

	const OrtTensorTypeAndShapeInfo *tensor_info = nullptr;
	rc = _ort_status_to_alp(api->CastTypeInfoToTensorInfo(type_info, &tensor_info));
	if (rc != ALP_OK) {
		api->ReleaseTypeInfo(type_info);
		return rc;
	}

	ONNXTensorElementDataType dtype = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
	rc = _ort_status_to_alp(api->GetTensorElementType(tensor_info, &dtype));
	if (rc != ALP_OK) {
		api->ReleaseTypeInfo(type_info);
		return rc;
	}

	size_t ndim = 0;
	rc          = _ort_status_to_alp(api->GetDimensionsCount(tensor_info, &ndim));
	if (rc != ALP_OK) {
		api->ReleaseTypeInfo(type_info);
		return rc;
	}
	if (ndim > kHostileTensorRankCeiling) {
		/* Different concern from the kMaxTensorRank check below: this ndim
		 * is implausible for ANY real model (corrupt/hostile input), and
		 * is rejected here -- before it drives an oversized
		 * std::vector<int64_t> allocation just below -- rather than
		 * refused as merely "unrepresentable". */
		api->ReleaseTypeInfo(type_info);
		return ALP_ERR_INVAL;
	}
	if (ndim > kMaxTensorRank) {
		/* Rank > 4 cannot be represented by alp_inference_tensor_t's
		 * fixed shape[4] -- refuse the model rather than let
		 * get_input()/get_output() hand back a shape[] silently
		 * truncated to the first 4 dims (issue #1729). NOSUPPORT, not
		 * INVAL: the model itself is well-formed (ndim already passed the
		 * hostile-ceiling check above), it is this portable descriptor
		 * that has no slot for its rank -- same "API can't represent it"
		 * sense ALP_ERR_NOSUPPORT already carries for
		 * ORT_NOT_IMPLEMENTED/ORT_EP_FAIL above. */
		api->ReleaseTypeInfo(type_info);
		return ALP_ERR_NOSUPPORT;
	}

	std::vector<int64_t> dims(ndim);
	if (ndim > 0) {
		rc = _ort_status_to_alp(api->GetDimensions(tensor_info, dims.data(), ndim));
		if (rc != ALP_OK) {
			api->ReleaseTypeInfo(type_info);
			return rc;
		}
	}
	api->ReleaseTypeInfo(type_info);

	const size_t elem_size = _ort_dtype_size_bytes(dtype);
	if (elem_size == 0) {
		return ALP_ERR_NOSUPPORT;
	}

	size_t elem_count = 1;
	for (auto &d : dims) {
		if (d <= 0) {
			d = 1; /* symbolic/dynamic dim -- pin to single-sample. */
		}
		if (d > UINT16_MAX) {
			/* A dim this large would silently truncate in get_input()'s /
             * get_output()'s uint16_t shape[] descriptor -- reject here
             * instead of lying to the caller (see the file's dtype-vs-
             * descriptor note). */
			return ALP_ERR_NOSUPPORT;
		}
		const size_t ud = static_cast<size_t>(d);
		if (elem_count > SIZE_MAX / ud) {
			return ALP_ERR_INVAL;
		}
		elem_count *= ud;
	}

	if (elem_count != 0 && elem_size > SIZE_MAX / elem_count) {
		return ALP_ERR_INVAL;
	}
	const size_t size_bytes = elem_size * elem_count;
	if (size_bytes > kMaxTensorBytes) {
		return ALP_ERR_INVAL;
	}

	char *raw_name = nullptr;
	rc = _ort_status_to_alp(is_input ? api->SessionGetInputName(session, index, alloc, &raw_name)
	                                 : api->SessionGetOutputName(session, index, alloc, &raw_name));
	if (rc != ALP_OK) {
		return rc;
	}
	out.name = raw_name;
	alloc->Free(alloc, raw_name);

	out.dtype      = dtype;
	out.shape      = std::move(dims);
	out.elem_size  = elem_size;
	out.size_bytes = size_bytes;
	return ALP_OK;
}

/** Release every OrtValue in @p values (NULL-safe per entry) via @p api. */
void _release_values(const OrtApi *api, std::vector<OrtValue *> &values)
{
	for (OrtValue *v : values) {
		if (v != nullptr) {
			api->ReleaseValue(v);
		}
	}
	values.clear();
}

/** Per-handle ORT state.  Owns the OrtEnv/OrtSession, the SDK-owned input
 *  and output staging buffers, and an OrtValue wrapping each one (created
 *  once at open() time and reused every invoke() -- CreateTensorWithDataAsOrtValue
 *  aliases the given memory rather than copying it, so Run() reads inputs
 *  from / writes outputs into these buffers directly, symmetric to how
 *  DRP-AI's input_bufs get pushed via SetInput()).
 *
 *  RAII (issue #1494): ~OrtState() is the ONE place that releases every
 *  vendor resource this struct owns.  open() drives an std::unique_ptr<OrtState>
 *  through several fallible steps -- each one an std::vector::resize/assign,
 *  std::make_unique, or std::string assignment that can throw -- and used to
 *  hand-roll a matching cleanup at every early return plus a SEPARATE,
 *  incomplete copy in its catch(...) handler (the catch arm released
 *  cpu_mem_info/session/env but skipped the OrtValue vectors entirely,
 *  leaking every OrtValue already created before the throw).  Now every exit
 *  path -- an early `return rc;` inside open()'s try, the catch(...) handler,
 *  and close() -- destroys the same std::unique_ptr<OrtState>/raw OrtState*
 *  and gets the same cleanup for free; open()'s success path is the only one
 *  that escapes it, via std::unique_ptr::release(). */
struct OrtState {
	OrtEnv        *env          = nullptr;
	OrtSession    *session      = nullptr;
	OrtMemoryInfo *cpu_mem_info = nullptr;

	std::vector<OrtTensorInfo> in_info;
	std::vector<OrtTensorInfo> out_info;

	std::vector<std::vector<uint8_t>> input_bufs;
	std::vector<OrtValue *>           input_values;
	std::vector<const char *>         input_name_ptrs;

	/* Zero-initialised (std::vector value-init) at open() from each
     * output's static shape, so get_output() has a valid descriptor
     * before the first invoke() -- same contract DRP-AI's wrapper gives
     * (see inference_drpai.cpp's alp_inference_drpai_get_output doc). */
	std::vector<std::vector<uint8_t>> output_bufs;
	std::vector<OrtValue *>           output_values;
	std::vector<const char *>         output_name_ptrs;

	/** NULL-safe on every member; safe on a partially-populated state (any
	 *  prefix of open()'s steps may not have run yet).  Uses _ort_api()
	 *  rather than a stored pointer -- it is a memoised static, already
	 *  proven non-NULL by the time open() constructs the first OrtState,
	 *  so there is no ordering hazard resolving it again here. */
	~OrtState()
	{
		const OrtApi *api = _ort_api();
		if (api == nullptr) {
			return;
		}
		_release_values(api, output_values);
		_release_values(api, input_values);
		if (cpu_mem_info != nullptr) {
			api->ReleaseMemoryInfo(cpu_mem_info);
		}
		if (session != nullptr) {
			api->ReleaseSession(session);
		}
		if (env != nullptr) {
			api->ReleaseEnv(env);
		}
	}
};

} /* namespace */

/* ------------------------------------------------------------------ */
/* Backend hooks (C ABI, matching inference_yocto.c's declarations).   */
/*                                                                      */
/* Exception boundary: every hook body below is wrapped in try/catch.  */
/* inference_yocto.c calls these as plain C, so a C++ exception (e.g.  */
/* std::bad_alloc / std::length_error out of a std::vector::resize /   */
/* assign, std::make_unique, or std::string assignment) unwinding      */
/* through that frame would be undefined behaviour / std::terminate.  */
/* Nothing may escape this file.                                       */
/* ------------------------------------------------------------------ */

extern "C" alp_status_t alp_inference_ort_open(struct alp_inference         *h_,
                                               const alp_inference_config_t *cfg)
{
	if (h_ == nullptr || cfg == nullptr || cfg->model_data == nullptr || cfg->model_size == 0) {
		return ALP_ERR_INVAL;
	}
	auto *h = h_;

	const OrtApi *api = _ort_api();
	if (api == nullptr) {
		/* Linked libonnxruntime.so predates ORT_API_VERSION -- an ABI-version
         * mismatch, not a bad model. */
		return ALP_ERR_VERSION;
	}

	/* Declared outside the try so the catch handler below can still see it;
     * either way, whatever st owns is released by exactly one place --
     * ~OrtState() (see its own doc comment, issue #1494) -- when st is
     * destroyed, whether that is an early `return rc;` below, the
     * catch(...) handler falling through to the function's own return, or
     * (on success) never, because st.release() hands ownership to h->be_state
     * first. */
	std::unique_ptr<OrtState> st;
	try {
		st = std::make_unique<OrtState>();

		alp_status_t rc =
		    _ort_status_to_alp(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "alp_sdk_ort", &st->env));
		if (rc != ALP_OK) {
			return rc;
		}

		OrtSessionOptions *opts = nullptr;
		rc                      = _ort_status_to_alp(api->CreateSessionOptions(&opts));
		if (rc != ALP_OK) {
			return rc;
		}

		rc = _ort_status_to_alp(api->CreateSessionFromArray(
		    st->env, cfg->model_data, cfg->model_size, opts, &st->session));
		api->ReleaseSessionOptions(opts);
		if (rc != ALP_OK) {
			return rc;
		}

		rc = _ort_status_to_alp(
		    api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &st->cpu_mem_info));
		if (rc != ALP_OK) {
			return rc;
		}

		OrtAllocator *alloc = nullptr;
		rc                  = _ort_status_to_alp(api->GetAllocatorWithDefaultOptions(&alloc));
		if (rc != ALP_OK) {
			return rc;
		}

		size_t n_in = 0, n_out = 0;
		rc = _ort_status_to_alp(api->SessionGetInputCount(st->session, &n_in));
		if (rc == ALP_OK) {
			rc = _ort_status_to_alp(api->SessionGetOutputCount(st->session, &n_out));
		}

		st->in_info.resize(n_in);
		st->input_bufs.resize(n_in);
		st->input_values.assign(n_in, nullptr);
		st->input_name_ptrs.resize(n_in);
		st->out_info.resize(n_out);
		st->output_bufs.resize(n_out);
		st->output_values.assign(n_out, nullptr);
		st->output_name_ptrs.resize(n_out);

		for (size_t i = 0; i < n_in && rc == ALP_OK; ++i) {
			rc = _gather_tensor_info(api, st->session, i, /*is_input=*/true, alloc, st->in_info[i]);
		}
		for (size_t i = 0; i < n_out && rc == ALP_OK; ++i) {
			rc = _gather_tensor_info(
			    api, st->session, i, /*is_input=*/false, alloc, st->out_info[i]);
		}

		/* Allocate the SDK-owned buffers and wrap each one in an OrtValue that
         * ALIASES it (no copy): Run() below reads inputs from / writes outputs
         * into these buffers in place. */
		for (size_t i = 0; i < n_in && rc == ALP_OK; ++i) {
			st->input_bufs[i].resize(st->in_info[i].size_bytes);
			st->input_name_ptrs[i] = st->in_info[i].name.c_str();
			rc = _ort_status_to_alp(api->CreateTensorWithDataAsOrtValue(st->cpu_mem_info,
			                                                            st->input_bufs[i].data(),
			                                                            st->input_bufs[i].size(),
			                                                            st->in_info[i].shape.data(),
			                                                            st->in_info[i].shape.size(),
			                                                            st->in_info[i].dtype,
			                                                            &st->input_values[i]));
		}
		/* NOTE: output buffers are sized once here from the model's static
         * (dynamic-dims-pinned-to-1) shape and reused every invoke() -- a
         * model whose real output shape is data-dependent (e.g. NMS box
         * count) will fail Run() below with ORT_RUNTIME_EXCEPTION, mapped to
         * ALP_ERR_INVAL by _ort_errorcode_to_status().  Upgrade path: pass
         * nullptr in `outputs` and let ORT allocate fresh OrtValues per
         * call, re-reading GetTensorMutableData()+shape after each Run(). */
		for (size_t i = 0; i < n_out && rc == ALP_OK; ++i) {
			st->output_bufs[i].resize(st->out_info[i].size_bytes);
			st->output_name_ptrs[i] = st->out_info[i].name.c_str();
			rc =
			    _ort_status_to_alp(api->CreateTensorWithDataAsOrtValue(st->cpu_mem_info,
			                                                           st->output_bufs[i].data(),
			                                                           st->output_bufs[i].size(),
			                                                           st->out_info[i].shape.data(),
			                                                           st->out_info[i].shape.size(),
			                                                           st->out_info[i].dtype,
			                                                           &st->output_values[i]));
		}

		if (rc != ALP_OK) {
			return rc;
		}

		h->be_state = st.release();
		return ALP_OK;
	} catch (...) {
		/* st (if constructed) is destroyed when this function returns --
         * ~OrtState() releases every vendor resource it owns, however far
         * open() got before throwing (issue #1494: this used to be a
         * separate, incomplete hand-rolled copy that skipped the OrtValue
         * vectors; see the struct's doc comment). */
		return ALP_ERR_NOMEM;
	}
}

extern "C" size_t alp_inference_ort_num_inputs(struct alp_inference *h_)
{
	try {
		auto *h  = h_;
		auto *st = static_cast<OrtState *>(h->be_state);
		return (st != nullptr) ? st->in_info.size() : 0u;
	} catch (...) {
		return 0u;
	}
}

extern "C" size_t alp_inference_ort_num_outputs(struct alp_inference *h_)
{
	try {
		auto *h  = h_;
		auto *st = static_cast<OrtState *>(h->be_state);
		return (st != nullptr) ? st->out_info.size() : 0u;
	} catch (...) {
		return 0u;
	}
}

extern "C" alp_status_t
alp_inference_ort_get_input(struct alp_inference *h_, size_t index, alp_inference_tensor_t *out)
{
	try {
		auto *h  = h_;
		auto *st = static_cast<OrtState *>(h->be_state);
		if (st == nullptr) {
			return ALP_ERR_NOT_READY;
		}
		if (index >= st->in_info.size()) {
			return ALP_ERR_OUT_OF_RANGE;
		}

		const OrtTensorInfo &info = st->in_info[index];
		/* info.shape.size() is <= 4 by construction: _gather_tensor_info()
         * refuses (ALP_ERR_NOSUPPORT) any tensor whose rank exceeds
         * out->shape[]'s 4 slots at open() time (issue #1729), so this
         * clamp is defense-in-depth against that invariant ever slipping,
         * not a truncation this accessor performs on a live rank > 4 --
         * such a tensor never reaches here.  Every dim that reaches here
         * has also been bounds-checked against UINT16_MAX in
         * _gather_tensor_info(), so the cast below never truncates. */
		const size_t rank = (info.shape.size() > 4) ? 4 : info.shape.size();

		out->data       = st->input_bufs[index].data();
		out->size_bytes = info.size_bytes;
		out->dtype      = _ort_dtype_to_alp(info.dtype);
		out->rank       = static_cast<uint8_t>(rank);
		for (size_t i = 0; i < rank; ++i) {
			out->shape[i] = static_cast<uint16_t>(info.shape[i]);
		}
		for (size_t i = rank; i < 4; ++i) {
			out->shape[i] = 0;
		}
		out->scale      = 1.0f;
		out->zero_point = 0;
		return ALP_OK;
	} catch (...) {
		return ALP_ERR_NOMEM;
	}
}

extern "C" alp_status_t
alp_inference_ort_get_output(struct alp_inference *h_, size_t index, alp_inference_tensor_t *out)
{
	try {
		auto *h  = h_;
		auto *st = static_cast<OrtState *>(h->be_state);
		if (st == nullptr) {
			return ALP_ERR_NOT_READY;
		}
		if (index >= st->out_info.size()) {
			return ALP_ERR_OUT_OF_RANGE;
		}

		const OrtTensorInfo &info = st->out_info[index];
		/* See the matching comment in alp_inference_ort_get_input() above:
         * info.shape.size() is <= 4 by construction (issue #1729); this
         * clamp is defense-in-depth, not a live truncation. */
		const size_t rank = (info.shape.size() > 4) ? 4 : info.shape.size();

		out->data       = st->output_bufs[index].data();
		out->size_bytes = info.size_bytes;
		out->dtype      = _ort_dtype_to_alp(info.dtype);
		out->rank       = static_cast<uint8_t>(rank);
		for (size_t i = 0; i < rank; ++i) {
			out->shape[i] = static_cast<uint16_t>(info.shape[i]);
		}
		for (size_t i = rank; i < 4; ++i) {
			out->shape[i] = 0;
		}
		out->scale      = 1.0f;
		out->zero_point = 0;
		return ALP_OK;
	} catch (...) {
		return ALP_ERR_NOMEM;
	}
}

extern "C" alp_status_t alp_inference_ort_invoke(struct alp_inference *h_)
{
	try {
		auto *h  = h_;
		auto *st = static_cast<OrtState *>(h->be_state);
		if (st == nullptr) {
			return ALP_ERR_NOT_READY;
		}

		const OrtApi *api    = _ort_api();
		OrtStatus    *status = api->Run(st->session,
		                                nullptr,
		                                st->input_name_ptrs.data(),
		                                st->input_values.data(),
		                                st->input_values.size(),
		                                st->output_name_ptrs.data(),
		                                st->output_values.size(),
		                                st->output_values.data());
		return _ort_status_to_alp(status);
	} catch (...) {
		return ALP_ERR_NOMEM;
	}
}

extern "C" void alp_inference_ort_close(struct alp_inference *h_)
{
	if (h_ == nullptr) {
		return;
	}
	try {
		auto *h = h_;
		/* delete on a NULL OrtState* is well-defined (no-op); ~OrtState()
         * (see its own doc comment, issue #1494) is the ONE place that
         * releases the OrtValue vectors then cpu_mem_info then session
         * then env, in that order -- the same routine open()'s error/throw
         * paths rely on, so there is exactly one cleanup sequence to keep
         * in sync instead of two. */
		delete static_cast<OrtState *>(h->be_state);
		h->be_state = nullptr;
	} catch (...) {
		/* Nothing throws in practice on this path (no allocation), but the
         * C ABI boundary rule is unconditional -- see the file-level
         * "Exception boundary" note. */
	}
}
