/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto storage_* driver-class backend.  Binds the
 * alp_storage dispatcher's ops vtable to two Linux userspace block
 * interfaces (issue #1140):
 *
 *   - SD/MMC (SDHI0 eMMC, SDHI1 uSD)  -> /dev/mmcblk<instance_id>
 *     Generic block device: BLKGETSIZE64 / BLKSSZGET for geometry,
 *     pread/pwrite for I/O, BLKDISCARD as the closest thing the block
 *     layer has to an "erase" primitive.
 *   - QSPI / OSPI / on-die NOR (xSPI) -> /dev/mtd<instance_id>
 *     Kernel MTD chardev: MEMGETINFO for geometry, pread/pwrite for
 *     I/O, MEMERASE for a real NOR-style erase.
 *
 * @par instance_id mapping (deliberately NOT hardcoded to one path):
 *      alp_storage_config_t carries only `kind` + `instance_id`, no
 *      device string, so this backend derives the /dev node from
 *      those two fields alone.  `instance_id` is passed straight
 *      through as the kernel's own device-minor index (mmcblkN / N
 *      in "/dev/mtdN") -- which controller lands on which index is
 *      decided by DT probe order (meta-alp-sdk's V2N SoM device tree
 *      sdhi0/sdhi1/xspi nodes), NOT by this backend.  A board
 *      integration that needs a stable mapping pins it via DT aliases
 *      / a udev rule upstream of this file; inventing a guess here
 *      would be worse than requiring the caller to know their board's
 *      boot-time enumeration order.
 *
 * @par Write/erase safety gate (#1140 review, blockers 1+2):
 *      `/dev/mmcblk<N>` is the WHOLE-DISK node (GPT/MBR + bootloader
 *      live at offset 0), and on V2N `/dev/mtd0` is itself the
 *      FIP/bootloader MTD partition -- this backend has no way to
 *      tell "user data" from "boot medium" from instance_id alone.
 *      `alp_storage_write()` and `alp_storage_erase()` therefore
 *      REFUSE with ALP_ERR_INVAL unless the caller explicitly sets
 *      `alp_storage_config_t.allow_unsafe_write = true` at open() --
 *      see that field's doxygen in <alp/storage.h>.  A caller using
 *      `ALP_STORAGE_CONFIG_DEFAULT` verbatim (allow_unsafe_write ==
 *      false) can therefore never write or erase through this
 *      backend, on any instance_id, full stop.  Reads stay permissive
 *      (get_info()/read() work with the default config) since a read
 *      cannot damage anything.
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.  Selected on any
 * silicon (silicon_ref "*") because both the block-device and MTD
 * ABIs are SoC-agnostic -- the device tree decides which physical
 * controller backs each node.
 *
 * @par Status: REAL implementation.  BENCH-UNVERIFIED -- no
 *      /dev/mmcblk*  or /dev/mtd* nodes exist in this build
 *      environment, so the open()/geometry/read/write/erase paths
 *      have not been exercised against real V2N SDHI/xSPI hardware.
 *      Do not read anything in this file as silicon-proven.
 *
 * @par Unsupported / partial ops
 *      - configure_inline_aes() returns ALP_ERR_NOSUPPORT: no Linux
 *        userspace ABI exposes inline XIP-AES key/IV programming for
 *        a block or MTD chardev (that is bootloader / secure-world
 *        OTP key-slot territory) -- matches the documented NOSUPPORT
 *        case in alp/storage.h's own doxygen for this op ("V2N
 *        today").
 *      - write() on an MTD (NOR) target does NOT erase-before-write:
 *        it is a raw pwrite(), so writing over an unerased region
 *        only clears bits, same as bare-metal NOR.  See
 *        <alp/storage.h>'s alp_storage_write() doxygen -- the "SD/MMC
 *        and the Yocto backend handle this transparently" sentence
 *        was corrected in the same change that added this note; only
 *        SD/MMC's block-layer path is transparent, MTD is not.
 *      - erase() on SD/MMC issues BLKDISCARD, a best-effort TRIM hint
 *        that is NOT guaranteed to read back as zeroed/erased, unlike
 *        a true NOR erase -- see <alp/storage.h>'s alp_storage_erase()
 *        doxygen for the same caveat at the call site.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/fs.h>
#include <mtd/mtd-abi.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/storage.h>

#include "storage_ops.h"
#include "common/alp_errno.h"

/* /dev/mmcblk<N> or /dev/mtd<N>; "mtd" + up to 10 digits + NUL is the
 * long side, so 32 bytes leaves comfortable headroom. */
#define Y_STORAGE_PATH_MAX 32

typedef enum {
	Y_STORAGE_DEV_BLOCK = 0, /* /dev/mmcblkN -- SD/MMC generic block layer */
	Y_STORAGE_DEV_MTD   = 1  /* /dev/mtdN -- kernel MTD chardev (NOR flash) */
} y_storage_devtype_t;

/* Per-handle backend data: the open fd, which ABI it speaks, the
 * geometry read back at open() so get_info()/erase() don't re-query
 * the kernel on every call, and the cached write/erase opt-in gate
 * (see the file-header "safety gate" note).  Boxed onto the heap so
 * the void* be_data slot in alp_storage_backend_state_t owns it. */
typedef struct {
	int                 fd;
	y_storage_devtype_t devtype;
	uint64_t            total_bytes;
	uint32_t            block_size;
	uint32_t            erase_size;
	bool                allow_unsafe_write;
} y_storage_data_t;

/* Test-only seam: intercepts the single sysfs-integer-attribute read
 * this backend performs (discard_granularity), so a test can supply a
 * canned value without a real /sys/block tree.  Default NULL means
 * "really read the file" (_read_uint_sysfs_attr's own body). */
static alp_status_t (*g_storage_test_read_uint_attr_hook)(const char *path, uint32_t *out) = NULL;

/** @brief Read a small ASCII-integer sysfs attribute file into @p out. */
static alp_status_t _read_uint_sysfs_attr(const char *path, uint32_t *out)
{
	if (g_storage_test_read_uint_attr_hook != NULL) {
		return g_storage_test_read_uint_attr_hook(path, out);
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return alp_status_from_posix_errno(errno);

	char    buf[32];
	ssize_t r = read(fd, buf, sizeof(buf) - 1u);
	int     e = errno;
	close(fd);
	if (r < 0) return alp_status_from_posix_errno(e);
	buf[r] = '\0';

	char *end       = NULL;
	errno           = 0;
	unsigned long v = strtoul(buf, &end, 10);
	if (end == buf || errno == ERANGE) return ALP_ERR_IO;
	*out = (uint32_t)v;
	return ALP_OK;
}

/**
 * @brief Read /sys/block/<devname>/queue/discard_granularity for the
 *        block device at @p devpath (e.g. "/dev/mmcblk0").
 *
 * This is the REAL minimum discard/TRIM unit -- typically 512 KiB to
 * 4 MiB on eMMC/SD, nowhere close to the 512-byte logical sector
 * BLKSSZGET reports.  Reporting the sector size here (the previous
 * behaviour) let a caller align an erase() call to get_info()'s
 * erase_size and still have BLKDISCARD reject it with EINVAL.  A
 * device that reports 0 (no discard support) or whose sysfs node is
 * absent leaves *out at 0, which y_erase() already treats as "no
 * erase primitive available" (ALP_ERR_NOSUPPORT) -- an honest signal,
 * not a guess.
 */
static void _block_discard_granularity(const char *devpath, uint32_t *out)
{
	*out = 0u;

	/* devpath is always "/dev/<name>" here (see _resolve_path). */
	const char *name = devpath;
	if (strncmp(name, "/dev/", 5) == 0) name += 5;

	char path[96];
	int  n = snprintf(path, sizeof(path), "/sys/block/%s/queue/discard_granularity", name);
	if (n < 0 || (size_t)n >= sizeof(path)) return;

	uint32_t     v  = 0;
	alp_status_t rc = _read_uint_sysfs_attr(path, &v);
	if (rc == ALP_OK) *out = v;
}

/**
 * @brief Map (kind, instance_id) to a /dev node path + which ioctl
 *        family to use.  See the file-header mapping note.
 */
static alp_status_t _resolve_path(alp_storage_kind_t   kind,
                                  uint32_t             instance_id,
                                  y_storage_devtype_t *devtype,
                                  char                *path,
                                  size_t               pathlen)
{
	int n;
	switch (kind) {
	case ALP_STORAGE_KIND_SD_MMC:
		*devtype = Y_STORAGE_DEV_BLOCK;
		n        = snprintf(path, pathlen, "/dev/mmcblk%u", (unsigned)instance_id);
		break;
	case ALP_STORAGE_KIND_QSPI_FLASH:
	case ALP_STORAGE_KIND_OSPI_FLASH:
	case ALP_STORAGE_KIND_INTERNAL_FLASH:
		/* xSPI backs on-module NOR under the kernel MTD subsystem
		 * regardless of whether the portable caller labelled it
		 * QSPI, OSPI or "internal" -- the wire protocol distinction
		 * lives in the DT node the kernel already bound, not in
		 * anything this chardev-level backend can observe. */
		*devtype = Y_STORAGE_DEV_MTD;
		n        = snprintf(path, pathlen, "/dev/mtd%u", (unsigned)instance_id);
		break;
	default:
		return ALP_ERR_INVAL;
	}
	if (n < 0 || (size_t)n >= pathlen) return ALP_ERR_INVAL;
	return ALP_OK;
}

/**
 * @brief Open the resolved device node and cache its geometry.
 *
 * MTD geometry comes from MEMGETINFO (struct mtd_info_user); block
 * geometry from BLKGETSIZE64 + BLKSSZGET + the real discard
 * granularity (see _block_discard_granularity()).  A sector-size
 * query that fails or reports 0 falls back to the conventional
 * 512-byte sector rather than failing the whole open -- some block
 * drivers simply don't implement BLKSSZGET.
 */
static alp_status_t y_open(const alp_storage_config_t  *cfg,
                           alp_storage_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	if (cfg == NULL || st == NULL || caps_out == NULL) return ALP_ERR_INVAL;

	char                path[Y_STORAGE_PATH_MAX];
	y_storage_devtype_t devtype;
	alp_status_t rc = _resolve_path(cfg->kind, cfg->instance_id, &devtype, path, sizeof(path));
	if (rc != ALP_OK) return rc;

	/* Open read-only unless the caller BOTH wants writes and has taken
	 * the safety gate off.  Without the second half we would hand out a
	 * kernel-writable fd on a whole-disk boot node to exactly the caller
	 * this file promises "can never write or erase" -- the gate would be
	 * one C-level `if` per op with nothing behind it.  It also avoids a
	 * pointless writable open of a mounted block device, which some
	 * kernels refuse outright, which would cost the default-config
	 * caller its get_info()/read() too. */
	const bool want_write = (!cfg->read_only && cfg->allow_unsafe_write);
	int        fd         = open(path, (want_write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (fd < 0) return alp_status_from_posix_errno(errno);

	y_storage_data_t *d = (y_storage_data_t *)malloc(sizeof(*d));
	if (d == NULL) {
		close(fd);
		return ALP_ERR_NOMEM;
	}
	d->fd                 = fd;
	d->devtype            = devtype;
	d->allow_unsafe_write = cfg->allow_unsafe_write;

	if (devtype == Y_STORAGE_DEV_MTD) {
		struct mtd_info_user info;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, MEMGETINFO, &info) < 0) {
			alp_status_t erc = alp_status_from_posix_errno(errno);
			close(fd);
			free(d);
			return erc;
		}
		d->total_bytes = (uint64_t)info.size;
		/* writesize is the minimum programmable unit (1 for most NOR,
		 * >1 for NAND page-write); never report 0 as a block_size, the
		 * public API documents it as "min unit for read/write". */
		d->block_size = (info.writesize != 0u) ? info.writesize : 1u;
		d->erase_size = info.erasesize;
	} else {
		uint64_t bytes = 0;
		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
			alp_status_t erc = alp_status_from_posix_errno(errno);
			close(fd);
			free(d);
			return erc;
		}
		int sector = 0;
		if (ioctl(fd, BLKSSZGET, &sector) < 0 || sector <= 0) {
			sector = 512; /* conventional default; see doxygen above */
		}
		d->total_bytes = bytes;
		d->block_size  = (uint32_t)sector;
		/* Real discard/TRIM granularity, NOT the logical sector size
		 * -- see _block_discard_granularity()'s doxygen. */
		_block_discard_granularity(path, &d->erase_size);
	}

	st->dev         = NULL;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t y_get_info(alp_storage_backend_state_t *st, alp_storage_info_t *info)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	info->total_bytes = d->total_bytes;
	info->block_size  = d->block_size;
	info->erase_size  = d->erase_size;
	return ALP_OK;
}

/**
 * @brief pread() the requested range, looping over short reads.
 *
 * A short read from a real block/MTD device before @p len bytes are
 * satisfied means the device end was reached early (geometry drift,
 * e.g. an OOB region on NAND) -- reported as ALP_ERR_IO rather than
 * silently returning a partial buffer to the caller.  Reads are NOT
 * gated by allow_unsafe_write -- see the file-header safety-gate note.
 */
static alp_status_t y_read(alp_storage_backend_state_t *st, uint64_t offset, void *data, size_t len)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (!alp_storage_range_in_capacity(offset, (uint64_t)len, d->total_bytes)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	uint8_t *out  = (uint8_t *)data;
	size_t   done = 0;
	while (done < len) {
		ssize_t r = pread(d->fd, out + done, len - done, (off_t)(offset + done));
		if (r < 0) {
			if (errno == EINTR) continue;
			return alp_status_from_posix_errno(errno);
		}
		if (r == 0) return ALP_ERR_IO; /* EOF before len bytes -- see doxygen above */
		done += (size_t)r;
	}
	return ALP_OK;
}

/**
 * @brief pwrite() the requested range, looping over short writes.
 *
 * Refuses with ALP_ERR_INVAL before touching the fd unless
 * `allow_unsafe_write` was set at open() -- see the file-header
 * safety-gate note (issue #1140 review, blockers 1+2).  No
 * erase-before-write for MTD targets: this is a raw pwrite(), same as
 * bare-metal NOR -- see the file-header "Unsupported / partial ops"
 * note and <alp/storage.h>'s alp_storage_write() doxygen.
 */
static alp_status_t
y_write(alp_storage_backend_state_t *st, uint64_t offset, const void *data, size_t len)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (!d->allow_unsafe_write) return ALP_ERR_INVAL;
	if (!alp_storage_range_in_capacity(offset, (uint64_t)len, d->total_bytes)) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	const uint8_t *in   = (const uint8_t *)data;
	size_t         done = 0;
	while (done < len) {
		ssize_t w = pwrite(d->fd, in + done, len - done, (off_t)(offset + done));
		if (w < 0) {
			if (errno == EINTR) continue;
			return alp_status_from_posix_errno(errno);
		}
		if (w == 0) return ALP_ERR_IO;
		done += (size_t)w;
	}
	return ALP_OK;
}

