/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "alp/peripheral.h"
#include "update_log/boot_metadata.h"

#if defined(__GNUC__) || defined(__clang__)
#define ALP_WEAK __attribute__((weak))
#else
#define ALP_WEAK
#endif

ALP_WEAK alp_status_t alp_update_log_boot_metadata_read(alp_update_log_entry_t *entry_out)
{
	if (entry_out == NULL) {
		return ALP_ERR_INVAL;
	}

	memset(entry_out, 0, sizeof(*entry_out));
	return ALP_ERR_NOSUPPORT;
}
