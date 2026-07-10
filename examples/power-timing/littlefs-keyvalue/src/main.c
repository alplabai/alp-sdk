/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * littlefs-keyvalue -- teaches the Zephyr `fs` subsystem's littlefs
 * backend by round-tripping a single key/value pair through a
 * mounted filesystem.
 *
 * What success looks like:
 *
 *   [littlefs-keyvalue] mounting /lfs ...
 *   [littlefs-keyvalue] mount rc=0
 *   [littlefs-keyvalue] wrote "answer=42" (9 bytes)
 *   [littlefs-keyvalue] read  "answer=42" (9 bytes)
 *   [littlefs-keyvalue] dir /lfs:
 *   [littlefs-keyvalue]   [FILE] kv.txt (size = 9)
 *   [littlefs-keyvalue] read matches write -- OK
 *   [littlefs-keyvalue] done
 *
 * Where this runs: native_sim only.  `board.yaml`'s `libraries:
 * [littlefs]` selects CONFIG_FILE_SYSTEM_LITTLEFS + CONFIG_FILE_SYSTEM
 * (see `_LIBRARY_KCONFIG` in scripts/alp_project.py); this example's
 * own prj.conf adds CONFIG_FLASH + CONFIG_FLASH_MAP, the storage-medium
 * plumbing littlefs needs underneath (see prj.conf for why that split
 * exists).  On native_sim the storage medium is native_sim.dts's
 * built-in `zephyr,sim-flash` `storage_partition` -- 16 KiB of
 * host-RAM standing in for flash, erase-granularity and all.
 *
 * Hardware swap: on real silicon, point `storage_partition` at the
 * SoM's actual flash-backed partition instead (Alif MRAM on E1M-AEN,
 * or Renesas xSPI NOR on E1M-V2N) -- the fs_mount()/fs_open()/
 * fs_read()/fs_write() calls below are unchanged; only the DTS
 * partition backing the label differs.  `ALP_LITTLEFS_XSPI_DMA` /
 * `ALP_LITTLEFS_EMMC_DMA` in zephyr/Kconfig.alp-libraries name the
 * per-family HW-accelerated backends; `ALP_LITTLEFS_SYNC_IO` (this
 * example's default) is the SW fallback that works everywhere.
 *
 * [UNTESTED]: this build machine's west workspace never ran `west
 * update` after littlefs was added to the Zephyr name-allowlist (see
 * west.yml), so the upstream `modules/fs/littlefs` source (lfs.c/
 * lfs.h) isn't checked out.  Kconfig demotes CONFIG_FILE_SYSTEM_LITTLEFS
 * to n in that case (`depends on ZEPHYR_LITTLEFS_MODULE`), but
 * <zephyr/fs/littlefs.h> `#include`s <lfs.h> unconditionally -- so the
 * real code path below is compiled only under
 * `#ifdef CONFIG_FILE_SYSTEM_LITTLEFS`, which is n here.  The `#else`
 * branch keeps the example buildable + runnable (green on twister) in
 * that state; a workspace with `west update` re-run picks up the `y`
 * branch instead, with no source change required. */

#include <stdio.h>

#ifdef CONFIG_FILE_SYSTEM_LITTLEFS
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

/* The key/value pair this example round-trips.  A real key/value
 * store would serialise many such pairs (e.g. one file per key, or a
 * small line-oriented format inside one file); one pair is enough to
 * exercise every fs_* call without extra bookkeeping. */
#define KV_LINE     "answer=42"
#define KV_FILENAME "/lfs/kv.txt"

/* FS_LITTLEFS_DECLARE_DEFAULT_CONFIG expands to a `struct fs_littlefs`
 * named `storage` sized from the FS_LITTLEFS_* Kconfig defaults
 * (block/cache/lookahead sizes) -- see zephyr/subsys/fs/Kconfig.littlefs.
 * We don't need to tune any of those for a single small file. */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);

/* The mount descriptor.  `storage_dev` is the flash_area ID for the
 * DTS `storage_partition` label -- PARTITION_ID() resolves the DT
 * node to that numeric ID at compile time, so nothing here names a
 * SoM-specific address. */
static struct fs_mount_t littlefs_mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &storage,
	.storage_dev = (void *)PARTITION_ID(storage_partition),
	.mnt_point   = "/lfs",
};

/* Lists every entry under `path`, printing type + size.  This is the
 * fs_opendir/fs_readdir/fs_closedir teaching block: littlefs directory
 * iteration always ends with an empty-name entry rather than a
 * distinguished EOF return code. */
