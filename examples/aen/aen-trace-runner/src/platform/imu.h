/* src/platform/imu.h */
#ifndef TR_PLATFORM_IMU_H
#define TR_PLATFORM_IMU_H

#include <stdint.h>

int tr_imu_open(void);
/* Rest-zeroed accel sample, Q8 g (256 == 1 g): x = steer, y = pitch. Always
 * writes both; (0, 0) = level when there is no IMU. Interpreted by
 * game/tilt.c. */
void tr_imu_read_q8(int16_t *x_q8, int16_t *y_q8);

#endif /* TR_PLATFORM_IMU_H */
