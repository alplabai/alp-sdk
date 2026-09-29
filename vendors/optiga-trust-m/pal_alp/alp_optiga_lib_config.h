/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-library configuration for alp-sdk, selected with
 * -DOPTIGA_LIB_EXTERNAL="alp_optiga_lib_config.h".  Upstream's Trust M
 * V3 defaults, minus the Shielded Connection: that needs a platform
 * binding secret provisioned into the chip (an irreversible write), so
 * it stays off until provisioning is designed (#1164).
 */
#ifndef ALP_OPTIGA_LIB_CONFIG_H
#define ALP_OPTIGA_LIB_CONFIG_H

#include "optiga_lib_config_m_v3.h"

#undef OPTIGA_COMMS_SHIELDED_CONNECTION

#endif /* ALP_OPTIGA_LIB_CONFIG_H */
