# AEN A32 BSP grounding notes — 2026-06-25

Branch: `feat/aen-a32-yocto-bringup`

## SP2 bake

**Recipe built:** `aen-a32-carrier-bringup-0.6.0-r0` — 920 tasks, all succeeded.

**Setup notes for the standing WSL build tree:**
- `meta-alp-sdk` was not yet in `bblayers.conf` → added via `bitbake-layers add-layer`.
- `BBMASK` / `BB_DANGLINGAPPENDS_WARNONLY` were absent from `auto.conf` → appended.
- `alp-sdk` recipe fetches from `main`; commit `017dff35` (audio CMakeLists fix) is only
  on the feature branch → overrode `SRC_URI:pn-alp-sdk` + `SRCREV:pn-alp-sdk` in
  `auto.conf` to pin to `e848f7c0` on `feat/aen-a32-yocto-bringup`.  This override must
  be reverted (or `main` updated) once the branch merges.
- `IMAGE_INSTALL:append` from `e1m-aen801-a32.conf` was stale-cached on the WSL mtime
  boundary → added `IMAGE_INSTALL:append = " aen-a32-carrier-bringup"` directly to
  `auto.conf` to force inclusion; root cause is Windows→WSL mtime drift suppressing
  BitBake's conf-file invalidation.

**ELF arch assertion:**
```
.../aen-a32-carrier-bringup/0.6.0/packages-split/aen-a32-carrier-bringup/usr/bin/aen-a32-carrier-bringup:
    ELF 32-bit LSB pie executable, ARM, EABI5 version 1 (SYSV),
    dynamically linked, interpreter /lib/ld-musl-armhf.so.1,
    BuildID[sha1]=28eb2459e6a2a59b1f67413ba22aa33adb345f68, stripped
```

Target arch: `cortexa32hf-neon`, musl libc — confirmed cortexa32 + musl.

**Manifest assertion:**
```
aen-a32-carrier-bringup cortexa32hf-neon 0.6.0-r0
```
Found in `alif-tiny-image-e1m-aen801-a32.rootfs.manifest`.

**Image bake:** `alif-tiny-image` — 2373 tasks, all succeeded.
