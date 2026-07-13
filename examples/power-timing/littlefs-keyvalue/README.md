# littlefs-keyvalue

Mounts a littlefs partition, writes a single key/value pair to a
file, reads it back, lists the mount-point directory, and asserts the
read matches the write. Teaches the Zephyr `fs` subsystem's
`fs_mount`/`fs_open`/`fs_write`/`fs_read`/`fs_readdir` API surface
using the littlefs backend (`libraries: [littlefs]` in `board.yaml`).

**[UNTESTED]: the littlefs code path itself.** `board.yaml` selects
`libraries: [littlefs]`, and `west.yml` lists `littlefs` in the Zephyr
name-allowlist (upstream `modules/fs/littlefs`, providing `lfs.c` /
`lfs.h`) -- but this build machine's west workspace never ran `west
update` after that allowlist entry landed, so the module isn't checked
out. Zephyr's `FILE_SYSTEM_LITTLEFS` Kconfig symbol `depends on
ZEPHYR_LITTLEFS_MODULE` and silently demotes to `n` when the module is
missing, so `src/main.c` guards the real fs_mount/fs_open/fs_read/
fs_write/fs_readdir demonstration behind `#ifdef
CONFIG_FILE_SYSTEM_LITTLEFS` and falls back to an explanatory message
in the `#else` branch. **What ships here is real, correct API usage
that is currently untested on this machine** -- run `west update` in
the workspace topdir to fetch `modules/fs/littlefs`, rebuild, and the
`#ifdef` branch (the actual mount/write/read/list/assert sequence)
takes over with no source change required. Verified PASS on native_sim
today is the `#else` fallback branch only.

## What this shows

* `board.yaml`'s `libraries: [littlefs]` -> the loader emits
  `CONFIG_FILE_SYSTEM_LITTLEFS=y` + `CONFIG_FILE_SYSTEM=y` +
  `CONFIG_ALP_LITTLEFS_SYNC_IO=y` (see `_LIBRARY_KCONFIG` in
  `scripts/alp_project.py`). `CONFIG_FLASH` + `CONFIG_FLASH_MAP` are
  storage-medium plumbing, not library selection, so they're set in
  this example's own `prj.conf` instead.
* `FS_LITTLEFS_DECLARE_DEFAULT_CONFIG()` + a `struct fs_mount_t`
  naming a DTS fixed-partition label via `PARTITION_ID()`.
* `fs_mount()` / `fs_file_t_init()` / `fs_open()` / `fs_write()` /
  `fs_seek()` / `fs_read()` / `fs_close()` / `fs_unmount()`.
* `fs_opendir()` / `fs_readdir()` / `fs_closedir()` -- littlefs marks
  end-of-directory with an empty `entry.name`, not a distinguished
  error code.
* Asserting a read-after-write round-trip byte-for-byte.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/power-timing/littlefs-keyvalue \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Expected output

With `modules/fs/littlefs` fetched (`CONFIG_FILE_SYSTEM_LITTLEFS=y`):

```
[littlefs-keyvalue] mounting /lfs ...
[littlefs-keyvalue] mount rc=0
[littlefs-keyvalue] wrote "answer=42" (9 bytes)
[littlefs-keyvalue] read  "answer=42" (9 bytes)
[littlefs-keyvalue] dir /lfs:
[littlefs-keyvalue]   [FILE] kv.txt (size = 9)
[littlefs-keyvalue] read matches write -- OK
[littlefs-keyvalue] done
```

Without it (this machine's current state -- the `#else` fallback in
`src/main.c`):

```
[littlefs-keyvalue] CONFIG_FILE_SYSTEM_LITTLEFS=n in this build --
[littlefs-keyvalue] the littlefs west module isn't fetched (run
[littlefs-keyvalue] `west update` to pull modules/fs/littlefs); the
[littlefs-keyvalue] fs_mount/fs_open/fs_read/fs_write/fs_readdir code
[littlefs-keyvalue] above this #else is the real, correct API usage.
[littlefs-keyvalue] done
```

## Hardware swap

Swap the storage medium behind the `storage_partition` DTS label:

| SoM family | Storage medium                    | HW-accelerated Kconfig knob   |
|------------|------------------------------------|-------------------------------|
| E1M-AEN    | Alif on-die MRAM over xSPI DMA    | `CONFIG_ALP_LITTLEFS_XSPI_DMA` |
| E1M-V2N(-M1) | eMMC over DMA                   | `CONFIG_ALP_LITTLEFS_EMMC_DMA` |
| any        | Sync I/O (this example's default) | `CONFIG_ALP_LITTLEFS_SYNC_IO` |

No change to `src/main.c` is required -- only the board's DTS
partition map changes what `storage_partition` physically backs onto.

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
- Zephyr's own littlefs sample: `zephyr/samples/subsys/fs/littlefs`
  (this example is a minimal, single-file-focused subset of it).
