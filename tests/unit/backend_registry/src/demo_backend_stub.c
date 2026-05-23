/* SPDX-License-Identifier: Apache-2.0
 *
 * @brief Stub: demo backend for a fictional silicon.
 * @par Implementation status: NOT_IMPLEMENTED (planned: never -- test fixture)
 * @par Tracking: github.com/alplabai/alp-sdk/issues/0
 */

#include "demo_class.h"

/* No ops -> demo_open() returns ALP_ERR_NOT_IMPLEMENTED for this silicon. */

ALP_BACKEND_REGISTER(demo, stub_target, {
    .silicon_ref = "fictional:stub:target",
    .vendor      = "fictional",
    .base_caps   = 0u,
    .priority    = 50,
    .ops         = NULL,
    .probe       = NULL,
});
