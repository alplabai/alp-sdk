/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * INTERNAL -- the real layout of `struct alp_inference`, shared between the
 * Yocto dispatcher and its per-backend helpers.  Not a public header: it is
 * not installed, `<alp/inference.h>` keeps the type opaque
 * (`typedef struct alp_inference alp_inference_t;`), and nothing outside
 * `src/yocto/` may include it.
 *
 * Why this exists (issue #1257)
 * ----------------------------
 * Each backend needs to reach `be_state` on a handle the dispatcher owns.
 * Before this header they each hand-mirrored the struct:
 *
 *     struct alp_inference_handle_layout {
 *             bool                    in_use;
 *             alp_inference_backend_t backend;
 *             void                   *be_state;
 *     };
 *
 * and cast the handle to it.  That mirror has a DIFFERENT field order from
 * the real struct below, and was correct only by a coincidence of pointer
 * width: on LP64 the real struct puts `be_state` at offset 8 (4-byte enum,
 * 4 bytes of padding, then the pointer) and the mirror also lands it at 8
 * (1-byte bool, 3 bytes of padding, 4-byte enum, then the pointer).  On an
 * ILP32 target the real offset is 4 and the mirror's is 8, so a write
 * through the mirror would silently corrupt the adjacent field.
 *
 * Nothing tied the copies to the original, so adding or reordering a field
 * could corrupt handles in every backend at once with no diagnostic.  That
 * nearly happened during the use-after-free fix, which had to thread new
 * fields between `be_state` and `in_use` specifically to keep the mirrored
 * prefix byte-identical.
 *
 * One definition makes the compiler the enforcer instead of a comment.
 */

#ifndef ALP_SDK_SRC_YOCTO_INFERENCE_HANDLE_INTERNAL_H_
#define ALP_SDK_SRC_YOCTO_INFERENCE_HANDLE_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/inference.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dispatcher-owned inference handle.
 *
 * Field ORDER is load-bearing: `inference_yocto.c`'s `pool_acquire()` zeroes
 * `offsetof(struct alp_inference, in_use)` bytes to reset a claimed slot
 * without clobbering the `in_use` flag it just won.  Keep `in_use` LAST, and
 * add new fields before it.
 *
 * `lifecycle` and `active_ops` carry the open/invoke/close state machine and
 * the count of in-flight operations (issues #1115, #629); they sit between
 * `be_state` and `in_use` precisely as the rule above requires.
 */
struct alp_inference {
	alp_inference_backend_t backend;
	void                   *be_state;
	uint8_t                 lifecycle;
	uint32_t                active_ops;
	/* Last successful alp_inference_invoke()'s wall-clock duration, in
	 * microseconds -- written by inference_yocto.c's dispatcher (it
	 * brackets the per-backend invoke switch), never by a backend.
	 * UINT64_MAX means "no successful invoke yet"; the sentinel is set
	 * explicitly in alp_inference_open() below (pool_acquire()'s
	 * claim-time memset only zeroes it). Same atomic-access discipline
	 * as src/backends/inference/inference_ops.h's twin field: this is
	 * an op-counted field (active_ops permits concurrent ops on one
	 * handle -- it's a drain counter, not a mutex), so every access is
	 * __atomic_store_n/__atomic_load_n, never a plain read/write. */
	uint64_t last_invoke_latency_us;
	bool     in_use;
};

/* The other half of the invariant the comment above describes: `be_state`
 * must sit immediately after `backend`.  A comment cannot enforce that, so
 * pin it (issue #1257). */
#if defined(__cplusplus)
static_assert(offsetof(struct alp_inference, be_state) == sizeof(void *),
              "alp_inference: be_state must immediately follow backend "
              "(pointer-aligned) -- see issue #1257");
#else
_Static_assert(offsetof(struct alp_inference, be_state) == sizeof(void *),
               "alp_inference: be_state must immediately follow backend "
               "(pointer-aligned) -- see issue #1257");
#endif

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_SRC_YOCTO_INFERENCE_HANDLE_INTERNAL_H_ */
