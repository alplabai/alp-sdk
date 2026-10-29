/* SPDX-License-Identifier: Apache-2.0
 *
 * Which flash_area IDs back a MOUNTED filesystem.
 *
 * The answer comes from the generated alp_storage_mounts[] table
 * (scripts/alp_orchestrate/headers.py), the same table that names each
 * mount point and sets `.storage_dev = FIXED_PARTITION_ID(<name>_partition)`.
 * Deciding by the backing partition -- not by how the mount point is
 * spelled -- is what makes "a mounted partition is not raw-addressable"
 * true for "/lfs/settings", "/lfs" or any other board.yaml mount name.
 *
 * Both zephyr_littlefs (claims the id) and zephyr_flash (refuses the id)
 * use this, so they can never disagree.  The table is a weak reference:
 * an app without board.yaml storage regions has no table, and then no
 * partition is known to be mounted.  A mount made by hand with
 * fs_mount() and absent from the table is invisible here.
 */
#ifndef ALP_BACKENDS_STORAGE_ZEPHYR_MOUNTS_H
#define ALP_BACKENDS_STORAGE_ZEPHYR_MOUNTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/fs/fs.h>

extern const struct fs_mount_t *alp_storage_mounts[] __attribute__((weak));
extern const size_t             alp_storage_mount_count __attribute__((weak));

/* True when `mnt_point` is currently mounted.  fs_readmount() walks the
 * mount list without logging, unlike fs_stat()/fs_statvfs() which
 * LOG_ERR on a missing mount. */
static inline bool alp_zmnt_is_mounted(const char *mnt_point)
{
	int         idx  = 0;
	const char *name = NULL;
	while (fs_readmount(&idx, &name) == 0) {
		if (name != NULL && strcmp(name, mnt_point) == 0) return true;
	}
	return false;
}

/* The currently-mounted table entry whose backing partition is
 * flash_area `id`, or NULL when that partition backs no mounted fs. */
static inline const struct fs_mount_t *alp_zmnt_find_mounted(uint32_t id)
{
	if (alp_storage_mounts == NULL || &alp_storage_mount_count == NULL) return NULL;
	for (size_t i = 0; i < alp_storage_mount_count; ++i) {
		const struct fs_mount_t *m = alp_storage_mounts[i];
		if (m == NULL || m->mnt_point == NULL) continue;
		if ((uintptr_t)m->storage_dev != (uintptr_t)id) continue;
		if (alp_zmnt_is_mounted(m->mnt_point)) return m;
	}
	return NULL;
}

#endif /* ALP_BACKENDS_STORAGE_ZEPHYR_MOUNTS_H */
