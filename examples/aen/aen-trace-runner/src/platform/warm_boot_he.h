/* src/platform/warm_boot_he.h -- is this HE boot a WARM one: the HE alone was reset while the HP
 * kept running (the I2C1 handover's verdict, valid once the kernel is up; zephyr/soc-bridge/alif/
 * i2c_handover.h). Without the handover node (no TR_INPUT_NPU shield) there is no verdict and
 * every boot is treated as cold. Used to keep the levels the user set (volume_he.c). */
#ifndef TR_PLATFORM_WARM_BOOT_HE_H
#define TR_PLATFORM_WARM_BOOT_HE_H

#include <stdbool.h>

#ifdef CONFIG_ALIF_I2C_HANDOVER
#include "../../../../../zephyr/soc-bridge/alif/i2c_handover.h"

static inline bool tr_warm_boot(void)
{
	return alp_i2c_handover_warm_boot();
}
#else
static inline bool tr_warm_boot(void)
{
	return false;
}
#endif

#endif /* TR_PLATFORM_WARM_BOOT_HE_H */
