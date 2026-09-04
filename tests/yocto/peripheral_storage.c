/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for the #1140 review blockers on the Yocto
 * storage backend (src/backends/storage/yocto_drv.c):
 *
 *   - blockers 1+2: alp_storage_write()/alp_storage_erase() must
 *     refuse with ALP_ERR_INVAL, BEFORE touching the device fd,
 *     unless allow_unsafe_write was set at open() -- a caller using
 *     ALP_STORAGE_CONFIG_DEFAULT must never be able to write/erase
 *     the whole-disk mmcblk node or an unvalidated MTD partition.
 *   - major: get_info()'s erase_size for the SD/MMC path must come
 *     from the real discard_granularity sysfs attribute, not the
 *     512-byte logical sector size.
 *   - minor: an MTD erase whose offset/len exceeds UINT32_MAX must be
 *     rejected, not silently truncated (which would erase nothing and
 *     still report ALP_OK).
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_wdt.c) to reach its file-local
 * y_write()/y_read()/y_erase() and the g_storage_test_read_uint_attr_hook
 * seam, and to construct a y_storage_data_t directly instead of going
 * through y_open() (which needs a real block/MTD device this test
 * never touches).  The write/read tests use a real temporary regular
 * file -- never a real /dev/mmcblk* or /dev/mtd* node -- so pwrite()/pread()
 * exercise genuine syscalls without risking any real storage.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_storage
 *   ctest --test-dir build -R alp_test_peripheral_storage
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/backends/storage/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture: a real temp regular file standing in for the device fd     */
/* ------------------------------------------------------------------ */

static char g_tmpl[] = "/tmp/alp_test_storage_XXXXXX";