/**
 * @brief Erase [@p offset, @p offset + @p len).
 *
 * Refuses with ALP_ERR_INVAL before touching the fd unless
 * `allow_unsafe_write` was set at open() -- see the file-header
 * safety-gate note.
 *
 * MTD: MEMERASE -- a real, synchronous NOR-style erase (the ioctl
 * blocks until the erase completes).  MEMERASE's start/length fields
 * are 32-bit; a range whose offset or length exceeds UINT32_MAX is
 * rejected outright rather than silently truncated (a truncated
 * length of exactly 0 would erase nothing and still report ALP_OK).
 *
 * SD/MMC: no NOR-style erase exists on the generic block layer.
 * BLKDISCARD is the closest real primitive -- a best-effort TRIM hint
 * that the range is no longer needed.  It is NOT guaranteed to read
 * back as zeroed (unlike a true NOR erase); a card/controller that
 * doesn't support it surfaces ENOTTY/EOPNOTSUPP, which
 * alp_status_from_posix_errno maps to ALP_ERR_NOSUPPORT -- an honest
 * signal instead of a silent no-op.  erase_size for this path is the
 * device's real discard granularity (_block_discard_granularity()),
 * not the logical sector size, so an aligned request here is actually
 * accepted by BLKDISCARD.
 */
