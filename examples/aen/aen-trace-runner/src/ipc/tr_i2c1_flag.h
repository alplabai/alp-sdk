/* src/ipc/tr_i2c1_flag.h -- the HE -> HP "I2C1 is free" handshake of the
 * RVT121 full game (TR_PANEL=rvt121 + TR_INPUT_NPU).
 *
 * I2C1 (0x49011000, P3_7 SCL / P7_2 SDA) carries the camera SCCB, which the
 * HP owns under TR_INPUT_NPU, and, on the RVT121, the SN65DSI83 bridge the HE
 * configures once at boot. The HE writes TR_I2C1_FREE_MAGIC to TR_MEM_I2C1_FREE
 * (tr_memmap.h: the reserved block next to TR_MEM_SRAM1_READY, SRAM0, always
 * on) after it has finished with the bus and stopped its controller; the HP
 * touches the bus (pad unstick, i2c driver, camera sensor) only after reading
 * it. The HE clears the word first thing at every boot, so a previous boot's
 * magic never counts. RK055 builds neither write nor wait.
 *
 * Pure logic, host-tested (tests/host/test_i2c1_flag.c).
 */
#ifndef TR_I2C1_FLAG_H
#define TR_I2C1_FLAG_H

#include <stdbool.h>
#include <stdint.h>

#define TR_I2C1_FREE_MAGIC 0x31433249u /* 'I2C1' */

/* HP: poll `rd` for the magic every `step_ms` for at most `limit_ms`
 * (clock `now_ms`, nap `nap_ms`). True once it reads as the magic, false when
 * the limit passes without it -- the caller reports and may call again; it
 * never touches the bus in between. */
static inline bool tr_i2c1_wait_free(uint32_t (*rd)(void *),
                                     int64_t (*now_ms)(void *),
                                     void (*nap_ms)(void *, uint32_t),
                                     void    *ctx,
                                     uint32_t step_ms,
                                     uint32_t limit_ms)
{
	int64_t t0 = now_ms(ctx);

	for (;;) {
		if (rd(ctx) == TR_I2C1_FREE_MAGIC) {
			return true;
		}
		if (now_ms(ctx) - t0 >= (int64_t)limit_ms) {
			return false;
		}
		nap_ms(ctx, step_ms);
	}
}

#endif /* TR_I2C1_FLAG_H */
