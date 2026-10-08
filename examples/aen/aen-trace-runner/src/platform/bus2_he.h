/* src/platform/bus2_he.h -- the HE's side of the I2C2 + GPIO5 lease to the HP's game sound
 * (TR_HP_SOUND builds; protocol and fail-safe rules: src/ipc/tr_bus2.h). Without TR_HP_SOUND
 * the HE owns the bus for good and both calls compile to nothing. */
#ifndef TR_PLATFORM_BUS2_HE_H
#define TR_PLATFORM_BUS2_HE_H

#include <stdbool.h>

#ifndef TR_HP_SOUND
#define TR_HP_SOUND 0
#endif

#if TR_HP_SOUND
/* Once per game frame (main loop, never an ISR): offers I2C2 to an HP that asks for it, takes it
 * back when the HP returns it (or restarted / gave up). */
void tr_bus2_he_frame(void);

/* True while this core may use I2C2 (BMI323, the +5V INA236). False while the HP holds the bus:
 * every I2C2 user must skip its transfer, keep its last value and mark it stale. */
bool tr_bus2_he_owns(void);
#else
static inline void tr_bus2_he_frame(void)
{
}

static inline bool tr_bus2_he_owns(void)
{
	return true;
}
#endif

#endif /* TR_PLATFORM_BUS2_HE_H */
