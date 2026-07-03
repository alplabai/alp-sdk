/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable TFLM-backed inference backend.  Registered at
 * priority 50 against silicon_ref="*" so it wins everywhere
 * unless a vendor-specific NPU backend (ethos_u_aen on AEN,
 * ethos_u_n93 on i.MX 93) registers at priority 100 on the
 * matching silicon.
 *
 * Why C++?  TFLM exposes tflite::MicroInterpreter +
 * tflite::MicroMutableOpResolver only via its C++ surface; the
 * C wrapper in TFL is for the full runtime, not the micro
 * variant.  The .cpp file keeps the binding simple and lets
 * the AEN / N93 Ethos-U backends layer on top by reusing
 * AddEthosU() against the same op resolver.
 *
 * Variant logging
 *   The build-time CPU-kernel + Ethos-U variant pair (NEON /
 *   Helium / scalar-ref and U55 / U65 / U85 / none) get logged
 *   once on first open() so a HIL operator running on a multi-
 *   variant SoM (E4 / E6 / E8 ship U55 pair + U85) can confirm
 *   which kernel set the build linked against.
 *
 * Native-sim coverage
 *   CONFIG_TENSORFLOW_LITE_MICRO is not enabled under native_sim
 *   so this file is excluded from that build; the inference
 *   dispatcher falls back to sw_fallback (priority 0) which
 *   returns NOSUPPORT.  Real CI coverage lands when the AEN HIL
 *   runner comes online with the Vela toolchain.
 */

#include <cstring>
#include <cstdint>
#include <new>

#include <zephyr/logging/log.h>

extern "C" {
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/inference.h>
#include <alp/peripheral.h>
}

#include "inference_ops.h"
/* tflm_shared.h carries the extern "C" declarations of
 * alp_inference_tflm_ops + the variant helpers.  Including it here is
 * load-bearing: without the prior extern "C" declaration the `const`
 * vtable definition below gets C++ INTERNAL linkage and every
 * ethos_u_aen / ethos_u_n93 build fails to link with undefined
 * references to alp_inference_tflm_ops. */
#include "tflm_shared.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN) ||                                       \
    defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93)
#include "tensorflow/lite/micro/kernels/ethos_u/ethosu.h"
#define ALP_INFERENCE_TFLM_HAS_ETHOS_U 1
#endif

LOG_MODULE_REGISTER(alp_inference_tflm, CONFIG_LOG_DEFAULT_LEVEL);

namespace
{

#if defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85)
constexpr const char *kEthosUVariantName = "ethos-u85";
#elif defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65)
constexpr const char *kEthosUVariantName = "ethos-u65";
#elif defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55)
constexpr const char *kEthosUVariantName = "ethos-u55";
#else
constexpr const char *kEthosUVariantName = "none";
#endif

#if defined(CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_NEON)
constexpr const char *kTflmKernelVariant = "neon";
#elif defined(CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM)
constexpr const char *kTflmKernelVariant = "helium-mve";
#elif defined(CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF)
constexpr const char *kTflmKernelVariant = "scalar-ref";
#else
/* Orchestrator always emits one of the three on real builds; this
 * branch fires on hand-edited prj.conf without the variant.
 * Default to scalar-ref so the build still functions. */
constexpr const char *kTflmKernelVariant = "scalar-ref-default";
#endif

/* Per-handle TFLM state.  Lives in the backend state's be_data slot. */
struct TflmState {
	const tflite::Model *model = nullptr;
	/* Op resolver -- 32 ops covers MobileNetV2-style models with
     * room for a couple of custom Ethos-U ops.  Bigger models can
     * raise the bound via CONFIG_ALP_SDK_INFERENCE_TFLM_MAX_OPS. */
	tflite::MicroMutableOpResolver<32> resolver;
	tflite::MicroInterpreter          *interp = nullptr;
	/* Tensor arena -- caller-supplied via cfg.arena, or a built-in
     * static fall-back when cfg.arena is NULL. */
	uint8_t *arena_buf  = nullptr;
	size_t   arena_size = 0;
	bool     own_arena  = false;
};

constexpr size_t kDefaultArenaBytes = 128 * 1024;
alignas(16) uint8_t g_default_arena[kDefaultArenaBytes];
bool g_default_arena_in_use = false;

/* Map a TFL data type onto the alp_inference dtype enum. */
alp_inference_dtype_t tfl_dtype_to_alp(TfLiteType t)
{
	switch (t) {
	case kTfLiteFloat32:
		return ALP_INFERENCE_DTYPE_F32;
	case kTfLiteFloat16:
		return ALP_INFERENCE_DTYPE_F16;
	case kTfLiteInt8:
		return ALP_INFERENCE_DTYPE_INT8;
	case kTfLiteUInt8:
		return ALP_INFERENCE_DTYPE_UINT8;
	case kTfLiteInt16:
		return ALP_INFERENCE_DTYPE_INT16;
	case kTfLiteInt32:
		return ALP_INFERENCE_DTYPE_INT32;
	default:
		return ALP_INFERENCE_DTYPE_F32;
	}
}

void register_default_ops(tflite::MicroMutableOpResolver<32> &r)
{
	/* Minimum op set for the v0.2 reference MobileNetV2.  Vela-
     * compiled models replace the bulk of these with Ethos-U
     * custom ops; the fallback ones still need to be registered
     * for the layers Vela leaves in CPU-land. */
	r.AddConv2D();
	r.AddDepthwiseConv2D();
	r.AddFullyConnected();
	r.AddSoftmax();
	r.AddReshape();
	r.AddAveragePool2D();
	r.AddMaxPool2D();
	r.AddAdd();
	r.AddMul();
	r.AddQuantize();
	r.AddDequantize();
	r.AddPad();
	r.AddMean();
	r.AddRelu();
	r.AddRelu6();
	r.AddLogistic();
#if defined(ALP_INFERENCE_TFLM_HAS_ETHOS_U)
	r.AddEthosU();
#endif
}

void fill_tensor_descriptor(const TfLiteTensor *t, alp_inference_tensor_t *out)
{
	out->data       = t->data.raw;
	out->size_bytes = t->bytes;
	out->dtype      = tfl_dtype_to_alp(t->type);
	out->rank       = (uint8_t)((t->dims->size <= 4) ? t->dims->size : 4);
	for (int i = 0; i < out->rank; ++i) {
		out->shape[i] = (uint16_t)t->dims->data[i];
	}
	out->scale =
	    (t->quantization.type == kTfLiteAffineQuantization)
	        ? static_cast<TfLiteAffineQuantization *>(t->quantization.params)->scale->data[0]
	        : 1.0f;
	out->zero_point =
	    (t->quantization.type == kTfLiteAffineQuantization)
	        ? static_cast<TfLiteAffineQuantization *>(t->quantization.params)->zero_point->data[0]
	        : 0;
}

} // namespace

