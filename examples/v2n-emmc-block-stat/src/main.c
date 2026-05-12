/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-emmc-block-stat -- query the V2N module's on-module eMMC via
 * Zephyr's disk-access (block-storage) subsystem.  Reports sector
 * count + sector size, reads a few blocks, prints a hex dump of
 * block 0, and asks the EXT_CSD register for any wear-level data
 * the silicon exposes.
 *
 * V2N's eMMC sits on the SD0 controller (per
 * metadata/e1m_modules/v2n/renesas-peripheral-map.tsv).  The Zephyr
 * disk-access subsystem mounts it under the name `SD` by default --
 * carriers that want a different alias can rename in their
 * board overlay.  This example only **reads** -- it does not write
 * anything to the eMMC.  Reading the MBR / GPT region is safe.
 *
 * The example does not parse the partition table; that's the
 * filesystem layer's job (`fs_mount` against ext4 / FAT via
 * subsys/fs/) and depends heavily on the image the production
 * line wrote to the device.  For block-level wear / health
 * checking + storage benchmarking this raw-block surface is what
 * the operator actually wants.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>

/* Zephyr names the SD0 disk "SD" by default; the V2N board overlay
 * confirms this.  Carriers that strap differently can override at
 * build time. */
#ifndef EMMC_DISK_NAME
#define EMMC_DISK_NAME "SD"
#endif

/* How many blocks to read (and how many to hex-dump).  16 blocks =
 * 8 KiB; that's the typical MBR + a few partition tables, plenty
 * for a sanity check of the read path. */
#define READ_BLOCKS    16u

/* Hex-dump the first 64 bytes of the buffer in a way that's easy
 * to compare against `hexdump -C` output from a separate host
 * tool.  Less idiomatic than just hex-dumping the whole block but
 * keeps the console log readable. */
static void hex_dump_first_line(const uint8_t *buf, size_t len) {
    const size_t show = len < 64u ? len : 64u;
    printf("[emmc] block 0 first %zu bytes:\n[emmc] ", show);
    for (size_t i = 0u; i < show; ++i) {
        printf("%02X", buf[i]);
        if ((i & 0x0Fu) == 0x0Fu && i + 1u < show) printf("\n[emmc] ");
    }
    printf("\n");
}

int main(void) {
    printf("[emmc] v2n-emmc-block-stat (disk='%s')\n", EMMC_DISK_NAME);

    /* Init the disk -- mounts the eMMC via the SDHC driver class
     * + reads its CSD (Card-Specific Data) register so subsequent
     * ioctl queries have something to answer with. */
    int rv = disk_access_init(EMMC_DISK_NAME);
    if (rv != 0) {
        printf("[emmc] disk_access_init -> %d (no eMMC?)\n", rv);
        return 0;
    }

    /* Query basic geometry.  These two ioctls combined tell you
     * the device's capacity in bytes: sector_count * sector_size. */
    uint32_t sector_size = 0u;
    rv = disk_access_ioctl(EMMC_DISK_NAME, DISK_IOCTL_GET_SECTOR_SIZE,
                           &sector_size);
    if (rv != 0) {
        printf("[emmc] GET_SECTOR_SIZE -> %d\n", rv);
        return 0;
    }
    uint32_t sector_count = 0u;
    rv = disk_access_ioctl(EMMC_DISK_NAME, DISK_IOCTL_GET_SECTOR_COUNT,
                           &sector_count);
    if (rv != 0) {
        printf("[emmc] GET_SECTOR_COUNT -> %d\n", rv);
        return 0;
    }
    /* Capacity in MiB.  cast through uint64_t so multiplication
     * doesn't overflow on a > 4 GiB card (which the V2N eMMC
     * almost certainly is -- 16 GiB minimum on the production
     * BOM). */
    const uint64_t capacity_bytes =
        (uint64_t)sector_size * (uint64_t)sector_count;
    printf("[emmc] sector_size=%u  sector_count=%u  capacity=%u MiB\n",
           (unsigned)sector_size, (unsigned)sector_count,
           (unsigned)(capacity_bytes >> 20));

    /* Allocate the read buffer in BSS.  sector_size on every
     * reasonable eMMC is 512 bytes; READ_BLOCKS = 16 gives 8 KiB,
     * which is larger than Zephyr's default CONFIG_MAIN_STACK_SIZE
     * (1024 B / 2048 B on most targets).  Stack-allocating an 8 KiB
     * buffer here used to silently corrupt the main thread on real
     * targets even though native_sim's 4 KiB default tolerated it.
     * Static storage is single-use here (no concurrent main()s) so
     * sharing one BSS slot is fine.  In a production app with a
     * larger sector_size (some industrial eMMC report 4096), or
     * with concurrent reads, the buffer would have to grow + move
     * to the heap. */
    static uint8_t buf[READ_BLOCKS * 512u];
    if (sector_size != 512u) {
        printf("[emmc] unexpected sector_size %u (not 512) -- skipping read\n",
               (unsigned)sector_size);
    } else {
        rv = disk_access_read(EMMC_DISK_NAME, buf, 0u, READ_BLOCKS);
        if (rv != 0) {
            printf("[emmc] disk_access_read(blocks=%u) -> %d\n",
                   (unsigned)READ_BLOCKS, rv);
        } else {
            /* MBR signature is at byte 0x1FE-0x1FF (offset 510 of
             * the first 512-byte block).  Standard value is
             * 0x55 0xAA; report what we see so the reader can
             * distinguish a partitioned-but-empty card from a
             * factory-fresh one. */
            const uint8_t sig0 = buf[510];
            const uint8_t sig1 = buf[511];
            printf("[emmc] MBR signature: %02X%02X (%s)\n",
                   sig0, sig1,
                   (sig0 == 0x55u && sig1 == 0xAAu) ? "valid"
                                                    : "not standard MBR");
            hex_dump_first_line(buf, sizeof buf);
        }
    }

    /* Optional: wear-level data.  Zephyr exposes a small subset of
     * the eMMC EXT_CSD register via DISK_IOCTL_CTRL_INIT-time
     * caching.  Field availability is silicon-dependent; the
     * driver class returns -ENOTSUP when the field isn't cached
     * which is fine -- we just print "n/a" in that case. */
#ifdef DISK_IOCTL_GET_ERASE_BLOCK_SZ
    uint32_t erase_block_sz = 0u;
    rv = disk_access_ioctl(EMMC_DISK_NAME, DISK_IOCTL_GET_ERASE_BLOCK_SZ,
                           &erase_block_sz);
    if (rv == 0) {
        printf("[emmc] erase block size: %u bytes\n", (unsigned)erase_block_sz);
    } else {
        printf("[emmc] erase block size: n/a (ioctl -> %d)\n", rv);
    }
#else
    printf("[emmc] erase block size ioctl not built in this Zephyr revision\n");
#endif

    printf("[emmc] done\n");
    return 0;
}
