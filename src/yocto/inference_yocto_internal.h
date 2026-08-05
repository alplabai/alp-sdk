/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal handle layout shared between the Yocto inference dispatcher
 * (inference_yocto.c) and its per-backend hooks (inference_ort.cpp,
 * inference_deepx.cpp, inference_drpai.cpp).  NOT a public header --
 * <alp/inference.h> exposes alp_inference_t only as an opaque forward
 * declaration, so customer code never sees this struct.  Layout may
 * change between SDK versions.
 *
 * issue #1257: this header replaces three hand-mirrored
 * `alp_inference_handle_layout` copies (one per backend .cpp) that each
 * re-derived this same layout by eye, purely to reach be_state.  Those
 * mirrors were correct only on LP64, by pointer-width coincidence: the
 * real layout put be_state at offset 8 and the mirrored
 * {in_use, backend, be_state} also landed it at 8 because pointers are
 * 8 bytes there; on an ILP32 target the real struct puts be_state at 4
 * while the mirror still puts it at 8, so a write through the mirror
 * would silently corrupt an adjacent field.  One definition, included
 * everywhere it's needed, makes the compiler the enforcer instead of a
 * comment.
 */

#ifndef ALP_YOCTO_INFERENCE_INTERNAL_H
#define ALP_YOCTO_INFERENCE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/inference.h"

/*
 * in_use MUST stay the LAST member: pool_acquire() in inference_yocto.c
 * does memset(h, 0, offsetof(struct alp_inference, in_use)) right after
 * winning the atomic claim on in_use (alp_slot_try_claim), so the claim
 * itself must never fall inside the zeroed range.
 *
 * lifecycle/active_ops MUST stay BEFORE in_use, so that same memset
 * resets them on every fresh claim -- they drive the generic
 * open/op/close guard in src/common/alp_slot_claim.h
 * (alp_handle_op_enter/leave/begin_close_blocking, issue #629).
 *
 * These two constraints were worked out when the use-after-free fix
 * landed (issue #1115 round-2 dev review); do not undo them. Reorder
 * or resize only after re-checking both AND the static_assert below.
 */
struct alp_inference {
	alp_inference_backend_t backend;
	void                   *be_state;
	uint8_t                 lifecycle;
	uint32_t                active_ops;
	bool                    in_use;
};

/*
 * Pins the one thing every backend hook actually depends on: be_state
 * sits immediately after backend, padded up to pointer alignment --
 * i.e. at an offset equal to sizeof(void *) on both LP64 (backend's 4
 * bytes pad out to an 8-byte offset) and ILP32 (backend's 4 bytes need
 * no padding to reach a 4-byte offset). A future field reorder/insert
 * ahead of be_state fails HERE, at compile time, instead of corrupting
 * a handle at runtime (issue #1257).
 */
#if defined(__cplusplus)
static_assert(offsetof(struct alp_inference, be_state) == sizeof(void *),
              "alp_inference: be_state must immediately follow backend "
              "(pointer-aligned) -- see issue #1257");
#else
_Static_assert(offsetof(struct alp_inference, be_state) == sizeof(void *),
               "alp_inference: be_state must immediately follow backend "
               "(pointer-aligned) -- see issue #1257");
#endif

#endif /* ALP_YOCTO_INFERENCE_INTERNAL_H */