/* ------------------------------------------------------------------ */
/* Backend ops bodies.  C++-linkage internally; the ops vtable is
 * declared in an extern "C" header so the symbols below see the
 * same `alp_inference_backend_state_t` layout as the dispatcher. */
/* ------------------------------------------------------------------ */

extern "C" {

static alp_status_t tflm_open(const alp_inference_config_t  *cfg,
                              alp_inference_backend_state_t *state,
                              alp_capabilities_t            *caps_out)
{
	/* Pinned-backend gate (the dispatcher contract in
	 * src/inference_dispatch.c: a pinned open the serving backend
	 * cannot honour returns NOSUPPORT).  This vtable serves the CPU
	 * TFLM executor and -- when the Ethos-U op-resolver entry is
	 * compiled in -- the ethos_u_aen / ethos_u_n93 registrations.
	 * DRP-AI3 and DEEPX DX-M1 are A55/Linux-side engines (issues
	 * #58/#59): no Zephyr registry backend can ever serve those
	 * pins, so reject them here instead of failing deep inside the
	 * flatbuffer parse with a misleading INVAL. */
	switch (cfg->backend) {
	case ALP_INFERENCE_BACKEND_AUTO:
	case ALP_INFERENCE_BACKEND_CPU:
		break;
#if defined(ALP_INFERENCE_TFLM_HAS_ETHOS_U)
	case ALP_INFERENCE_BACKEND_ETHOS_U:
		break;
#endif
	default:
		return ALP_ERR_NOSUPPORT;
	}

	/* One-shot variant log so HIL operators can confirm which
     * kernel set the build actually linked against.  Drops to
     * LOG_DBG after the first call to keep per-model open() noise
     * down. */
	static bool s_variant_logged = false;
	if (!s_variant_logged) {
		LOG_INF(
		    "tflm backend: cpu_kernels=%s npu_variant=%s", kTflmKernelVariant, kEthosUVariantName);
		s_variant_logged = true;
	} else {
		LOG_DBG(
		    "tflm backend: cpu_kernels=%s npu_variant=%s", kTflmKernelVariant, kEthosUVariantName);
	}

	auto *st = new (std::nothrow) TflmState();
	if (st == nullptr) return ALP_ERR_NOMEM;

	if (cfg->arena != nullptr && cfg->arena_bytes > 0) {
		st->arena_buf  = static_cast<uint8_t *>(cfg->arena);
		st->arena_size = cfg->arena_bytes;
		st->own_arena  = false;
	} else {
		if (g_default_arena_in_use) {
			delete st;
			return ALP_ERR_NOMEM;
		}
		g_default_arena_in_use = true;
		st->arena_buf          = g_default_arena;
		st->arena_size         = kDefaultArenaBytes;
		st->own_arena          = true;
	}

	st->model = tflite::GetModel(cfg->model_data);
	if (st->model->version() != TFLITE_SCHEMA_VERSION) {
		if (st->own_arena) g_default_arena_in_use = false;
		delete st;
		return ALP_ERR_INVAL;
	}

	register_default_ops(st->resolver);

	st->interp = new (std::nothrow)
	    tflite::MicroInterpreter(st->model, st->resolver, st->arena_buf, st->arena_size);
	if (st->interp == nullptr) {
		if (st->own_arena) g_default_arena_in_use = false;
		delete st;
		return ALP_ERR_NOMEM;
	}

	if (st->interp->AllocateTensors() != kTfLiteOk) {
		delete st->interp;
		if (st->own_arena) g_default_arena_in_use = false;
		delete st;
		return ALP_ERR_IO;
	}

	state->be_data  = st;
	state->dev      = nullptr;
	caps_out->flags = 0u; /* per-instance flags layered by NPU backends */
	return ALP_OK;
}

static size_t tflm_num_inputs(alp_inference_backend_state_t *state)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	return (st != nullptr && st->interp != nullptr) ? st->interp->inputs_size() : 0u;
}

