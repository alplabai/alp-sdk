/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file inference.h
 * @brief Alp SDK unified ML inference abstraction.
 *
 * Lifts compiled-model loaders + dispatch into a single uniform
 * surface so apps don't need to know which NPU is bonded out on
 * the active SoM.  Registry backends (Zephyr, via
 * ALP_BACKEND_REGISTER) plus Yocto-side hooks -- DRP-AI3 and
 * DEEPX DX-M1 are A55/Linux-only and dispatch through
 * `inference_yocto.c` instead of the Zephyr registry:
 *
 *   - **CPU** (`tflm`, silicon_ref `"*"`, priority 50): real, via
 *     TFLM's reference kernels.  Useful for development and for
 *     model parts that don't map to an NPU's supported op set.
 *   - **Ethos-U**: real, via the same TFLM executor plus the
 *     Ethos-U op resolver against a Vela-compiled model --
 *     registered per-part on Alif Ensemble E3/E4/E5/E6/E7/E8
 *     (`ethos_u_aen_e3`..`ethos_u_aen_e8`, priority 100 each) and
 *     NXP i.MX 93 (`ethos_u_n93`, priority 100).
 *   - **DRP-AI3** (Renesas RZ/V2N N44): real A55/Yocto-side backend
 *     (`src/yocto/inference_drpai.cpp`) against the real
 *     `MeraDrpRuntimeWrapper` DRP-AI TVM runtime -- an M-class
 *     (Zephyr) handle can never be DRP-AI-backed.  Gated
 *     `ALP_SDK_USE_DRPAI_V2N` (default OFF); BENCH-UNVERIFIED
 *     (issue #58).
 *   - **DEEPX DX-M1**: real A55/Yocto-side backend
 *     (`src/yocto/inference_deepx.cpp`) against the real
 *     `dxrt::InferenceEngine` runtime.  Gated
 *     `ALP_SDK_USE_DEEPX_DXM1` (default OFF); BENCH-UNVERIFIED
 *     (issue #59).
 *   - **sw_fallback** (priority 0): every call returns
 *     ALP_ERR_NOSUPPORT; wins only when no other backend links for
 *     the active silicon.
 *
 * Vendor-specific accelerator paths -- `<alp/ext/renesas/inference.h>`
 * (DRP-AI3 pipeline-stage + AI-SRAM pinning) and
 * `<alp/ext/deepx/inference.h>` (DX-M1 slot + DRAM-tile pinning) --
 * remain available as escape hatches when the unified API can't
 * express what the vendor SDK offers.  Both currently return
 * ALP_ERR_NOSUPPORT on every call past the vendor-handle gate: the
 * Zephyr registry ships no DRP-AI/DEEPX inference backend for those
 * knobs to bind to (DRP-AI3 and DX-M1 are A55/Linux-only engines),
 * and wiring them through to the Yocto handle is follow-up work
 * (issues #58/#59).  The unification stance is "best-effort, not
 * absolute".
 *
 * @par ABI status: [ABI-STABLE]
 *      Shape is frozen.  ALP_ERR_NOSUPPORT -- for a target/backend
 *      combination that hasn't landed, or a vendor knob still
 *      pending its HAL integration -- is part of the stable
 *      contract; callers handle it via @ref alp_last_error.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_INFERENCE_H
#define ALP_INFERENCE_H

#include <stdint.h>
#include <stddef.h>

#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Backend selector.  AUTO routes to the best available NPU; the
 *  others force a specific backend (useful for benchmarking, or to
 *  fall through to CPU when the model doesn't map to NPU ops).
 *
 *  The `ETHOS_U` token is a single customer-facing handle that
 *  covers every Arm Ethos NPU variant the SDK targets:
 *  - Ethos-U85 on Alif Ensemble E4 / E6 / E8 (Transformer-capable;
 *    the loader picks this first on those SKUs via the
 *    `CONFIG_ALP_TFLM_ETHOS_U85=y` per-NPU driver gate).
 *  - Ethos-U55 on every Alif Ensemble SKU (two per SoC;
 *    `CONFIG_ALP_TFLM_ETHOS_U55=y`).
 *  - Ethos-U65 on NXP i.MX 93 / E1M-NX9101 (`CONFIG_ALP_TFLM_ETHOS_U65=y`
 *    + the N93-specific driver shim `CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y`).
 *  Customers don't have to know which variant the silicon carries;
 *  Vela picks at model-compile time and the runtime dispatches via
 *  the matching driver shim emitted by `scripts/alp_project.py`. */
typedef enum {
	ALP_INFERENCE_BACKEND_AUTO    = 0,
	ALP_INFERENCE_BACKEND_CPU     = 1, /**< Portable CPU floor: TFLM reference
					    *   kernels on M-class/Zephyr, ONNX
					    *   Runtime on the A55s under Yocto.
					    *   Lowest priority under AUTO. */
	ALP_INFERENCE_BACKEND_ETHOS_U = 2, /**< Arm Ethos-U via Vela (U55 / U65 / U85). */
	ALP_INFERENCE_BACKEND_DRPAI   = 3, /**< Renesas DRP-AI3. */
	ALP_INFERENCE_BACKEND_DEEPX_DXM1 =
	    4 /**< DEEPX DX-M1 (canonical id; matches the .alpmodel `deepx_dxm1` backend string). */
} alp_inference_backend_t;

/** Model format.  Each backend supports a subset; AUTO picks based
 *  on whichever loader matches the magic bytes at the head of the
 *  model buffer. */
typedef enum {
	ALP_INFERENCE_MODEL_TFLITE     = 0, /**< `.tflite` flatbuffer. */
	ALP_INFERENCE_MODEL_VELA       = 1, /**< Vela-compiled `.tflite`. */
	ALP_INFERENCE_MODEL_DRPAI      = 2, /**< Renesas DRP-AI binary. */
	ALP_INFERENCE_MODEL_DXNN       = 3, /**< DEEPX DXNN binary. */
	ALP_INFERENCE_MODEL_EXECUTORCH = 4, /**< ExecuTorch program.  Write side is
					     *   live: ExecutorchAdapter (issue #1260)
					     *   produces this format from a .pte
					     *   source.  No backend runtime consumes
					     *   it yet.  Backend selection is
					     *   silicon_ref+priority and never reads
					     *   cfg->format, so the outcome depends on
					     *   which backend wins: on a TFLM-linked
					     *   build, alp_inference_open() falls
					     *   through to the CPU/TFLM backend, whose
					     *   flatbuffer verify rejects the raw .pte
					     *   bytes, failing with @ref ALP_ERR_INVAL
					     *   (not a deliberate format check); with no
					     *   TFLM linked, sw_fallback (priority 0)
					     *   wins instead and fails with @ref
					     *   ALP_ERR_NOSUPPORT.  See issue #1260. */
	ALP_INFERENCE_MODEL_ONNX       = 5  /**< Raw `.onnx` graph (ONNX Runtime CPU backend). */
} alp_inference_model_format_t;

/** Tensor element type. */
typedef enum {
	ALP_INFERENCE_DTYPE_F32   = 0,
	ALP_INFERENCE_DTYPE_F16   = 1,
	ALP_INFERENCE_DTYPE_INT8  = 2,
	ALP_INFERENCE_DTYPE_UINT8 = 3,
	ALP_INFERENCE_DTYPE_INT16 = 4,
	ALP_INFERENCE_DTYPE_INT32 = 5
} alp_inference_dtype_t;

/** Tensor descriptor — what `get_input` / `get_output` return. */
typedef struct {
	void                 *data;       /**< Backend-owned buffer. */
	size_t                size_bytes; /**< Total buffer size. */
	alp_inference_dtype_t dtype;
	uint8_t               rank;     /**< 0..4 typical. */
	uint16_t              shape[4]; /**< Most-significant first. */
	/** Quantisation params (only meaningful when dtype is integer). */
	float   scale;
	int32_t zero_point;
} alp_inference_tensor_t;

typedef struct alp_inference alp_inference_t;

typedef struct {
	const void                  *model_data; /**< Pointer to model bytes. */
	size_t                       model_size;
	alp_inference_model_format_t format;
	alp_inference_backend_t      backend;
	/** Bytes of scratch arena the backend may use.  TFLM-style
     *  backends size this from the compile-time tensor arena
     *  estimate; if 0, the backend uses a built-in default. */
	size_t arena_bytes;
	/** Caller-allocated arena, or NULL to let the backend use its
	 *  built-in default.  A model that dispatches onto the Ethos-U NPU
	 *  (it carries the fused Ethos-U op -- typically a Vela-compiled
	 *  model, whatever @c format reports) has NO safe default: the NPU is
	 *  a DMA master whose accesses are pinned to the SRAM AXI port, so the
	 *  arena MUST be an explicit NPU-reachable (SRAM0-resident) buffer
	 *  sized to the model.  @c arena = NULL for such a model is rejected
	 *  with @ref ALP_ERR_INVAL (see examples/aen/aen-npu-inference-alp). */
	void *arena;
} alp_inference_config_t;

/**
 * @brief Default-initialize an @ref alp_inference_config_t for model
 *        buffer @p id.
 *
 * Identity from @p id (the @c model_data pointer -- there is no
 * separate instance-id field for an inference handle).  @c model_size
 * is mandatory and paired with @c model_data with no sensible
 * default, so it defaults to 0 as a "you must set this" sentinel --
 * set it to the actual byte length of the buffer passed as @p id
 * before calling open(). @c format defaults to @ref
 * ALP_INFERENCE_MODEL_TFLITE (the enum's zero value and the most
 * common on-disk model format), @c backend defaults to @ref
 * ALP_INFERENCE_BACKEND_AUTO (route to whichever NPU/CPU backend is
 * available on the active SoM), @c arena_bytes = 0 and @c arena = NULL
 * both use the ALREADY-documented backend defaults ("if 0, the
 * backend uses a built-in default" / "NULL to let the backend use
 * heap") -- EXCEPT that a model which dispatches onto the Ethos-U NPU
 * has no safe default arena (see the @c arena field): open() rejects it
 * with @ref ALP_ERR_INVAL, so an NPU model must set @c arena explicitly.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_INFERENCE_CONFIG_DEFAULT(id) \
	((alp_inference_config_t){ .model_data  = (id), \
	                           .model_size  = 0u, \
	                           .format      = ALP_INFERENCE_MODEL_TFLITE, \
	                           .backend     = ALP_INFERENCE_BACKEND_AUTO, \
	                           .arena_bytes = 0u, \
	                           .arena       = NULL })

/**
 * @brief Load a compiled model and prepare it for invocation.
 *
 * Verifies the model's format / signature, allocates per-tensor
 * buffers (or maps them into the caller's arena), and binds the
 * selected backend.
 *
 * @param[in] cfg  Configuration; @c model_data must be non-NULL.
 * @return Open handle, or NULL with @ref alp_last_error set to one
 *         of ALP_ERR_INVAL (NULL cfg/model_data, model_size 0, bad
 *         magic / unsupported model format, or an Ethos-U model
 *         opened with @c arena == NULL), ALP_ERR_NOT_PRESENT_ON_THIS_SOC
 *         (no backend registered for the active silicon),
 *         ALP_ERR_NOT_IMPLEMENTED (registered backend has no open
 *         hook), ALP_ERR_NOSUPPORT (a pinned @c backend the selected
 *         backend can't serve, e.g. ETHOS_U pinned on a CPU-only
 *         build; on the ONNX Runtime / DEEPX DX-M1 / TFLM backends, any
 *         model tensor whose rank exceeds 4 -- @ref
 *         alp_inference_tensor_t's @c shape has exactly 4 slots, and
 *         a model that doesn't fit is refused rather than opened with
 *         a silently-truncated shape; on the ONNX Runtime / TFLM
 *         backends, a tensor dim too large for the @c shape slots'
 *         @c uint16_t type; or, on the DEEPX DX-M1 backend, a model
 *         with more input tensors than the backend can stage),
 *         ALP_ERR_NOMEM (handle-pool or arena allocation failure), or
 *         ALP_ERR_IO (backend's tensor-arena allocation failed).
 */
alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg);

/** Options for loading a `.alpmodel` package (the fat multi-backend
 *  container).  Provide the bytes in-memory (@c data/@c size, MCU embed)
 *  or a storage @c path (Linux).  @c backend = AUTO lets the loader pick
 *  the best blob for the active SoM; pin a specific backend to force it. */
typedef struct {
	const void             *data;        /**< Package bytes, or NULL to use @c path. */
	size_t                  size;        /**< Byte count when @c data is set. */
	const char             *path;        /**< Storage path (Linux), or NULL. */
	alp_inference_backend_t backend;     /**< AUTO, or a forced backend. */
	size_t                  arena_bytes; /**< 0 = size from the manifest. */
	void                   *arena;       /**< Caller arena, or NULL for backend default. */
} alp_model_open_opts_t;

/**
 * @brief Load a `.alpmodel` package and open the best-fit blob for this SoM.
 *
 * Parses the package, selects the blob whose backend is available on the
 * active SoC, whose `silicon_ref` is compatible, and that fits the device
 * NPU envelope (arena SRAM); ties break by the SoM's preferred backend.
 * Delegates the chosen blob to the matching backend via @ref
 * alp_inference_open — the returned handle works with all the
 * @c alp_inference_* accessors unchanged.
 *
 * @param[in] opts  Load options; @c data (with @c size) or @c path required.
 * @return Open handle, or NULL — read @ref alp_last_error for the cause:
 *         ALP_ERR_INVAL (bad opts / bad magic / corrupt),
 *         ALP_ERR_VERSION (package newer than this loader),
 *         ALP_ERR_NO_BACKEND / ALP_ERR_NO_FIT / ALP_ERR_NOT_FOUND (selection),
 *         or any @ref alp_inference_open error from the chosen backend.
 */
alp_inference_t *alp_inference_open_alpmodel(const alp_model_open_opts_t *opts);

/**
 * @brief Number of input tensors the model expects.
 *
 * @param[in] inf  Handle from @ref alp_inference_open, or NULL.
 * @return Input tensor count, or 0 if @p inf is NULL or closed.
 */
size_t alp_inference_num_inputs(alp_inference_t *inf);

/**
 * @brief Number of output tensors the model produces.
 *
 * @param[in] inf  Handle from @ref alp_inference_open, or NULL.
 * @return Output tensor count, or 0 if @p inf is NULL or closed.
 */
size_t alp_inference_num_outputs(alp_inference_t *inf);

/**
 * @brief Get a descriptor for input tensor @p index.
 *
 * The returned tensor's `data` pointer is owned by the SDK and
 * remains valid until @ref alp_inference_close.  Apps fill the
 * buffer before calling @ref alp_inference_invoke.
 *
 * @param[in]  inf    Handle from @ref alp_inference_open.
 * @param[in]  index  0..@ref alp_inference_num_inputs - 1.
 * @param[out] out    Filled with the tensor descriptor.
 *                    Must be non-NULL.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_NOT_READY.
 */
alp_status_t
alp_inference_get_input(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out);

/**
 * @brief Get a descriptor for output tensor @p index.
 *
 * Same ownership semantics as @ref alp_inference_get_input.  The
 * output buffer's contents are valid after @ref alp_inference_invoke
 * returns ALP_OK; reading before the first invoke returns the
 * backend's zero-initialised buffer.
 *
 * @param[in]  inf    Handle from @ref alp_inference_open.
 * @param[in]  index  0..@ref alp_inference_num_outputs - 1.
 * @param[out] out    Filled with the tensor descriptor.
 *                    Must be non-NULL.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE /
 *         ALP_ERR_NOT_READY.
 */
alp_status_t
alp_inference_get_output(alp_inference_t *inf, size_t index, alp_inference_tensor_t *out);

/**
 * @brief Run one inference pass.
 *
 * Dispatches to the bound backend.  On Ethos-U / DRP-AI / DX-M1
 * backends this offloads to the NPU and blocks the calling thread
 * until the result lands; on the CPU backend it executes in-thread.
 *
 * @param[in] inf  Handle from @ref alp_inference_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (handle closed) /
 *         ALP_ERR_INVAL / ALP_ERR_TIMEOUT (NPU stuck) /
 *         ALP_ERR_IO (NPU error).
 */
alp_status_t alp_inference_invoke(alp_inference_t *inf);

/**
 * @brief Wall-clock duration of the most recent successful @ref
 *        alp_inference_invoke call, in microseconds.
 *
 * Brackets the backend's synchronous invoke -- every shipped backend
 * (TFLM/Ethos-U, DRP-AI3, DEEPX DX-M1, ONNX Runtime) blocks the
 * calling thread until the result lands (see @ref alp_inference_invoke),
 * so this is a real per-invoke measurement taken by the dispatcher
 * itself, not an estimate a backend opts into -- there is no backend
 * that "can't" report it.
 *
 * Reports only the LAST successful invoke; it does not accumulate
 * statistics or retain a history.  A caller building a latency
 * distribution (mean / p95 / run count) calls this once per @ref
 * alp_inference_invoke and keeps the samples itself -- the SDK holds
 * no ring buffer, so this accessor's memory cost is fixed regardless
 * of how many samples a caller wants, on a part where the inference
 * arena is already the binding constraint.
 *
 * A failed invoke (any return other than @ref ALP_OK) does not update
 * the stored value -- reading after a failed invoke still returns the
 * last value a *successful* invoke produced (or @ref ALP_ERR_NOT_READY
 * if none has succeeded yet).
 *
 * Overlapping invokes on one handle -- two threads calling @ref
 * alp_inference_invoke on the SAME @p inf concurrently, a real
 * interleaving this handle's op-counting permits (it is a drain
 * counter, not a mutex) -- are last-STORE-wins, not largest-duration-
 * or largest-finish-time-wins: the stored value is whichever invoke's
 * atomic store instruction executes last, which is not necessarily the
 * invoke that finished last in wall-clock time (a scheduler can
 * preempt between an invoke's compute finishing and its store
 * running). Concurrent invokes on one handle are unusual -- most
 * callers own one handle per thread -- but the SDK does not forbid it,
 * and this accessor makes no attempt to attribute a reading to a
 * specific invoke call when more than one is in flight. Read this only
 * when you know at most one @ref alp_inference_invoke is outstanding
 * on this handle, or treat a reading taken while invokes may overlap
 * as "some recent invoke's duration," not "this specific call's."
 *
 * Units: MICROSECONDS. A caller feeding this into the tier-2 benchmark
 * recipe schema's `latency_ms_mean` / `latency_ms_p95` fields
 * (MILLISECONDS) must divide by 1000 itself -- this accessor performs
 * no unit conversion, and a missed division publishes a number 1000x
 * too large.
 *
 * @param[in]  inf     Handle from @ref alp_inference_open.
 * @param[out] out_us  Filled with the last successful invoke's
 *                     duration in whole microseconds (rounded to
 *                     nearest, not floored -- a sub-microsecond
 *                     invoke reports 0 only when it truly rounds to
 *                     0, not by truncation bias).  Must be non-NULL.
 *
 *                     Ceiling: the Zephyr/M-class dispatcher stores the
 *                     value in a 32-bit field internally -- a
 *                     naturally-aligned @c uint32_t load/store is
 *                     lock-free on every Cortex-M this SDK targets,
 *                     unlike the @c uint64_t the field used to be
 *                     (M-profile has no 64-bit atomic instruction, so
 *                     GCC lowers a 64-bit @c __atomic_store_n /
 *                     @c __atomic_load_n to a libatomic call, and the
 *                     Zephyr SDK's arm-zephyr-eabi toolchain ships no
 *                     libatomic -- every Cortex-M app failed to link).
 *                     A duration exceeding @c UINT32_MAX - 1
 *                     microseconds (4294967294 us, just under
 *                     ~71.58 minutes) therefore SATURATES at
 *                     @c UINT32_MAX - 1 rather than wrapping -- the
 *                     raw @c UINT32_MAX value is reserved as the "no
 *                     successful invoke yet" sentinel (see
 *                     @ref ALP_ERR_NOT_READY below) and is never
 *                     returned as a measured duration, so the ceiling
 *                     is one microsecond short of the field's true
 *                     numeric range. Saturating here is implausible
 *                     for one invoke in practice, but it is the
 *                     honest, OBSERVABLE ceiling: a caller reads
 *                     @c ALP_OK plus this saturated value, not
 *                     @ref ALP_ERR_NOT_READY.
 *
 *                     A second, TIGHTER, and considerably more
 *                     dangerous limit sits in front of that one on a
 *                     target without
 *                     @c CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER: the
 *                     underlying hardware cycle counter itself wraps
 *                     modulo 2^32 cycles first (depends on
 *                     @c CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC), and this
 *                     accessor has no way to detect that wrap -- unlike
 *                     the 32-bit storage ceiling above, which fails
 *                     safe (a saturated value that reads back as
 *                     implausibly huge), a wrapped cycle-counter delta
 *                     produces a plausible-looking, too-small duration
 *                     reported with @c ALP_OK, not an error. Zephyr's
 *                     @c CORTEX_M_SYSTICK_64BIT_CYCLE_COUNTER Kconfig
 *                     (which a plain Cortex-M SysTick target's
 *                     @c CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER depends
 *                     on) defaults to `y` only when
 *                     @c CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC exceeds
 *                     60 MHz, so under STOCK Kconfig the 32-bit-counter
 *                     fallback this paragraph describes only actually
 *                     compiles in on a target clocked at 60 MHz or
 *                     below, where the wrap ceiling is
 *                     >= ~71.58 s (2^32 cycles / 60 MHz) -- a
 *                     400 MHz target left at Kconfig defaults selects
 *                     the 64-bit counter instead and never hits this
 *                     wrap at all; "~10.74 s at 400 MHz" (a naive
 *                     2^32-cycles-at-400 MHz calculation) is not a
 *                     combination stock Kconfig produces. On
 *                     @c CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER targets
 *                     (e.g. the E1M-AEN801 M55 cores, and any target
 *                     with an ARM architected/GIC timer, which selects
 *                     this Kconfig unconditionally) the wrap limit
 *                     does not apply and the 32-bit storage ceiling
 *                     above is the binding one. The Yocto/A-class
 *                     dispatcher has neither limit (native 64-bit
 *                     atomics on x86-64/aarch64, a 64-bit
 *                     @c CLOCK_MONOTONIC nanosecond delta), so it has
 *                     no ceiling to saturate against at all.
 *
 *                     Cross-OS ceiling contract: on BOTH OSes, once
 *                     @ref alp_inference_invoke has completed with
 *                     @ref ALP_OK at least once, this accessor reports
 *                     @c ALP_OK with a real duration -- never
 *                     @ref ALP_ERR_NOT_READY merely because that
 *                     duration was long. Zephyr's UINT32_MAX - 1 clamp
 *                     (above) is what makes this true there: without
 *                     it, a duration landing exactly on the field's
 *                     raw numeric ceiling would collide with the "no
 *                     successful invoke yet" sentinel and read back as
 *                     @ref ALP_ERR_NOT_READY with @p out_us never
 *                     written, silently contradicting the SATURATES
 *                     wording above. @ref ALP_ERR_NOT_READY means
 *                     exactly one thing on either OS: @p inf is
 *                     NULL/closed, or no invoke has ever completed
 *                     with @ref ALP_OK on this handle yet -- never "the
 *                     invoke was too slow to report."
 *
 * @return ALP_OK, or one of:
 *         - @ref ALP_ERR_INVAL -- @p out_us is NULL.
 *         - @ref ALP_ERR_NOT_READY -- @p inf is NULL/closed, or no
 *           @ref alp_inference_invoke call has completed with
 *           @ref ALP_OK on this handle yet. Never returned merely
 *           because the last successful invoke's duration was long --
 *           see the Cross-OS ceiling contract above.
 *         - @ref ALP_ERR_NOSUPPORT -- the stub build only (no
 *           inference backend compiled in at all): there is no timing
 *           mechanism here that could ever have been populated, so
 *           this is the honest answer regardless of @p out_us or
 *           handle state.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      New accessor; the file-level marker stays [ABI-STABLE] -- see
 *      docs/abi-markers.md's mixed-tier note.
 */
alp_status_t alp_inference_last_invoke_latency_us(alp_inference_t *inf, uint64_t *out_us);

/**
 * @brief Release the model + tensor buffers.  NULL-safe.
 *
 * @param[in] inf  Handle from @ref alp_inference_open, or NULL.
 */
void alp_inference_close(alp_inference_t *inf);

/**
 * @brief Query the per-instance capabilities of an opened inference handle.
 *
 * Returns the refined capability descriptor captured at @ref
 * alp_inference_open time by the selected backend's `probe` /
 * `open` hook.  The pointer is valid for the lifetime of the
 * handle.
 *
 * @param[in] inf  Handle from @ref alp_inference_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p inf
 *         is NULL.
 */
const alp_capabilities_t *alp_inference_capabilities(const alp_inference_t *inf);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_INFERENCE_H */
