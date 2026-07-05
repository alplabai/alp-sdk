/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ALP_UPDATE_LOG_BOOT_METADATA_H
#define ALP_UPDATE_LOG_BOOT_METADATA_H

#include "alp/update_log.h"

/* Internal provider seam for authenticated boot metadata. Platform code
 * overrides this weak default once MCUboot shared data / Alif SE verification
 * facts are available in the running image. */
alp_status_t alp_update_log_boot_metadata_read(alp_update_log_entry_t *entry_out);

#endif /* ALP_UPDATE_LOG_BOOT_METADATA_H */