static size_t tflm_num_outputs(alp_inference_backend_state_t *state)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	return (st != nullptr && st->interp != nullptr) ? st->interp->outputs_size() : 0u;
}

static alp_status_t
tflm_get_input(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	if (st == nullptr || st->interp == nullptr) return ALP_ERR_NOT_READY;
	if (index >= st->interp->inputs_size()) return ALP_ERR_OUT_OF_RANGE;
	fill_tensor_descriptor(st->interp->input(index), out);
	return ALP_OK;
}

static alp_status_t
tflm_get_output(alp_inference_backend_state_t *state, size_t index, alp_inference_tensor_t *out)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	if (st == nullptr || st->interp == nullptr) return ALP_ERR_NOT_READY;
	if (index >= st->interp->outputs_size()) return ALP_ERR_OUT_OF_RANGE;
	fill_tensor_descriptor(st->interp->output(index), out);
	return ALP_OK;
}

static alp_status_t tflm_invoke(alp_inference_backend_state_t *state)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	if (st == nullptr || st->interp == nullptr) return ALP_ERR_NOT_READY;
	return (st->interp->Invoke() == kTfLiteOk) ? ALP_OK : ALP_ERR_IO;
}

static void tflm_close(alp_inference_backend_state_t *state)
{
	auto *st = static_cast<TflmState *>(state->be_data);
	if (st == nullptr) return;

	delete st->interp;
	if (st->own_arena) g_default_arena_in_use = false;
	delete st;
	state->be_data = nullptr;
}

/* Shared ops vtable.  Exported via the C ABI as
 * alp_inference_tflm_ops so the ethos_u_aen / ethos_u_n93
 * backends can register against the same body without
 * duplicating it -- the only thing that changes across the
 * three registrations is silicon_ref + vendor + priority. */
const alp_inference_ops_t alp_inference_tflm_ops = {
	/* .open        */ tflm_open,
	/* .num_inputs  */ tflm_num_inputs,
	/* .num_outputs */ tflm_num_outputs,
	/* .get_input   */ tflm_get_input,
	/* .get_output  */ tflm_get_output,
	/* .invoke      */ tflm_invoke,
	/* .close       */ tflm_close,
};

ALP_BACKEND_REGISTER(inference,
                     tflm,
                     {
                         /* .silicon_ref */ "*",
                         /* .vendor      */ "tflm",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 50,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

/* ------------------------------------------------------------------ */
/* Variant introspection.  Apps + integration tests can ask the SDK
 * which kernel-variant the build actually selected -- handy when the
 * runtime is on a multi-variant SoM (E4 / E6 / E8: U55 pair + U85)
 * and the customer needs to confirm the Vela target on their model-
 * compile pipeline matches.
 *
 * The returned pointers are static-storage string literals; callers
 * must NOT free.  Stable across builds for the same CONFIG_* set.
 */
/* ------------------------------------------------------------------ */

const char *alp_inference_tflm_cpu_kernel_variant(void)
{
	return kTflmKernelVariant;
}

const char *alp_inference_tflm_npu_variant_name(void)
{
	/* TODO(v0.7 + Arm driver vendor pack): when multiple Ethos-U
     * variants are linked (U55+U85 on E4/E6/E8), the active variant
     * for a given handle is decided by the Vela-compiled model's
     * accelerator-config tag -- not by build-time selection.  Surface
     * a per-handle variant query once the Arm driver pack lands.
     * Today we return the highest-tier variant the build linked,
     * which is the right answer for the headline log line. */
	return kEthosUVariantName;
}

} /* extern "C" */
