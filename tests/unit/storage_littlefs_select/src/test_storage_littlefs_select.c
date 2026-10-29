/* SPDX-License-Identifier: Apache-2.0
 *
 * Both-backends-enabled selection for <alp/storage.h>: zephyr_flash
 * (100) and zephyr_littlefs (110) are linked together, as on every
 * littlefs build.  The contract under test:
 *
 *   - an instance_id with a littlefs volume mounted at /lfs<id> is
 *     served by littlefs (never by raw flash_area <id>, which would
 *     corrupt the volume);
 *   - every other instance_id falls through to raw flash_area <id>.
 *
 * The two are told apart through get_info(): littlefs reports
 * erase_size == 1 and total_bytes of the file; raw flash reports the
 * partition size.
 */

#include <stdio.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include <alp/storage.h>

#define LFS_ID  FIXED_PARTITION_ID(storage_partition)
#define RAW_ID  FIXED_PARTITION_ID(slot1_partition)
#define LFS_MNT "/lfs" STRINGIFY(LFS_ID)

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);

static struct fs_mount_t mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &lfs_data,
	.storage_dev = (void *)LFS_ID,
	.mnt_point   = LFS_MNT,
};

static void *suite_setup(void)
{
	/* A fresh flash-simulator partition is erased; mount formats it. */
	zassert_equal(fs_mount(&mnt), 0);
	return NULL;
}

static void suite_teardown(void *unused)
{
	(void)unused;
	(void)fs_unmount(&mnt);
}

ZTEST_SUITE(alp_storage_littlefs_select, NULL, suite_setup, NULL, NULL, suite_teardown);

ZTEST(alp_storage_littlefs_select, test_mounted_instance_routes_to_littlefs)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_INTERNAL_FLASH);
	cfg.instance_id          = LFS_ID;
	alp_storage_t *s         = alp_storage_open(&cfg);
	zassert_not_null(s);

	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	/* littlefs: byte granule.  Raw flash would report the real page size. */
	zassert_equal(info.erase_size, 1u);
	zassert_equal(info.block_size, 1u);

	uint8_t wr[16], rd[16] = { 0 };
	for (size_t i = 0; i < sizeof(wr); ++i) {
		wr[i] = (uint8_t)(i + 1u);
	}
	zassert_equal(alp_storage_write(s, 0, wr, sizeof(wr)), ALP_OK);
	zassert_equal(alp_storage_read(s, 0, rd, sizeof(rd)), ALP_OK);
	zassert_mem_equal(rd, wr, sizeof(wr));
	alp_storage_close(s);

	/* The volume must still mount-and-stat after the writes: the file
	 * exists, so no raw write clobbered its metadata. */
	struct fs_dirent ent;
	zassert_equal(fs_stat(LFS_MNT "/alp_storage.bin", &ent), 0);
	zassert_equal(ent.size, sizeof(wr));
}

ZTEST(alp_storage_littlefs_select, test_unmounted_instance_falls_through_to_raw_flash)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_INTERNAL_FLASH);
	cfg.instance_id          = RAW_ID;
	alp_storage_t *s         = alp_storage_open(&cfg);
	zassert_not_null(s);

	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	/* Raw flash_area: total_bytes is the partition size, not a file size. */
	zassert_equal(info.total_bytes, FIXED_PARTITION_SIZE(slot1_partition));
	alp_storage_close(s);
}
