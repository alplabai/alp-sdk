/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble CRC engine driver (compatible "alif,crc"), implementing the
 * upstream Zephyr v4.4 CRC class API (<zephyr/drivers/crc.h>:
 * crc_begin / crc_update / crc_finish over struct crc_ctx).
 *
 * CLEAN-ROOM, NOT vendored.  The Apache-2.0 zephyr_alif fork ships a CRC driver
 * (drivers/crc/) but it targets a DIFFERENT, fork-private API (crc_compute() +
 * struct crc_params + crc_set_seed(), Kconfig CONFIG_CRC_DRV) that does NOT
 * exist in pinned upstream Zephyr v4.4 -- so it cannot be consumed verbatim and
 * cannot be retired onto.  This driver is therefore re-authored from scratch
 * against the upstream class API.  Every register offset / bit / base address is
 * transcribed (clean-room, value-only) from the proprietary Alif DFP register
 * header drivers/include/crc.h + the SoC header (see the citations inline);
 * the programming SEQUENCE is cross-checked against the fork's CMSIS reference
 * driver Driver_CRC.c.  ADR 0017 Tier-1.5 (in-tree-thin over the silicon).
 *
 * The engine is a POLLED memory-mapped block -- no interrupt, no DMA (the DFP
 * exposes an optional DMA path; this driver uses the simple register feed).
 *
 * Mapping of the upstream enum crc_type onto the Alif hardware algorithms
 * (drivers/include/crc.h algorithm selects):
 *   CRC8_CCITT  -> CRC_8_CCITT     (8-bit, poly 0x07,       seed 0xFF)
 *   CRC16       -> CRC_16          (16-bit, poly 0x8005)
 *   CRC16_CCITT -> CRC_16_CCITT    (16-bit, poly 0x1021)
 *   CRC32_IEEE  -> CRC_32          (32-bit, poly 0x04C11DB7)
 *   CRC32_C     -> CRC_32C custom  (32-bit, poly 0x1EDC6F41 via CRC_POLY_CUSTOM)
 * Other enum crc_type values have no hardware algorithm here -> -ENOTSUP.
 *
 * vendor-ext, BENCH-UNVERIFIED.
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(crc_alif, CONFIG_CRC_DRIVER_LOG_LEVEL);

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crc.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/byteorder.h>

#define DT_DRV_COMPAT alif_crc

/*
 * --- CRC register map ----------------------------------------------------
 * Offsets transcribed from the Alif DFP CRC_Type struct
 * (alif-dfp-ref/Device/soc/AE822FA0E5597/include/rtss_he/soc.h, the
 * "(@ 0x48107000) CRC0 Structure" typedef) -- value-only, clean-room.
 */
#define CRC_CTRL_OFFSET        0x00U /* CRC_CONTROL  (@ 0x00) DFP soc.h    */
#define CRC_SEED_OFFSET        0x10U /* CRC_SEED     (@ 0x10) DFP soc.h    */
#define CRC_POLY_CUSTOM_OFFSET 0x14U /* CRC_POLY_CUSTOM (@ 0x14) DFP soc.h */
#define CRC_OUT_OFFSET         0x18U /* CRC_OUT      (@ 0x18) DFP soc.h    */
/* 8-bit / 32-bit data-input register offsets, from DFP crc.h: */
#define CRC_DATA_IN_8BIT_OFFSET  0x20U /* CRC_DATA_IN_8BIT_REG_OFFSET  (DFP crc.h) */
#define CRC_DATA_IN_32BIT_OFFSET 0x60U /* CRC_DATA_IN_32BIT_REG_OFFSET (DFP crc.h) */

/*
 * --- CRC_CONTROL register bits -------------------------------------------
 * Transcribed value-only from the Alif DFP crc.h #defines (clean-room).
 */