static alp_status_t y_erase(alp_storage_backend_state_t *st, uint64_t offset, uint64_t len)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (!d->allow_unsafe_write) return ALP_ERR_INVAL;
	if (d->erase_size == 0u) return ALP_ERR_NOSUPPORT;
	if ((offset % d->erase_size) != 0u || (len % d->erase_size) != 0u) return ALP_ERR_INVAL;
	if (!alp_storage_range_in_capacity(offset, len, d->total_bytes)) return ALP_ERR_OUT_OF_RANGE;

	if (d->devtype == Y_STORAGE_DEV_MTD) {
		if (offset > (uint64_t)UINT32_MAX || len > (uint64_t)UINT32_MAX) {
			return ALP_ERR_OUT_OF_RANGE;
		}
		struct erase_info_user ei = { .start = (uint32_t)offset, .length = (uint32_t)len };
		if (ioctl(d->fd, MEMERASE, &ei) < 0) return alp_status_from_posix_errno(errno);
		return ALP_OK;
	}

	uint64_t range[2] = { offset, len };
	if (ioctl(d->fd, BLKDISCARD, range) < 0) return alp_status_from_posix_errno(errno);
	return ALP_OK;
}

static alp_status_t y_sync(alp_storage_backend_state_t *st)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	if (fsync(d->fd) < 0) return alp_status_from_posix_errno(errno);
	return ALP_OK;
}

/** @brief NOSUPPORT -- see the file-header "Unsupported / partial ops" note. */
static alp_status_t y_configure_inline_aes(alp_storage_backend_state_t    *st,
                                           const alp_storage_aes_config_t *cfg)
{
	(void)st;
	(void)cfg;
	return ALP_ERR_NOSUPPORT;
}

/** @brief Close the fd and free the per-handle box. */
static void y_close(alp_storage_backend_state_t *st)
{
	y_storage_data_t *d = (y_storage_data_t *)st->be_data;
	if (d != NULL) {
		if (d->fd >= 0) close(d->fd);
		free(d);
		st->be_data = NULL;
	}
}

static const alp_storage_ops_t _ops = {
	.open                 = y_open,
	.get_info             = y_get_info,
	.read                 = y_read,
	.write                = y_write,
	.erase                = y_erase,
	.sync                 = y_sync,
	.configure_inline_aes = y_configure_inline_aes,
	.close                = y_close,
};

ALP_BACKEND_REGISTER(storage,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
