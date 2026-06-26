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

## SP3 bake

**Machine conf change (committed):** `e1m-aen801-a32.conf` `IMAGE_INSTALL:append` extended
to include ` alp-remoteproc` (public-safe; the systemd unit no-ops when no firmware is
present).  `aen-m55-hp-fw` intentionally NOT committed to `IMAGE_INSTALL` — the prebuilt
ELF is internal/not redistributed.

**Local auto.conf overrides (NOT committed, local bake only):**
```
SKIP_RECIPE[aen-m55-hp-fw] = ""
IMAGE_INSTALL:append = " aen-m55-hp-fw"
IMAGE_INSTALL:append = " alp-remoteproc"
```
The local `files/m55_hp.elf` (untracked, gitignored) supplies the firmware.

**Image bake:** `alif-tiny-image` — 2407 tasks, all succeeded (82 rebuilt, 2325 from
sstate).  Exit code 0.

**Manifest assertion:**
```
aen-m55-hp-fw cortexa32hf-neon 0.6-r0
alp-remoteproc cortexa32hf-neon 0.6-r0
```
Found in `alif-tiny-image-e1m-aen801-a32.rootfs.manifest`.

**DTB assertion** (`dtc -I dtb -O dts e1m-aen801-evk.dtb`):
```
reserved-memory { #address-cells = <0x01>; #size-cells = <0x01>; ...
    alp_default_rpmsg@23c0000 {
        compatible = "shared-dma-pool";
        reg = <0x23c0000 0x40000>;
        no-map; label = "alp_default_rpmsg"; }
arm,mhuv2-tx  mbox-name = "arm-m55_hp-mhu0_tx"  (+ mhu0_rx enabled)
remoteproc-m55-hp {
    compatible = "alif,ensemble-m55-rproc";
    mboxes = <&mbox_m55_hp_mhu0_tx 0 0>, <&mbox_m55_hp_mhu0_rx 0 0>;
    firmware-name = "alp/E1M-AEN801/m55_hp.elf"; status = "okay"; }
```

**dtsi fix (committed in SP3):** `aen801-dts-reservations.dtsi` was generated with
`#address-cells = <2>` (64-bit) which caused DTC warnings on the 32-bit E8 SoC (the base
`reserved-memory` uses `#address-cells = <1>`).  Fixed to remove the override and use
32-bit `reg = <0x023c0000 0x00040000>`.  The mtime-drift issue (Windows→WSL) caused the
kernel to be compiled with the old DTS from sstate; the DTB was rebuilt in-tree via
`make dtbs` after the fix, confirmed clean (no DTC warnings), then deployed.  A clean
bake from scratch (after touching the DTS files) will produce the same DTB.
