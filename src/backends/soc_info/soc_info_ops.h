/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the soc_info dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never sees
 * this struct.  Layout may change between SDK versions.
 *
 * The surface is handle-less (TMU pattern): the dispatcher zero-fills
 * the out-struct, stamps soc_ref from the build-time ALP_SOC_REF_STR,
 * and hands it to the selected backend to fill the runtime fields.
 */

#ifndef ALP_BACKENDS_SOC_INFO_OPS_H
#define ALP_BACKENDS_SOC_INFO_OPS_H

#include <alp/hw_info.h>
#include <alp/peripheral.h>

/** Vtable each soc_info backend implements. */
typedef struct alp_soc_info_ops {
	/** Fill the runtime identity fields.  @p out arrives zero-filled
	 *  with soc_ref already stamped; the backend fills what it can
	 *  and returns the first failure it hit (already-filled fields
	 *  stay valid). */
	alp_status_t (*read)(alp_soc_info_t *out);
	/** Bounded liveness ping of the identity source. */
	alp_status_t (*ping)(void);
} alp_soc_info_ops_t;

#endif /* ALP_BACKENDS_SOC_INFO_OPS_H */