static int make_tmpfile(size_t size, uint8_t fill)
{
	char path[sizeof(g_tmpl)];
	memcpy(path, g_tmpl, sizeof(g_tmpl));
	int fd = mkstemp(path);
	if (fd < 0) return -1;
	unlink(path); /* deleted on close; nothing left behind */

	uint8_t *buf = (uint8_t *)malloc(size);
	memset(buf, fill, size);
	ssize_t w = write(fd, buf, size);
	free(buf);
	if (w < 0 || (size_t)w != size) {
		close(fd);
		return -1;
	}
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static uint8_t read_byte_at(int fd, off_t off)
{
	uint8_t b = 0xAA;
	pread(fd, &b, 1, off);
	return b;
}

/* ------------------------------------------------------------------ */

static void test_write_refused_without_opt_in_and_does_not_touch_file(void)
{
	int fd = make_tmpfile(64, 0xFF);
	ALP_ASSERT_TRUE(fd >= 0);

	y_storage_data_t d = {
		.fd                 = fd,
		.devtype            = Y_STORAGE_DEV_BLOCK,
		.total_bytes        = 64,
		.block_size         = 1,
		.erase_size         = 1,
		.allow_unsafe_write = false, /* the ALP_STORAGE_CONFIG_DEFAULT case */
	};
	alp_storage_backend_state_t st = { .be_data = &d };

	uint8_t      new_data[4] = { 1, 2, 3, 4 };
	alp_status_t rc          = y_write(&st, 0, new_data, sizeof(new_data));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	/* Proves the gate ran BEFORE any pwrite(): the file the "default
	 * config" caller could otherwise have damaged is untouched. */
	ALP_ASSERT_EQ_INT(read_byte_at(fd, 0), 0xFF);

	close(fd);
}

static void test_write_succeeds_with_explicit_opt_in(void)
{
	int fd = make_tmpfile(64, 0xFF);
	ALP_ASSERT_TRUE(fd >= 0);

	y_storage_data_t d = {
		.fd                 = fd,
		.devtype            = Y_STORAGE_DEV_BLOCK,
		.total_bytes        = 64,
		.block_size         = 1,
		.erase_size         = 1,
		.allow_unsafe_write = true,
	};
	alp_storage_backend_state_t st = { .be_data = &d };

	uint8_t      new_data[4] = { 1, 2, 3, 4 };
	alp_status_t rc          = y_write(&st, 0, new_data, sizeof(new_data));
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT(read_byte_at(fd, 0), 1);
	ALP_ASSERT_EQ_INT(read_byte_at(fd, 3), 4);

	close(fd);
}

static void test_read_allowed_without_opt_in(void)
{
	int fd = make_tmpfile(8, 0x42);
	ALP_ASSERT_TRUE(fd >= 0);

	y_storage_data_t d = {
		.fd                 = fd,
		.devtype            = Y_STORAGE_DEV_BLOCK,
		.total_bytes        = 8,
		.block_size         = 1,
		.erase_size         = 1,
		.allow_unsafe_write = false, /* reads stay permissive regardless */
	};
	alp_storage_backend_state_t st = { .be_data = &d };

	uint8_t      out[8] = { 0 };
	alp_status_t rc     = y_read(&st, 0, out, sizeof(out));
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT(out[0], 0x42);

	close(fd);
}

/* Erase gate proof: same (offset, len, erase_size) against a regular
 * file (not a real block device) with the gate false vs true.  A
 * regular file rejects BLKDISCARD with ENOTTY (-> ALP_ERR_NOSUPPORT)
 * when the ioctl is actually attempted, which is a DIFFERENT code
 * than the gate's ALP_ERR_INVAL -- so this also proves the gate check
 * runs strictly before the ioctl, not just that the final code
 * happens to match. */
static void test_erase_gate_short_circuits_before_ioctl(void)
{
	int fd = make_tmpfile(4096, 0xFF);
	ALP_ASSERT_TRUE(fd >= 0);

	y_storage_data_t d = {
		.fd          = fd,
		.devtype     = Y_STORAGE_DEV_BLOCK,
		.total_bytes = 4096,
		.block_size  = 512,
		.erase_size  = 4096,
	};
	alp_storage_backend_state_t st = { .be_data = &d };

	d.allow_unsafe_write = false;
	ALP_ASSERT_EQ_INT(y_erase(&st, 0, 4096), ALP_ERR_INVAL);

	d.allow_unsafe_write = true;
	ALP_ASSERT_EQ_INT(y_erase(&st, 0, 4096), ALP_ERR_NOSUPPORT);

	close(fd);
}

/* MTD 32-bit truncation guard: an erase spanning exactly 4 GiB must
 * not have its length silently truncate to 0 (which would erase
 * nothing and still report ALP_OK). */
static void test_mtd_erase_rejects_len_past_uint32_max(void)
{
	y_storage_data_t d = {
		.fd                 = -1, /* never reached -- rejected before any ioctl */
		.devtype            = Y_STORAGE_DEV_MTD,
		.total_bytes        = (uint64_t)UINT32_MAX + 1u,
		.block_size         = 1,
		.erase_size         = 1,
		.allow_unsafe_write = true,
	};
	alp_storage_backend_state_t st = { .be_data = &d };

	alp_status_t rc = y_erase(&st, 0, (uint64_t)UINT32_MAX + 1u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_OUT_OF_RANGE);
}

/* Discard-granularity sysfs read: the seam the geometry fix
 * (_block_discard_granularity) uses, proving it plumbs the sysfs
 * value through rather than reporting the logical sector size. */
static alp_status_t fake_discard_granularity_4mib(const char *path, uint32_t *out)
{
	(void)path;
	*out = 4u * 1024u * 1024u;
	return ALP_OK;
}

static void test_discard_granularity_hook_feeds_erase_size(void)
{
	g_storage_test_read_uint_attr_hook = fake_discard_granularity_4mib;

	uint32_t erase_size = 0;
	_block_discard_granularity("/dev/mmcblk0", &erase_size);
	ALP_ASSERT_EQ_INT(erase_size, 4u * 1024u * 1024u);

	g_storage_test_read_uint_attr_hook = NULL;
}

int main(void)
{
	test_write_refused_without_opt_in_and_does_not_touch_file();
	test_write_succeeds_with_explicit_opt_in();
	test_read_allowed_without_opt_in();
	test_erase_gate_short_circuits_before_ioctl();
	test_mtd_erase_rejects_len_past_uint32_max();
	test_discard_granularity_hook_feeds_erase_size();

	ALP_TEST_SUMMARY();
}
