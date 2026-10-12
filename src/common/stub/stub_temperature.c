/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/temperature.h> NOSUPPORT stub -- baremetal.
 *
 * The real implementation (src/zephyr/temperature_zephyr.c) binds the
 * on-module sensor through the upstream ZEPHYR sensor API against a
 * metadata-emitted DT alias -- there is no portable, OS-agnostic way to
 * express that on a build with no devicetree at all, so unlike
 * <alp/hw_info.h> (whose EEPROM reader is built from already-portable
 * alp_i2c primitives and so is genuinely one shared translation unit)
 * this class needs a real per-OS split.  This stub is that split's
 * baremetal half (Yocto has its own
 * src/yocto/temperature_yocto.c with a real SoC-die read; the Zephyr die read is `die-temp0`); see issue #2066.
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

alp_status_t alp_temperature_read_die_milli_c(int32_t *milli_c)
{
	if (milli_c == NULL) return ALP_ERR_INVAL;
	return ALP_ERR_NOSUPPORT;
}
