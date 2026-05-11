/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-backend internal helpers for the ALP SDK.  Not part of the
 * public surface -- application code must use the <alp/...> public
 * headers instead.
 *
 * Lives in src/common/ so every backend (baremetal, yocto, and any
 * non-Zephyr build that picks up stub_backend.c) shares one
 * canonical implementation.  Zephyr has its own thread-local
 * last-error in src/zephyr/last_error.c; this header is irrelevant
 * to Zephyr builds.
 */

#ifndef ALP_INTERNAL_H_
#define ALP_INTERNAL_H_

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stamp the process-wide last-error slot read by
 *        `alp_last_error`.
 *
 * Use from any backend source file that needs to convey a precise
 * failure reason before returning NULL or a non-OK status.  The
 * setter writes to the same static that
 * `src/common/stub_backend.c`'s peripheral stubs write to, so a
 * caller invoking `alp_last_error()` after a failure sees the
 * latest write regardless of which layer stamped it.
 *
 * On `ALP_VENDOR_OVERRIDES_PERIPHERAL=1` builds (vendor wrappers
 * such as `vendors/alif/` take over the peripheral surface), the
 * vendor's own `alp_last_error` reads from a different static --
 * cross-TU error correlation isn't guaranteed there.  Vendor
 * builds that want full last-error fidelity should call this
 * setter *and* the vendor's local equivalent.
 */
void alp_internal_set_last_error(alp_status_t s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_INTERNAL_H_ */
