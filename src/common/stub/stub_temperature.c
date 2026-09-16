/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/temperature.h> NOSUPPORT stub -- baremetal / Yocto.
 *
 * The real implementation (src/zephyr/temperature_zephyr.c) binds the
 * on-module sensor through the upstream ZEPHYR sensor API against a
 * metadata-emitted DT alias -- there is no portable, OS-agnostic way to
 * express that on a build with no devicetree at all, so unlike
 * <alp/hw_info.h> (whose EEPROM reader is built from already-portable
 * alp_i2c primitives and so is genuinely one shared translation unit)
 * this class needs a real per-OS split.  This stub is that split's
 * baremetal/Yocto half; see issue #2066.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "alp/temperature.h"

alp_status_t alp_temperature_read_milli_c(int32_t *milli_c)
{
	if (milli_c == NULL) return ALP_ERR_INVAL;
	return ALP_ERR_NOSUPPORT;
}
