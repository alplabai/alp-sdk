/* SPDX-License-Identifier: Apache-2.0 -- host stub: k_msleep/k_uptime_get on
 * pthreads. usleep() is a cancellation point, which is how the test "resets"
 * the owner core. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
static inline int64_t k_uptime_get(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static inline int32_t k_msleep(int32_t ms)
{
	usleep((useconds_t)ms * 1000U);
	return 0;
}
