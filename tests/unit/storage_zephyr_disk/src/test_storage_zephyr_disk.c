/* SPDX-License-Identifier: Apache-2.0
 *
 * Exercises the zephyr_disk SD_MMC storage backend against Zephyr's RAM
 * disk (disk-name "RAM", 16 x 512 B), reached via the open dispatch's
 * decline-and-fall-through selection (sw_fallback is also linked).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/storage.h>

ZTEST_SUITE(alp_storage_zephyr_disk, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_storage_zephyr_disk, test_open_info_rw_sync)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_SD_MMC);
	alp_storage_t       *s   = alp_storage_open(&cfg);
	zassert_not_null(s);

	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	zassert_equal(info.total_bytes, 16u * 512u);
	zassert_equal(info.block_size, 512u);

	/* Aligned multi-sector, ragged (RMW across a sector edge), and a
	 * misaligned destination buffer. */
	uint8_t wr[1100], rd[1100 + 1];
	for (size_t i = 0; i < sizeof(wr); ++i)
		wr[i] = (uint8_t)(i * 7u + 1u);
	zassert_equal(alp_storage_write(s, 0, wr, 1024), ALP_OK);
	zassert_equal(alp_storage_write(s, 700, wr, 100), ALP_OK);
	zassert_equal(alp_storage_sync(s), ALP_OK);

	memset(rd, 0, sizeof(rd));
	zassert_equal(alp_storage_read(s, 0, rd, 700), ALP_OK);
	zassert_mem_equal(rd, wr, 700);
	zassert_equal(alp_storage_read(s, 700, rd + 1, 100), ALP_OK);
	zassert_mem_equal(rd + 1, wr, 100);
	zassert_equal(alp_storage_read(s, 800, rd, 224), ALP_OK);
	zassert_mem_equal(rd, &wr[800], 224);

	zassert_equal(alp_storage_read(s, 16u * 512u - 1u, rd, 2), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(alp_storage_erase(s, 0, 512), ALP_ERR_NOSUPPORT);
	alp_storage_close(s);
}

ZTEST(alp_storage_zephyr_disk, test_flash_kind_not_served)
{
	/* No flash backend is linked here: zephyr_disk declines, sw_fallback
	 * takes the open -- the SD backend never answers for a flash kind. */
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_QSPI_FLASH);
	alp_storage_t       *s   = alp_storage_open(&cfg);
	zassert_not_null(s);
	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_ERR_NOSUPPORT);
	alp_storage_close(s);
}
