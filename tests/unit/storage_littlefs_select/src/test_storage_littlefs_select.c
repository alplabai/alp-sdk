/* SPDX-License-Identifier: Apache-2.0
 *
 * Both-backends-enabled selection for <alp/storage.h>: zephyr_flash
 * (100) and zephyr_littlefs (110) are linked together, as on every
 * littlefs build.  The mount is laid out the way the SDK's own emitter
 * (scripts/alp_orchestrate/headers.py) lays it out: a board.yaml-style
 * mount point ("/lfs/settings"), `.storage_dev = FIXED_PARTITION_ID(..)`,
 * published through alp_storage_mounts[].  The contract under test:
 *
 *   - a flash_area that backs a mounted littlefs volume is served by
 *     littlefs, whatever the mount point is called, and is NEVER opened
 *     raw (a raw write would corrupt the volume);
 *   - the match is by BACKING partition, not by mount-point spelling: a
 *     mount named after another flash_area ID does not claim that ID;
 *   - every other (or no-longer-mounted) flash_area is opened raw.
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

#define LFS_ID FIXED_PARTITION_ID(storage_partition)
#define RAW_ID FIXED_PARTITION_ID(slot1_partition)

#define PROD_MNT "/lfs/settings"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);

static struct fs_mount_t mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &lfs_data,
	.storage_dev = (void *)LFS_ID,
	.mnt_point   = PROD_MNT,
};

/* Strong definitions of what the generated storage header emits. */
const struct fs_mount_t *alp_storage_mounts[]    = { &mnt };
const size_t             alp_storage_mount_count = 1;

static void test_before(void *unused)
{
	(void)unused;
	mnt.mnt_point = PROD_MNT;
	/* A fresh flash-simulator partition is erased; mount formats it. */
	zassert_equal(fs_mount(&mnt), 0);
}

static void test_after(void *unused)
{
	(void)unused;
	(void)fs_unmount(&mnt);
}

ZTEST_SUITE(alp_storage_littlefs_select, NULL, NULL, test_before, test_after, NULL);

static alp_storage_t *open_id(uint32_t id)
{
	alp_storage_config_t cfg = ALP_STORAGE_CONFIG_DEFAULT(ALP_STORAGE_KIND_INTERNAL_FLASH);
	cfg.instance_id          = id;
	return alp_storage_open(&cfg);
}

ZTEST(alp_storage_littlefs_select, test_production_mount_routes_to_littlefs_never_raw)
{
	alp_storage_t *s = open_id(LFS_ID);
	zassert_not_null(s);

	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	/* littlefs: byte granule.  Raw flash would report the real page size
	 * and the partition size as total_bytes. */
	zassert_equal(info.erase_size, 1u);
	zassert_equal(info.block_size, 1u);
	zassert_not_equal(info.total_bytes, FIXED_PARTITION_SIZE(storage_partition));

	uint8_t wr[16], rd[16] = { 0 };
	for (size_t i = 0; i < sizeof(wr); ++i) {
		wr[i] = (uint8_t)(i + 1u);
	}
	zassert_equal(alp_storage_write(s, 0, wr, sizeof(wr)), ALP_OK);
	zassert_equal(alp_storage_read(s, 0, rd, sizeof(rd)), ALP_OK);
	zassert_mem_equal(rd, wr, sizeof(wr));
	alp_storage_close(s);

	/* The volume must still stat after the writes: the file exists, so
	 * no raw write clobbered its metadata. */
	struct fs_dirent ent;
	zassert_equal(fs_stat(PROD_MNT "/alp_storage.bin", &ent), 0);
	zassert_equal(ent.size, sizeof(wr));
}

ZTEST(alp_storage_littlefs_select, test_mount_index_differs_from_backing_flash_area)
{
	/* Re-mount the same partition under a name that spells ANOTHER
	 * flash_area ID.  The name must not move the claim. */
	zassert_equal(fs_unmount(&mnt), 0);
	static char decoy[16];
	(void)snprintf(decoy, sizeof(decoy), "/lfs%u", (unsigned)RAW_ID);
	mnt.mnt_point = decoy;
	zassert_equal(fs_mount(&mnt), 0);

	/* LFS_ID backs the volume: littlefs. */
	alp_storage_t *s = open_id(LFS_ID);
	zassert_not_null(s);
	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	zassert_equal(info.erase_size, 1u);
	alp_storage_close(s);

	/* RAW_ID merely appears in the mount name: raw flash_area. */
	s = open_id(RAW_ID);
	zassert_not_null(s);
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	zassert_equal(info.total_bytes, FIXED_PARTITION_SIZE(slot1_partition));
	alp_storage_close(s);
}

ZTEST(alp_storage_littlefs_select, test_unmounted_instance_falls_through_to_raw_flash)
{
	alp_storage_t *s = open_id(RAW_ID);
	zassert_not_null(s);

	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	/* Raw flash_area: total_bytes is the partition size, not a file size. */
	zassert_equal(info.total_bytes, FIXED_PARTITION_SIZE(slot1_partition));
	alp_storage_close(s);
}

ZTEST(alp_storage_littlefs_select, test_partition_is_raw_again_once_unmounted)
{
	zassert_equal(fs_unmount(&mnt), 0);

	alp_storage_t *s = open_id(LFS_ID);
	zassert_not_null(s);
	alp_storage_info_t info;
	zassert_equal(alp_storage_get_info(s, &info), ALP_OK);
	zassert_equal(info.total_bytes, FIXED_PARTITION_SIZE(storage_partition));
	alp_storage_close(s);
}