#define CRC_CTRL_REFLECT       (1U << 11) /* CRC_REFLECT       DFP crc.h */
#define CRC_CTRL_INVERT        (1U << 10) /* CRC_INVERT        DFP crc.h */
#define CRC_CTRL_CUSTOM_POLY   (1U << 9)  /* CRC_CUSTOM_POLY   DFP crc.h */
#define CRC_CTRL_BIT_SWAP      (1U << 8)  /* CRC_BIT_SWAP      DFP crc.h */
#define CRC_CTRL_BYTE_SWAP     (1U << 7)  /* CRC_BYTE_SWAP     DFP crc.h */
#define CRC_CTRL_ALGO_8_CCITT  (0U << 3)  /* CRC_8_CCITT       DFP crc.h */
#define CRC_CTRL_ALGO_16       (2U << 3)  /* CRC_16            DFP crc.h */
#define CRC_CTRL_ALGO_16_CCITT (3U << 3)  /* CRC_16_CCITT      DFP crc.h */
#define CRC_CTRL_ALGO_32       (4U << 3)  /* CRC_32            DFP crc.h */
#define CRC_CTRL_ALGO_32C      (5U << 3)  /* CRC_32C           DFP crc.h */
#define CRC_CTRL_SIZE_8        (0U << 1)  /* CRC_ALGO_8_BIT_SIZE  DFP crc.h */
#define CRC_CTRL_SIZE_16       (1U << 1)  /* CRC_ALGO_16_BIT_SIZE DFP crc.h */
#define CRC_CTRL_SIZE_32       (2U << 1)  /* CRC_ALGO_32_BIT_SIZE DFP crc.h */
#define CRC_CTRL_INIT_BIT      (1U << 0)  /* CRC_INIT_BIT      DFP crc.h */

/*
 * Standard 32-bit CRC polynomial, transcribed value-only from the DFP
 * crc.h CRC_STANDARD_POLY (also the upstream CRC32_IEEE_POLY 0x04C11DB7).
 * The CRC32C polynomial (0x1EDC6F41) is the upstream CRC32C_POLY; the
 * engine's CRC_32C select drives it through the CRC_POLY_CUSTOM register.
 */
#define CRC_STANDARD_POLY 0x04C11DB7U /* CRC_STANDARD_POLY DFP crc.h */

struct crc_alif_config {
	/* CRC engine register block base (DT reg, e.g. 0x48107000 = CRC0). */
	uintptr_t base;
};

struct crc_alif_data {
	/* Serialises begin..finish so two threads cannot interleave on the
	 * single hardware engine. */
	struct k_sem lock;
};

static inline void crc_alif_write(const struct crc_alif_config *cfg, uint32_t off, uint32_t val)
{
	sys_write32(val, cfg->base + off);
}

static inline uint32_t crc_alif_read(const struct crc_alif_config *cfg, uint32_t off)
{
	return sys_read32(cfg->base + off);
}

/*
 * Translate an upstream enum crc_type into the Alif CRC_CONTROL algorithm +
 * size select and validate the caller-supplied polynomial.  The data-input
 * width (byte-fed vs. word-fed) is derived from ctx->type in crc_alif_update().
 *
 * Returns 0 on success, -ENOTSUP for an unsupported type, -EINVAL on a
 * polynomial mismatch.
 */
