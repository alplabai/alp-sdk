/* src/audio/tr_amp_ready.h -- wait until a TAS2563 answers on I2C after SD_N (P5_2) is released,
 * before tas2563_init() (sound/src/main.c steps 4..6; the standalone GAME and TEST images and the
 * combined HP image share it).
 *
 * WHY. Bench 2026-09-30 (E1M-AEN803 2026W36-0002, 2 cold boots, identical): after SD_N rises, U27
 * (0x4D) NACKs its address (IC_TX_ABRT_SOURCE 0x1, ADDR7BNACK) at 313 us and first ACKs at 1142 us;
 * U28 (0x4E) ACKs at 390 us. The SDK's TAS2563_RESET_SETTLE_US is 200 us, so tas2563_init(0x4d) hit
 * the NACK window (-5).
 *
 * So: a settle of TR_SND_AMP_SETTLE_US (2000 us, >= 1.75 x the 1142 us measured), then a bounded
 * poll -- a 1-byte read every TR_SND_AMP_POLL_US until it ACKs, at most TR_SND_AMP_POLL_MAX_US in
 * total -- per amp. A timeout is a bring-up failure (teardown, no sound).
 *
 * Pure C: the probe, the sleep and the clock are the caller's (tests/host/test_amp_ready.c).
 */
#ifndef TR_AMP_READY_H
#define TR_AMP_READY_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TR_SND_AMP_SETTLE_US
#define TR_SND_AMP_SETTLE_US 2000u /* measured: U27 ACKs from 1142 us after SD_N (313 us: NACK) */
#endif
#define TR_SND_AMP_ACK_MEASURED_US 1142u /* the latest first-ACK seen (U27) */
#define TR_SND_AMP_POLL_US         1000u
#define TR_SND_AMP_POLL_MAX_US     50000u
_Static_assert(TR_SND_AMP_SETTLE_US >= TR_SND_AMP_ACK_MEASURED_US,
               "the amp settle must cover the measured first-ACK time of U27 (1142 us)");

typedef struct {
	bool (*ack)(void *ctx, uint8_t addr); /* one 1-byte read: true = the address ACKed */
	void (*sleep_us)(uint32_t us);
	uint64_t (*now_us)(void);
	void *ctx;
} tr_amp_io_t;

/* After the settle: poll `addr` until it ACKs (true) or poll_max_us passed (false). *tries = the
 * reads made, *waited_us = from the first read to the answer (or the give-up). */
bool tr_amp_wait_ack(const tr_amp_io_t *io,
                     uint8_t            addr,
                     uint32_t           poll_us,
                     uint32_t           poll_max_us,
                     uint32_t          *tries,
                     uint32_t          *waited_us);

#endif /* TR_AMP_READY_H */
