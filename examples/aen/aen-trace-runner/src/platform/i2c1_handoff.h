/* src/platform/i2c1_handoff.h -- TR_PANEL=rvt121 + TR_INPUT_NPU: the HE gives
 * I2C1 to the HP once the SN65DSI83 bridge is configured. See ipc/tr_i2c1_flag.h. */
#ifndef TR_I2C1_HANDOFF_H
#define TR_I2C1_HANDOFF_H

#include <stdbool.h>

/* Stop this core's I2C1 controller and its IRQ, then publish TR_I2C1_FREE_MAGIC.
 * True when published; false (nothing published, the HP keeps waiting) when the
 * controller would not go idle. The bridge is configured once, by its driver at
 * boot; nothing on the HE uses I2C1 afterwards. */
bool tr_i2c1_release(void);

#endif