static int crc_alif_select_algo(const struct crc_ctx *ctx, uint32_t *ctrl)
{
	switch (ctx->type) {
	case CRC8_CCITT:
		if (ctx->polynomial != CRC8_POLY) { /* CRC8_POLY 0x07, zephyr/sys/crc.h */
			return -EINVAL;
		}
		*ctrl = CRC_CTRL_ALGO_8_CCITT | CRC_CTRL_SIZE_8;
		return 0;
	case CRC16:
		if (ctx->polynomial != CRC16_POLY) { /* CRC16_POLY 0x8005 */
			return -EINVAL;
		}
		*ctrl = CRC_CTRL_ALGO_16 | CRC_CTRL_SIZE_16;
		return 0;
	case CRC16_CCITT:
		if (ctx->polynomial != CRC16_CCITT_POLY) { /* CRC16_CCITT_POLY 0x1021 */
			return -EINVAL;
		}
		*ctrl = CRC_CTRL_ALGO_16_CCITT | CRC_CTRL_SIZE_16;
		return 0;
	case CRC32_IEEE:
		if (ctx->polynomial != CRC32_IEEE_POLY) { /* CRC32_IEEE_POLY 0x04C11DB7 */
			return -EINVAL;
		}
		*ctrl = CRC_CTRL_ALGO_32 | CRC_CTRL_SIZE_32;
		return 0;
	case CRC32_C:
		if (ctx->polynomial != CRC32C_POLY) { /* CRC32C_POLY 0x1EDC6F41 */
			return -EINVAL;
		}
		/* CRC_32C drives the polynomial from CRC_POLY_CUSTOM. */
		*ctrl = CRC_CTRL_ALGO_32C | CRC_CTRL_SIZE_32 | CRC_CTRL_CUSTOM_POLY;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int crc_alif_begin(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_alif_config *cfg  = dev->config;
	struct crc_alif_data         *data = dev->data;
	uint32_t                      ctrl = 0U;
	int                           ret;

	if (ctx == NULL) {
		return -EINVAL;
	}

	ret = crc_alif_select_algo(ctx, &ctrl);
	if (ret != 0) {
		return ret;
	}

	/*
	 * Map the upstream class-API reverse flags onto the Alif CRC_CONTROL
	 * swap/reflect/invert bits.
	 *
	 * The Alif engine is an MSB-first CRC core: it shifts the accumulator
	 * LEFT and XORs with the NORMAL polynomial (0x04C11DB7 for CRC_32).  The
	 * canonical CRC-32/IEEE and CRC-32C are REFLECTED (LSB-first) algorithms
	 * with a final one's-complement (XorOut = 0xFFFFFFFF).  To make the
	 * MSB-first core emit the reflected result the silicon provides three
	 * post/pre-processing bits that the fork couples together for the
	 * "full-mode" reflected 32-bit CRC:
	 *
	 *   - REVERSE_INPUT  -> BYTE_SWAP | BIT_SWAP : reflect the INPUT word
	 *       stream (byte order within the 32-bit word + bit order within
	 *       each byte) so the MSB-first engine consumes the data as if it
	 *       were fed LSB-first.
	 *   - REVERSE_OUTPUT -> REFLECT             : reflect the final CRC_OUT.
	 *
	 * Source for the bit set: the Alif DFP bare-metal demo programs the
	 * standard 32-bit CRC with
	 *   ARM_CRC_ENABLE_BIT_SWAP | ARM_CRC_ENABLE_BYTE_SWAP |
	 *   ARM_CRC_ENABLE_INVERT_OUTPUT | ARM_CRC_ENABLE_REFLECT_OUTPUT
	 * (alif-dfp-ref/Boards/Templates/Baremetal/demo_crc.c, CRC_32_BIT case),
	 * and the fork's HW/SW conformance test for canonical CRC-32 sets
	 *   .bit_swap = .byte_swap = .reflect = .invert = CRC_TRUE, seed
	 *   0xFFFFFFFF (sdk-alif-ref/tests/drivers/crc/src/test_crc_32.c,
	 *   crc32_full_reference()).  The earlier "REFLECT-only" mapping left
	 *   the input bit/byte order wrong AND omitted the final complement,
	 *   which is why no plain refl/xor transform of the raw CRC_OUT could
	 *   reach the zlib reference.
	 */
	if (ctx->reversed & CRC_FLAG_REVERSE_INPUT) {
		ctrl |= CRC_CTRL_BYTE_SWAP | CRC_CTRL_BIT_SWAP;
	}
	if (ctx->reversed & CRC_FLAG_REVERSE_OUTPUT) {
		ctrl |= CRC_CTRL_REFLECT;

		/*
		 * The class API exposes no separate XorOut flag, but the
		 * reflected 32-bit CRCs (CRC-32/IEEE and CRC-32C) are DEFINED
		 * with a final one's-complement.  The DFP demo + fork test
		 * always pair INVERT with REFLECT for these, so couple the
		 * engine's CRC_INVERT bit to the output-reverse request for the
		 * 32-bit reflected types.  (8/16-bit CCITT-family algorithms
		 * stay non-inverted: their canonical form is reflect-only, per
		 * the fork's 8/16-bit reflect tests which set invert = FALSE.)
		 */
		if (ctx->type == CRC32_IEEE || ctx->type == CRC32_C) {
			ctrl |= CRC_CTRL_INVERT;
		}
	}

	k_sem_take(&data->lock, K_FOREVER);

	/* Programming sequence cross-checked vs the fork CMSIS Driver_CRC.c:
	 *   1. clear CRC_CONTROL,
	 *   2. write the algorithm + size + option bits,
	 *   3. for CRC_32C, load the custom polynomial,
	 *   4. write the seed,
	 *   5. set CRC_INIT_BIT to latch seed (+ poly) into the engine. */
	crc_alif_write(cfg, CRC_CTRL_OFFSET, 0U);
	crc_alif_write(cfg, CRC_CTRL_OFFSET, ctrl);

	if (ctx->type == CRC32_C) {
		crc_alif_write(cfg, CRC_POLY_CUSTOM_OFFSET, ctx->polynomial);
	}

	crc_alif_write(cfg, CRC_SEED_OFFSET, ctx->seed);
	crc_alif_write(cfg, CRC_CTRL_OFFSET, ctrl | CRC_CTRL_INIT_BIT);

	ctx->result = 0U;
	ctx->state  = CRC_STATE_IN_PROGRESS;

	return 0;
}

static int
crc_alif_update(const struct device *dev, struct crc_ctx *ctx, const void *buffer, size_t bufsize)
{
	const struct crc_alif_config *cfg = dev->config;

	if (ctx == NULL || (buffer == NULL && bufsize != 0U)) {
		return -EINVAL;
	}

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	if (ctx->type == CRC32_IEEE || ctx->type == CRC32_C) {
		/*
		 * The 32-bit data-input register consumes whole 32-bit words.
		 * Feed the NATURAL little-endian word (alignment-safe load via
		 * sys_get_le32) -- on the little-endian M55 this is the same
		 * value the DFP crc_calculate_32bit() pushes as *(uint32_t *)
		 * (alif-dfp-ref/drivers/source/crc.c).  Any input bit/byte
		 * reflection needed for canonical CRC-32 is done in hardware by
		 * the CRC_BYTE_SWAP | CRC_BIT_SWAP bits set in crc_alif_begin()
		 * (from CRC_FLAG_REVERSE_INPUT); the word fed here must stay the
		 * raw LE word so the engine's swap operates on the real bytes.
		 */
		const uint8_t *p     = buffer;
		size_t         words = bufsize / 4U;

		if ((bufsize % 4U) != 0U) {
			return -ENOTSUP;
		}

		for (size_t i = 0; i < words; i++) {
			uint32_t w = sys_get_le32(&p[i * 4U]);

			crc_alif_write(cfg, CRC_DATA_IN_32BIT_OFFSET, w);
		}
	} else {
		/* 8/16-bit algorithms feed one byte at a time. */
		const uint8_t *p = buffer;

		for (size_t i = 0; i < bufsize; i++) {
			crc_alif_write(cfg, CRC_DATA_IN_8BIT_OFFSET, p[i]);
		}
	}

	/* Latch the running accumulator after each chunk so a caller doing
	 * crc_update() then crc_verify() (without crc_finish()) still sees the
	 * current value; crc_finish() re-reads it. */
	ctx->result = crc_alif_read(cfg, CRC_OUT_OFFSET);

	return 0;
}

static int crc_alif_finish(const struct device *dev, struct crc_ctx *ctx)
{
	const struct crc_alif_config *cfg  = dev->config;
	struct crc_alif_data         *data = dev->data;

	if (ctx == NULL) {
		return -EINVAL;
	}

	if (ctx->state != CRC_STATE_IN_PROGRESS) {
		return -EINVAL;
	}

	ctx->result = crc_alif_read(cfg, CRC_OUT_OFFSET);
	ctx->state  = CRC_STATE_IDLE;

	k_sem_give(&data->lock);

	return 0;
}

static int crc_alif_init(const struct device *dev)
{
	struct crc_alif_data *data = dev->data;

	k_sem_init(&data->lock, 1, 1);

	return 0;
}

static DEVICE_API(crc, crc_alif_driver_api) = {
	.begin  = crc_alif_begin,
	.update = crc_alif_update,
	.finish = crc_alif_finish,
};

#define CRC_ALIF_INIT(idx)                                                                         \
	static const struct crc_alif_config crc_alif_config_##idx = {                                  \
		.base = DT_INST_REG_ADDR(idx),                                                             \
	};                                                                                             \
	static struct crc_alif_data crc_alif_data_##idx;                                               \
	DEVICE_DT_INST_DEFINE(idx,                                                                     \
	                      crc_alif_init,                                                           \
	                      NULL,                                                                    \
	                      &crc_alif_data_##idx,                                                    \
	                      &crc_alif_config_##idx,                                                  \
	                      POST_KERNEL,                                                             \
	                      CONFIG_CRC_DRIVER_INIT_PRIORITY,                                         \
	                      &crc_alif_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CRC_ALIF_INIT)
