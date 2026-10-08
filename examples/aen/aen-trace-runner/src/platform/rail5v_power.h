/* src/platform/rail5v_power.h */
#ifndef TR_PLATFORM_RAIL5V_POWER_H
#define TR_PLATFORM_RAIL5V_POWER_H

#include <stdint.h>

#include "../hud/hud.h" /* TR_PWR_N */

/* The carrier's downstream +5V net, opened on the shared sensor I2C bus
 * (EVK_I2C_BUS_SENSORS -- the same bus platform/imu.c's BMI323 already
 * uses; a second alp_i2c_open() on one bus_id is supported -- see
 * src/i2c_dispatch.c's handle pool). Non-fatal: a failed open or every
 * later sample just leaves tr_rail5v_avg_mw at 0, same as a missing IMU
 * leaves tilt input neutral. See rail5v_power.c's header comment for what
 * this rail does and does not cover. */
int tr_rail5v_open(void);

/* Call every tick; internally paced to ~10 Hz (RAIL5V_PERIOD_MS in the
 * .c), so this is cheap to call from the hot path -- most calls are a single
 * timestamp compare and return. Off ticks: no I2C traffic, so it never
 * contends with platform/imu.c's BMI323 polling on the shared bus. */
void tr_rail5v_poll(void);

/* Bench + HUD readable: an EMA of the U30-monitored +5V net's power, mW.
 * 0 before the first successful sample or if the rail was never opened. */
extern volatile int32_t tr_rail5v_avg_mw;

/* Bench readable (ELF symbol): the INA236 CONFIG readback, [15:0] the last
 * CONFIG register value read, [23:16] times it was found reverted and
 * rewritten since boot (saturating), [31] set once the open-time
 * write + readback verified TR_INA236_CONFIG (ina236_math.h). Healthy:
 * 0x80004927 (reserved bits 14:13 read 10b). */
extern volatile uint32_t tr_rail5v_config_rb;

/* The graph's samples (hud.h TR_PWR_N, oldest first, TR_PWR_GAP where none could be taken) into
 * out; returns the number of samples pushed so far. Same thread as tr_rail5v_poll(). */
uint32_t tr_rail5v_ring_read(int16_t out[TR_PWR_N]);

#endif /* TR_PLATFORM_RAIL5V_POWER_H */