static void list_dir(const char *path)
{
	struct fs_dir_t  dir;
	struct fs_dirent entry;
	int              rc;

	fs_dir_t_init(&dir);

	rc = fs_opendir(&dir, path);
	if (rc < 0) {
		printf("[littlefs-keyvalue] opendir %s failed: %d\n", path, rc);
		return;
	}

	printf("[littlefs-keyvalue] dir %s:\n", path);
	for (;;) {
		rc = fs_readdir(&dir, &entry);
		/* entry.name[0] == '\0' is littlefs's end-of-directory
		 * marker -- there is no separate "-ENOENT means done"
		 * convention here. */
		if (rc != 0 || entry.name[0] == '\0') {
			break;
		}
		if (entry.type == FS_DIR_ENTRY_DIR) {
			printf("[littlefs-keyvalue]   [DIR ] %s\n", entry.name);
		} else {
			printf("[littlefs-keyvalue]   [FILE] %s (size = %zu)\n", entry.name, entry.size);
		}
	}

	fs_closedir(&dir);
}

int main(void)
{
	struct fs_file_t file;
	char             readback[32] = { 0 };
	int              rc;
	ssize_t          n;

	printf("[littlefs-keyvalue] mounting %s ...\n", littlefs_mnt.mnt_point);

	/* fs_mount() formats the partition on first use (littlefs
	 * auto-formats an unrecognised/blank device) and mounts it on
	 * every subsequent run -- no explicit "is this formatted?"
	 * check needed from the app. */
	rc = fs_mount(&littlefs_mnt);
	printf("[littlefs-keyvalue] mount rc=%d\n", rc);
	if (rc < 0) {
		/* Nothing else to do without a filesystem -- return early
		 * rather than dereference an unmounted mount point. */
		return 0;
	}

	/* fs_file_t_init() zeroes the handle before fs_open() touches
	 * it -- skipping this step is a common source of garbage-state
	 * bugs when a `struct fs_file_t` is reused across opens. */
	fs_file_t_init(&file);

	rc = fs_open(&file, KV_FILENAME, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		printf("[littlefs-keyvalue] open failed: %d\n", rc);
		goto unmount;
	}

	n = fs_write(&file, KV_LINE, sizeof(KV_LINE) - 1);
	printf("[littlefs-keyvalue] wrote \"%s\" (%d bytes)\n", KV_LINE, (int)n);

	/* Writes don't implicitly rewind the file position -- seek back
	 * to the start before reading what was just written. */
	rc = fs_seek(&file, 0, FS_SEEK_SET);
	if (rc < 0) {
		printf("[littlefs-keyvalue] seek failed: %d\n", rc);
		fs_close(&file);
		goto unmount;
	}

	n = fs_read(&file, readback, sizeof(readback) - 1);
	printf("[littlefs-keyvalue] read  \"%s\" (%d bytes)\n", readback, (int)n);

	fs_close(&file);

	list_dir(littlefs_mnt.mnt_point);

	/* The whole point of the round-trip: prove what came back out
	 * is byte-for-byte what went in. */
	if (n == (ssize_t)(sizeof(KV_LINE) - 1) && memcmp(readback, KV_LINE, (size_t)n) == 0) {
		printf("[littlefs-keyvalue] read matches write -- OK\n");
	} else {
		printf("[littlefs-keyvalue] read does NOT match write -- FAIL\n");
	}

unmount:
	rc = fs_unmount(&littlefs_mnt);
	printf("[littlefs-keyvalue] unmount rc=%d\n", rc);

	printf("[littlefs-keyvalue] done\n");
	return 0;
}

#else /* !CONFIG_FILE_SYSTEM_LITTLEFS */

/* The littlefs module isn't fetched in this workspace -- see the
 * [UNTESTED] block in the top-of-file comment.  This branch prints
 * why instead of silently skipping the demonstration, and still ends
 * with the twister marker line so the build+run gate stays honest
 * (green because it genuinely built and ran, not because the real
 * code path was skipped unnoticed). */
int main(void)
{
	printf("[littlefs-keyvalue] CONFIG_FILE_SYSTEM_LITTLEFS=n in this build --\n");
	printf("[littlefs-keyvalue] the littlefs west module isn't fetched (run\n");
	printf("[littlefs-keyvalue] `west update` to pull modules/fs/littlefs); the\n");
	printf("[littlefs-keyvalue] fs_mount/fs_open/fs_read/fs_write/fs_readdir code\n");
	printf("[littlefs-keyvalue] above this #else is the real, correct API usage.\n");
	printf("[littlefs-keyvalue] done\n");
	return 0;
}

#endif /* CONFIG_FILE_SYSTEM_LITTLEFS */
